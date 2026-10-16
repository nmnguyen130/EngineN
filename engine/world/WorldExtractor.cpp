#include "world/WorldExtractor.h"
#include "renderer/RenderList.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace en::world {

void WorldExtractor::Extract(
    const World& world,
    const assets::AssetBank& assetBank,
    time::RenderTimeView timeView,
    renderer::RenderList& outRenderList) noexcept {

    // Precompute interpolation alphas across all 8 TimeChannels once per frame
    alignas(32) std::array<f32, 8> chAlphas{};
    for (u8 c = 0; c < 8; ++c) {
        chAlphas[c] = timeView.Alpha(static_cast<time::TimeChannel>(c));
    }

    const auto* __restrict currSpan = world.WorldTransforms().data();
    const auto* __restrict prevSpan = world.PrevWorldTransforms().data();
    const auto* __restrict chSpan   = world.TimeChannels().data();

    // 1. Extract active CameraRig2D views into RenderList
    const auto& camPool    = world.GetPool<CameraRig2D>();
    const auto camEntities = camPool.Entities();
    const auto camRigs     = camPool.Components();

    for (usize i = 0; i < camEntities.size(); ++i) {
        const CameraRig2D& rig = camRigs[i];
        if (!rig.active) {
            continue;
        }
        const u16 idx                   = camEntities[i].index;
        const f32 alpha                 = chAlphas[static_cast<u8>(chSpan[idx])];
        const PrevWorldSnapshot2D& prev = prevSpan[idx];
        const WorldTransform2D& curr    = currSpan[idx];

        outRenderList.SetView(rig.viewSlot, renderer::Camera2D{
            .worldCenterX = prev.x + alpha * (curr.x - prev.x),
            .worldCenterY = prev.y + alpha * (curr.y - prev.y),
            .zoom         = rig.zoom,
            .viewport     = rig.viewport,
            .scissor      = rig.scissor,
            .screenSpace  = rig.screenSpace,
        });
    }

    // 2. Linearly sweep dense Sprite2D array with direct pointer indexing
    const auto& spritePool    = world.GetPool<Sprite2D>();
    const auto spriteEntities = spritePool.Entities();
    const auto sprites        = spritePool.Components();

    for (usize i = 0; i < spriteEntities.size(); ++i) {
        const Sprite2D& sp = sprites[i];
        if ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::Hidden)) != 0) [[unlikely]] {
            continue;
        }

        const u16 idx                  = spriteEntities[i].index;
        const f32 alpha                = chAlphas[static_cast<u8>(chSpan[idx])];
        const PrevWorldSnapshot2D& t0  = prevSpan[idx];
        const WorldTransform2D& t1     = currSpan[idx];

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

        const assets::SpriteFrame& frame = assetBank.GetSprite(assets::SpriteHandle{
            .spriteIndex = sp.spriteIndex,
            .generation  = sp.spriteGeneration,
        });
        const f32 quadW = static_cast<f32>(frame.width) * std::abs(sx);
        const f32 quadH = static_cast<f32>(frame.height) * std::abs(sy);

        const bool flipX = ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::FlipX)) != 0) ^ (sx < 0.0f);
        const bool flipY = ((sp.blendAndFlags & static_cast<u8>(SpriteFlags::FlipY)) != 0) ^ (sy < 0.0f);

        const f32 pivX = flipX ? (1.0f - frame.pivotX) : frame.pivotX;
        const f32 pivY = flipY ? (1.0f - frame.pivotY) : frame.pivotY;

        const f32 offsetX  = (0.5f - pivX) * quadW + static_cast<f32>(sp.localOffsetX);
        const f32 offsetY  = (0.5f - pivY) * quadH + static_cast<f32>(sp.localOffsetY);
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
    std::array<EntityId, 256> localStackIds{};
    const u16 count = std::min<u16>(hdr->entityCount, 256u);

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

        if ((rec.flags & 0x01u) != 0) {
            (void)world.Emplace<Sprite2D>(entityId, Sprite2D{
                .tint       = rec.tint,
                .baseDepth  = rec.baseDepth,
                .ySortScale = rec.ySortScale,
                .passId     = rec.passId,
                .layer      = rec.layer,
            });
        }
        if ((rec.flags & 0x02u) != 0) {
            (void)world.Emplace<SpriteAnimator2D>(entityId, SpriteAnimator2D{
                .firstFrame    = rec.firstFrame,
                .frameCount    = std::max<u16>(1u, rec.frameCount),
                .ticksPerFrame = std::max<u16>(1u, rec.ticksPerFrame),
                .loop          = 1,
                .playing       = 1,
            });
        }
    }

    return localStackIds[0];
}

} // namespace en::world