#include "world/WorldExtractor.h"

namespace en {

void WorldExtractor::Extract(
    const World& world,
    const AssetBank& assetBank,
    const RenderTimeView& timeView,
    RenderList& outRenderList,
    f32 defaultPixelsPerMeter) noexcept {
    // 1. Extract active CameraRig2D views via direct dense pool iteration
    bool foundCamera = false;
    if ((world.RegisteredPoolsMask() & (1ULL << CameraRig2D::kTypeId)) != 0ULL) {
        const auto& camPool = world.GetPool<CameraRig2D>();
        const auto camEntities = camPool.Entities();
        const auto camRigs = camPool.Components();
        const f32 camAlpha = timeView.Alpha(TimeChannel::Camera);

        for (usize i = 0U; i < camEntities.size(); ++i) {
            const CameraRig2D& rig = camRigs[i];
            if (!rig.active) continue;
            const u16 idx = camEntities[i].index;
            const WorldTransform2D& wt = world.GetWorldByIndex(idx);
            const PrevWorldSnapshot2D& prev = world.GetPrevWorldByIndex(idx);

            Camera2D cam{};
            cam.viewport = rig.viewport;
            cam.worldCenterX = prev.x + (wt.x - prev.x) * camAlpha;
            cam.worldCenterY = prev.y + (wt.y - prev.y) * camAlpha;
            cam.zoom = rig.zoom;
            cam.pixelsPerMeter = (rig.pixelsPerMeter > 0.0f) ? rig.pixelsPerMeter : defaultPixelsPerMeter;
            cam.screenSpace = rig.screenSpace;
            if (rig.trauma > 0.0f) {
                cam.ApplyTraumaShake(rig.trauma, static_cast<f32>(timeView.tickIndex) * 0.05f, 0.5f);
            }
            outRenderList.SetView(rig.viewSlot, cam);
            foundCamera = true;
        }
    }

    if (!foundCamera) {
        Camera2D defaultCam{};
        defaultCam.pixelsPerMeter = defaultPixelsPerMeter;
        outRenderList.SetView(0U, defaultCam);
    }

    const std::span<const u64> movedWords = world.MovedWords();

    // 2. Extract interpolated world Sprite2D entities (stationary fast-path bypasses PrevWorldSnapshot2D & TimeChannel loads)
    if ((world.RegisteredPoolsMask() & (1ULL << Sprite2D::kTypeId)) != 0ULL) {
        const auto& spritePool = world.GetPool<Sprite2D>();
        const auto& animPool = world.GetPool<SpriteAnimator2D>();
        const auto spriteEntities = spritePool.Entities();
        const auto sprites = spritePool.Components();

        for (usize i = 0U; i < spriteEntities.size(); ++i) {
            const Sprite2D& sp = sprites[i];
            if (!sp.Visible()) continue;

            const u16 idx = spriteEntities[i].index;
            const WorldTransform2D& wt = world.GetWorldByIndex(idx);
            f32 rx = wt.x;
            f32 ry = wt.y;
            f32 sx = wt.scaleX;
            f32 sy = wt.scaleY;
            f32 c = wt.cosRot;
            f32 s = wt.sinRot;

            const bool moved = (movedWords[idx >> 6U] & (1ULL << (idx & 63U))) != 0ULL;
            if (moved) {
                const PrevWorldSnapshot2D& pv = world.GetPrevWorldByIndex(idx);
                const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(idx));
                const f32 alpha = timeView.Alpha(ch);
                rx = pv.x + (wt.x - pv.x) * alpha;
                ry = pv.y + (wt.y - pv.y) * alpha;
                sx = pv.scaleX + (wt.scaleX - pv.scaleX) * alpha;
                sy = pv.scaleY + (wt.scaleY - pv.scaleY) * alpha;
                if (wt.worldRotRad != pv.worldRotRad) {
                    f32 dRot = wt.worldRotRad - pv.worldRotRad;
                    dRot -= std::floor((dRot + kPi) * kInvTwoPi) * kTwoPi;
                    FastSinCos(pv.worldRotRad + dRot * alpha, c, s);
                }
            }

            // 1-hop sibling lookup via GetComponentMaskByIndex + DirectAtEntityIndex (zero TryGet overhead)
            const u64 compMask = world.GetComponentMaskByIndex(idx);
            const SpriteFrame* framePtr = &assetBank.GetSprite(sp.sprite);
            if ((compMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
                const SpriteAnimator2D& anim = animPool.DirectAtEntityIndex(idx);
                if (anim.atlas.IsValid()) {
                    framePtr = &assetBank.GetAtlasFrame(
                        anim.atlas, static_cast<u16>(anim.firstFrame + anim.currentFrame));
                }
            }

            QuadCmd cmd{};
            cmd.center = {rx, ry};
            cmd.size = {sp.width * sx, sp.height * sy};
            cmd.pivot = framePtr->Pivot();
            cmd.cosRot = c;
            cmd.sinRot = s;
            cmd.uv = framePtr->uv;
            cmd.color = sp.tint;
            cmd.texture = framePtr->gpuTexture;
            cmd.depth = sp.YSort() ? SortKey::EncodeYDepth(ry) : sp.baseDepth;
            cmd.layer = sp.layer;
            cmd.blend = sp.Blend();
            cmd.viewSlot = sp.ViewSlot();
            cmd.flipX = sp.FlipX();
            cmd.flipY = sp.FlipY();
            outRenderList.PushQuad(cmd);
        }
    }

    // 3. Extract dynamic PointLight2D additive halos (interpolated on moving entities)
    if ((world.RegisteredPoolsMask() & (1ULL << PointLight2D::kTypeId)) != 0ULL) {
        const auto& lightPool = world.GetPool<PointLight2D>();
        const auto lightEntities = lightPool.Entities();
        const auto lights = lightPool.Components();

        for (usize i = 0U; i < lightEntities.size(); ++i) {
            const PointLight2D& light = lights[i];
            if (light.intensity <= 0.0f || light.radius <= 0.0f) continue;
            const u16 idx = lightEntities[i].index;
            const WorldTransform2D& wt = world.GetWorldByIndex(idx);
            f32 lx = wt.x;
            f32 ly = wt.y;
            if ((movedWords[idx >> 6U] & (1ULL << (idx & 63U))) != 0ULL) {
                const PrevWorldSnapshot2D& pv = world.GetPrevWorldByIndex(idx);
                const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(idx));
                const f32 alpha = timeView.Alpha(ch);
                lx = pv.x + (wt.x - pv.x) * alpha;
                ly = pv.y + (wt.y - pv.y) * alpha;
            }

            f32 pulse = 1.0f;
            if (light.pulseSpeed != 0.0f) {
                f32 c = 1.0f;
                f32 s = 0.0f;
                FastSinCos(light.pulsePhase, c, s);
                pulse = 0.85f + 0.15f * s;
            }
            const f32 diameter = light.radius * pulse * 2.0f;
            const f32 intensity = light.intensity * pulse;

            QuadCmd halo{};
            halo.center = {lx, ly};
            halo.size = {diameter, diameter};
            halo.color = Color{
                light.color.r * intensity,
                light.color.g * intensity,
                light.color.b * intensity,
                light.color.a * intensity,
            };
            halo.texture = kBuiltinSoftLightTexture;
            halo.layer = 200U;
            halo.blend = BlendMode::Additive;
            outRenderList.PushQuad(halo);
        }
    }
}

Result<EntityId, WorldErrc> WorldExtractor::InstantiatePrefab(
    World& world,
    const AssetBank& assetBank,
    BlobHandle prefabBlob,
    const LocalTransform2D& rootTransform,
    std::span<EntityId> outSpawnedEntities) noexcept {
    return world.InstantiatePrefab(assetBank, prefabBlob, rootTransform, outSpawnedEntities);
}

} // namespace en
