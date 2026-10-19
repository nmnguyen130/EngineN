#include "world/WorldExtractor.h"
#include "renderer/RenderList.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace en::world {

void WorldExtractor::Extract(
    const World& world,
    const assets::AssetBank& assetBank,
    const time::RenderTimeView& timeView,
    renderer::RenderList& outRenderList) noexcept {

    const auto* __restrict currSpan   = world.WorldTransforms().data();
    const auto* __restrict prevSpan   = world.PrevWorldTransforms().data();
    const auto* __restrict chSpan     = world.TimeChannels().data();
    const auto* __restrict movedWords = world.MovedWords().data();

    // 1. Extract active CameraRig2D views into RenderList
    const auto& camPool    = world.GetPool<CameraRig2D>();
    const auto camEntities = camPool.Entities();
    const auto camRigs     = camPool.Components();

    for (usize i = 0; i < camEntities.size(); ++i) {
        const CameraRig2D& rig = camRigs[i];
        if (!rig.active) {
            continue;
        }
        const u16 idx                = camEntities[i].index;
        const WorldTransform2D& curr = currSpan[idx];
        f32 camX = curr.x;
        f32 camY = curr.y;

        if ((movedWords[idx >> 6] & (1ULL << (idx & 63u))) != 0) {
            const f32 alpha = timeView.Alpha(chSpan[idx]);
            if (alpha < 0.9999f) {
                const PrevWorldSnapshot2D& prev = prevSpan[idx];
                camX = prev.x + alpha * (curr.x - prev.x);
                camY = prev.y + alpha * (curr.y - prev.y);
            }
        }

        outRenderList.SetView(rig.viewSlot, renderer::Camera2D{
            .worldCenterX = camX,
            .worldCenterY = camY,
            .zoom         = rig.zoom,
            .viewport     = rig.viewport,
            .scissor      = rig.scissor,
            .screenSpace  = rig.screenSpace,
        });
    }

    // 2. Linearly sweep dense Sprite2D array with stationary fast path & direct animator resolution
    const auto& spritePool = world.GetPool<Sprite2D>();
    const auto& animPool   = world.GetPool<SpriteAnimator2D>();
    const usize spriteCount = spritePool.Size();
    const auto* __restrict spriteEntities = spritePool.Entities().data();
    const auto* __restrict sprites        = spritePool.Components().data();
    constexpr ComponentMask kAnimatorBit  = ComponentMask{1} << SpriteAnimator2D::kTypeId;

    for (usize i = 0; i < spriteCount; ++i) {
        const Sprite2D& sp = sprites[i];
        if ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::Hidden)) != 0) [[unlikely]] {
            continue;
        }

        const u16 idx              = spriteEntities[i].index;
        const WorldTransform2D& t1 = currSpan[idx];

        f32 wx   = t1.x;
        f32 wy   = t1.y;
        f32 sx   = t1.scaleX;
        f32 sy   = t1.scaleY;
        f32 wd   = t1.worldDepth;
        f32 cosR = t1.cosRot;
        f32 sinR = t1.sinRot;

        // Stationary sprites (bit == 0 in movedWords) bypass prevSpan and chSpan cache misses completely.
        if ((movedWords[idx >> 6] & (1ULL << (idx & 63u))) != 0) {
            const f32 alpha = timeView.Alpha(chSpan[idx]);
            if (alpha < 0.9999f) {
                const PrevWorldSnapshot2D& t0 = prevSpan[idx];
                wx = t0.x + alpha * (t1.x - t0.x);
                wy = t0.y + alpha * (t1.y - t0.y);
                sx = t0.scaleX + alpha * (t1.scaleX - t0.scaleX);
                sy = t0.scaleY + alpha * (t1.scaleY - t0.scaleY);
                wd = t0.worldDepth + alpha * (t1.worldDepth - t0.worldDepth);

                if (t0.worldRotRad != t1.worldRotRad) [[unlikely]] {
                    const f32 lerpedAngle = t0.worldRotRad + alpha * ShortestAngleDelta(t0.worldRotRad, t1.worldRotRad);
                    FastSinCos(lerpedAngle, cosR, sinR);
                }
            }
        }

        const bool hasAnimator = (world.GetComponentMaskByIndex(idx) & kAnimatorBit) != 0;
        const assets::SpriteFrame& frame = hasAnimator
            ? [&]() noexcept -> const assets::SpriteFrame& {
                const SpriteAnimator2D& anim = animPool.DirectAtEntityIndex(idx);
                return anim.atlas.IsValid()
                    ? assetBank.GetAtlasFrame(anim.atlas, static_cast<u16>(anim.firstFrame + anim.currentFrame))
                    : assetBank.GetSprite(sp.sprite);
            }()
            : assetBank.GetSprite(sp.sprite);

        const f32 quadW = static_cast<f32>(frame.width) * std::abs(sx);
        const f32 quadH = static_cast<f32>(frame.height) * std::abs(sy);

        const bool flipX = ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::FlipX)) != 0) ^ (sx < 0.0f);
        const bool flipY = ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::FlipY)) != 0) ^ (sy < 0.0f);

        const f32 pivX      = flipX ? (1.0f - frame.pivotX) : frame.pivotX;
        const f32 pivY      = flipY ? (1.0f - frame.pivotY) : frame.pivotY;
        const f32 flipSignX = flipX ? -1.0f : 1.0f;
        const f32 flipSignY = flipY ? -1.0f : 1.0f;

        const f32 offsetX  = (0.5f - pivX) * quadW + static_cast<f32>(sp.localOffsetX) * flipSignX;
        const f32 offsetY  = (0.5f - pivY) * quadH + static_cast<f32>(sp.localOffsetY) * flipSignY;
        const f32 rawDepth = static_cast<f32>(sp.baseDepth) + wd + wy * static_cast<f32>(sp.ySortScale);

        outRenderList.PushQuad(renderer::QuadCmd{
            .x        = wx + (offsetX * cosR - offsetY * sinR),
            .y        = wy + (offsetX * sinR + offsetY * cosR),
            .w        = quadW,
            .h        = quadH,
            .cosRot   = cosR,
            .sinRot   = sinR,
            .uv       = frame.uv,
            .color    = sp.tint,
            .texture  = frame.gpuTexture,
            .depth    = static_cast<u16>(std::clamp(rawDepth, 0.0f, 65535.0f)),
            .passId   = sp.passId,
            .layer    = sp.layer,
            .viewSlot = sp.viewSlot,
            .blend    = static_cast<renderer::BlendMode>((sp.blendAndFlags >> 3) & 0x03u),
            .flipX    = flipX,
            .flipY    = flipY,
        });
    }
}

