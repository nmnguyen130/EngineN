#include "world/WorldExtractor.h"
#include "renderer/RendererList.h"

#include <algorithm>
#include <cmath>

namespace en::world {

void WorldExtractor::Extract(
    const World& world,
    const assets::AssetBank& assetBank,
    time::RenderTimeView timeView,
    renderer::RenderList& outRenderList) noexcept {

    // 1. Extract active CameraRig2D views into RenderList
    const auto& camPool     = world.GetPool<CameraRig2D>();
    const auto camEntities  = camPool.Entities();
    const auto camRigs      = camPool.Components();

    for (usize i = 0; i < camEntities.size(); ++i) {
        const CameraRig2D& rig = camRigs[i];
        if (!rig.active) {
            continue;
        }
        const u16 idx   = camEntities[i].index;
        const f32 alpha = timeView.Alpha(world.GetTimeChannelByIndex(idx));
        const PrevWorldSnapshot2D& prev = world.GetPrevWorldByIndex(idx);
        const WorldTransform2D& curr    = world.GetWorldByIndex(idx);

        renderer::Camera2D cam{};
        cam.worldCenterX = prev.x + alpha * (curr.x - prev.x);
        cam.worldCenterY = prev.y + alpha * (curr.y - prev.y);
        cam.zoom         = rig.zoom;
        cam.viewport     = rig.viewport;
        cam.scissor      = rig.scissor;
        cam.screenSpace  = rig.screenSpace;
        outRenderList.SetView(rig.viewSlot, cam);
    }

    // 2. Linearly sweep dense Sprite2D array with 1:1 direct spatial lookup
    const auto& spritePool     = world.GetPool<Sprite2D>();
    const auto spriteEntities  = spritePool.Entities();
    const auto sprites         = spritePool.Components();

    for (usize i = 0; i < spriteEntities.size(); ++i) {
        const Sprite2D& sp = sprites[i];
        if ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::Hidden)) != 0) [[unlikely]] {
            continue;
        }

        const u16 idx   = spriteEntities[i].index;
        const f32 alpha = timeView.Alpha(world.GetTimeChannelByIndex(idx));
        const PrevWorldSnapshot2D& t0 = world.GetPrevWorldByIndex(idx);
        const WorldTransform2D& t1    = world.GetWorldByIndex(idx);

        const f32 wx = t0.x + alpha * (t1.x - t0.x);
        const f32 wy = t0.y + alpha * (t1.y - t0.y);
        const f32 sx = t0.scaleX + alpha * (t1.scaleX - t0.scaleX);
        const f32 sy = t0.scaleY + alpha * (t1.scaleY - t0.scaleY);
        const f32 wd = t0.worldDepth + alpha * (t1.worldDepth - t0.worldDepth);

        f32 cosR = t1.cosRot;
        f32 sinR = t1.sinRot;
        if (alpha < 0.9999f && t0.worldRotRad != t1.worldRotRad) [[unlikely]] {
            const f32 lerpedAngle = t0.worldRotRad + alpha * ShortestAngleDelta(t0.worldRotRad, t1.worldRotRad);
            FastSinCos(lerpedAngle, cosR, sinR);
        }

        const assets::SpriteFrame& frame = assetBank.GetSprite(sp.sprite);
        const f32 quadW = static_cast<f32>(frame.width) * std::abs(sx);
        const f32 quadH = static_cast<f32>(frame.height) * std::abs(sy);

        const bool flipX = ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::FlipX)) != 0) ^ (sx < 0.0f);
        const bool flipY = ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::FlipY)) != 0) ^ (sy < 0.0f);

        const f32 pivX = flipX ? (1.0f - frame.pivotX) : frame.pivotX;
        const f32 pivY = flipY ? (1.0f - frame.pivotY) : frame.pivotY;

        const f32 offsetX = (0.5f - pivX) * quadW + static_cast<f32>(sp.localOffsetX);
        const f32 offsetY = (0.5f - pivY) * quadH + static_cast<f32>(sp.localOffsetY);
        const f32 rawDepth = static_cast<f32>(sp.baseDepth) + wd + wy * static_cast<f32>(sp.ySortScale);

        renderer::QuadCmd cmd{};
        cmd.x        = wx + (offsetX * cosR - offsetY * sinR);
        cmd.y        = wy + (offsetX * sinR + offsetY * cosR);
        cmd.w        = quadW;
        cmd.h        = quadH;
        cmd.cosRot   = cosR;
        cmd.sinRot   = sinR;
        cmd.uv       = frame.uv;
        cmd.color    = sp.tint;
        cmd.texture  = frame.gpuTexture;
        cmd.depth    = static_cast<u16>(std::clamp(rawDepth, 0.0f, 65535.0f));
        cmd.passId   = sp.passId;
        cmd.layer    = sp.layer;
        cmd.viewSlot = sp.viewSlot;
        cmd.blend    = static_cast<renderer::BlendMode>((sp.blendAndFlags >> 3) & 0x03u);
        cmd.flipX    = flipX;
        cmd.flipY    = flipY;

        outRenderList.PushQuad(cmd);
    }
}

Result<EntityId, WorldErrc> WorldExtractor::InstantiatePrefab(
    World& world,
    const assets::AssetBank& /*assetBank*/,
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
    std::array<EntityId, 256> localStackIds{};
    const u16 count = std::min<u16>(hdr->entityCount, 256u);

    for (u16 i = 0; i < count; ++i) {
        const CookedPrefabEntityRecord& rec = records[i];
        EntitySpawnDesc desc{};
        desc.localTransform  = (i == 0) ? rootTransform : rec.localTransform;
        desc.timeChannel     = static_cast<time::TimeChannel>(rec.timeChannel & 0x07u);
        desc.hasSpatial      = true;
        desc.isStaticSpatial = (rec.flags & 0x04u) != 0;
        if (rec.parentLocalIndex < i) {
            desc.parent = localStackIds[rec.parentLocalIndex];
        }

        auto spawned = world.Spawn(desc);
        if (!spawned) [[unlikely]] {
            return Err(spawned.error());
        }
        localStackIds[i] = *spawned;
        if (i < outSpawnedEntities.size()) {
            outSpawnedEntities[i] = *spawned;
        }

        if ((rec.flags & 0x01u) != 0) {
            Sprite2D sp{};
            sp.tint        = rec.tint;
            sp.baseDepth   = rec.baseDepth;
            sp.ySortScale  = rec.ySortScale;
            sp.passId      = rec.passId;
            sp.layer       = rec.layer;
            (void)world.Emplace<Sprite2D>(*spawned, sp);
        }
    }

    return localStackIds[0];
}

} // namespace en::world