Result<EntityId, WorldErrc> WorldExtractor::InstantiatePrefab(
    World& world,
    const assets::AssetBank& assetBank,
    assets::BlobHandle prefabBlob,
    const LocalTransform2D& rootTransform,
    std::span<EntityId> outSpawnedEntities) noexcept {

    const assets::BlobRecord& blob = assetBank.GetBlob(prefabBlob);
    if (blob.data == nullptr || blob.size < sizeof(CookedPrefabHeader)) [[unlikely]] {
        return Err(WorldErrc::CorruptedPrefabBlob);
    }

    const auto* hdr = reinterpret_cast<const CookedPrefabHeader*>(blob.data);
    const usize expectedBytes = sizeof(CookedPrefabHeader) +
                                static_cast<usize>(hdr->entityCount) * sizeof(CookedPrefabEntityRecord);
    if (hdr->magic != CookedPrefabHeader::kMagic || hdr->entityCount == 0 || blob.size < expectedBytes) [[unlikely]] {
        return Err(WorldErrc::CorruptedPrefabBlob);
    }

    const auto* records = reinterpret_cast<const CookedPrefabEntityRecord*>(blob.data + sizeof(CookedPrefabHeader));
    std::array<EntityId, 64> localStackIds{};
    const u16 count = std::min<u16>(hdr->entityCount, static_cast<u16>(localStackIds.size()));

    for (u16 i = 0; i < count; ++i) {
        const CookedPrefabEntityRecord& rec = records[i];
        const EntitySpawnDesc desc{
            .localTransform  = (i == 0) ? rootTransform : rec.localTransform,
            .timeChannel     = static_cast<time::TimeChannel>(rec.timeChannel & 0x07u),
            .parent          = (rec.parentLocalIndex < i) ? localStackIds[rec.parentLocalIndex] : kNullEntity,
            .hasSpatial      = true,
            .isStaticSpatial = (rec.flags & 0x04u) != 0,
        };

        auto spawned = world.Spawn(desc);
        if (!spawned) [[unlikely]] {
            return Err(spawned.error());
        }
        const EntityId entityId = *spawned;
        localStackIds[i] = entityId;
        if (i < outSpawnedEntities.size()) {
            outSpawnedEntities[i] = entityId;
        }

        if ((rec.flags & 0x03u) != 0) {
            const u16 atlasSlot = assetBank.FindSlot(rec.atlasOrTextureId);
            const assets::AtlasHandle resolvedAtlas = assetBank.ToAtlasHandle(atlasSlot);

            if ((rec.flags & 0x01u) != 0) {
                const assets::SpriteHandle initialSprite = assetBank.GetSpriteHandle(resolvedAtlas, rec.firstFrame);
                (void)world.Emplace<Sprite2D>(entityId, Sprite2D{
                    .tint       = rec.tint,
                    .sprite     = initialSprite,
                    .baseDepth  = rec.baseDepth,
                    .ySortScale = rec.ySortScale,
                    .passId     = rec.passId,
                    .layer      = rec.layer,
                });
            }
            if ((rec.flags & 0x02u) != 0) {
                (void)world.Emplace<SpriteAnimator2D>(entityId, SpriteAnimator2D{
                    .atlas         = resolvedAtlas,
                    .firstFrame    = rec.firstFrame,
                    .frameCount    = std::max<u16>(1u, rec.frameCount),
                    .currentFrame  = 0,
                    .ticksPerFrame = std::max<u16>(1u, rec.ticksPerFrame),
                    .subTickAccum  = 0,
                    .loop          = 1,
                    .playing       = 1,
                });
            }
        }
    }

    world.PropagateTransforms();
    return localStackIds[0];
}

} // namespace en::world
