#include "gameplay/SpatialIndex2D.h"

#include "assets/AssetBank.h"
#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace en::gameplay {

namespace {

constexpr f32 kInvGridCellSize = 0.25f;

struct GridCellSpan {
    i32 x0, y0, x1, y1;
};

[[nodiscard]] inline GridCellSpan ComputeGridSpan(f32 minX, f32 minY, f32 maxX, f32 maxY) noexcept {
    return {
        static_cast<i32>(std::floor(minX * kInvGridCellSize)),
        static_cast<i32>(std::floor(minY * kInvGridCellSize)),
        static_cast<i32>(std::floor(maxX * kInvGridCellSize)),
        static_cast<i32>(std::floor(maxY * kInvGridCellSize))
    };
}

[[nodiscard]] constexpr bool ClipSlabAxis(
    f32 orig, f32 delta, f32 minB, f32 maxB,
    f32& inoutTNear, f32& inoutTFar, f32& outAxisNorm) noexcept {
    outAxisNorm = 0.0f;
    if (std::abs(delta) < 1e-6f) {
        return orig > minB && orig < maxB;
    }
    const f32 invD = 1.0f / delta;
    f32 t1 = (minB - orig) * invD;
    f32 t2 = (maxB - orig) * invD;
    f32 n  = -1.0f;
    if (t1 > t2) { std::swap(t1, t2); n = 1.0f; }
    if (t1 > inoutTNear) { inoutTNear = t1; outAxisNorm = n; }
    inoutTFar = std::min(inoutTFar, t2);
    return inoutTNear <= inoutTFar;
}

[[nodiscard]] constexpr SlabImpact2D IntersectRayExpandedAabb(
    f32 origX, f32 origY, f32 deltaX, f32 deltaY,
    f32 minX, f32 minY, f32 maxX, f32 maxY) noexcept {
    SlabImpact2D res{};
    f32 tNear = 0.0f, tFar = 1.0f, nx = 0.0f, ny = 0.0f;
    if (!ClipSlabAxis(origX, deltaX, minX, maxX, tNear, tFar, nx) ||
        !ClipSlabAxis(origY, deltaY, minY, maxY, tNear, tFar, ny)) {
        return res;
    }
    if (ny != 0.0f) nx = 0.0f;
    if (tNear >= 0.0f && tNear <= 1.0f && (nx != 0.0f || ny != 0.0f)) {
        res.toi = tNear; res.normalX = nx; res.normalY = ny; res.hit = true;
    }
    return res;
}

[[nodiscard]] constexpr bool IsSlopeTile(TileShape s) noexcept {
    return s >= TileShape::Slope45Asc && s <= TileShape::Slope26HiDesc;
}

struct alignas(16) SlopeCoeffs { f32 baseH, slopeH, normX, normY; };

constexpr std::array<SlopeCoeffs, 6> kSlopeTable{{
    {0.0f,  1.0f, -0.70710678f, 0.70710678f}, // Slope45Asc
    {1.0f, -1.0f,  0.70710678f, 0.70710678f}, // Slope45Desc
    {0.0f,  0.5f, -0.44721360f, 0.89442719f}, // Slope26LoAsc
    {0.5f,  0.5f, -0.44721360f, 0.89442719f}, // Slope26HiAsc
    {0.5f, -0.5f,  0.44721360f, 0.89442719f}, // Slope26LoDesc
    {1.0f, -0.5f,  0.44721360f, 0.89442719f}, // Slope26HiDesc
}};

} // namespace

void SpatialIndex2D::BindArena(std::span<u8> arenaSlice) noexcept {
    u8* ptr = arenaSlice.data();
    auto alignPtr = [&ptr](usize bytes) noexcept -> u8* {
        const usize aligned = (reinterpret_cast<usize>(ptr) + 63u) & ~usize{63u};
        ptr = reinterpret_cast<u8*>(aligned + bytes);
        return reinterpret_cast<u8*>(aligned);
    };

    m_cellCounts  = reinterpret_cast<u16*>(alignPtr(sizeof(u16) * (kGridCells + 1u)));
    m_cellOffsets = reinterpret_cast<u16*>(alignPtr(sizeof(u16) * (kGridCells + 1u)));
    m_boxes       = reinterpret_cast<GridBox*>(alignPtr(
        sizeof(GridBox) * kMaxCellRefs * 2u + sizeof(u16) * kMaxCellRefs));

    std::memset(m_cellCounts, 0, sizeof(u16) * (kGridCells + 1u));
    std::memset(m_cellOffsets, 0, sizeof(u16) * (kGridCells + 1u));
}

Result<void, GameplayErrc> SpatialIndex2D::BindCollisionMapBlob(const assets::BlobRecord& blob) noexcept {
    ClearCollisionMap();
    if (blob.data == nullptr || blob.size < sizeof(CookedCollisionMapHeader)) [[unlikely]] {
        return Err(GameplayErrc::CorruptedBlobHeader);
    }

    const auto* hdr = reinterpret_cast<const CookedCollisionMapHeader*>(blob.data);
    if (hdr->magic != CookedCollisionMapHeader::kMagic || hdr->version != CookedCollisionMapHeader::kVersion) [[unlikely]] {
        return Err(GameplayErrc::CorruptedBlobHeader);
    }

    const usize chunkCount = static_cast<usize>(hdr->chunkCountX) * static_cast<usize>(hdr->chunkCountY);
    const usize chunksEnd  = sizeof(CookedCollisionMapHeader) + chunkCount * sizeof(CookedCollisionChunk8x8);
    if (blob.size < chunksEnd || hdr->fineTileTableOffset > blob.size) [[unlikely]] {
        return Err(GameplayErrc::CorruptedBlobPayload);
    }

    m_mapHeader   = hdr;
    m_chunks      = reinterpret_cast<const CookedCollisionChunk8x8*>(blob.data + sizeof(CookedCollisionMapHeader));
    m_fineTiles   = reinterpret_cast<const CookedTileCell*>(blob.data + hdr->fineTileTableOffset);
    m_zones       = (hdr->zoneCount > 0 && hdr->zoneTableOffset + hdr->zoneCount * sizeof(EnvironmentalZone2D) <= blob.size)
        ? reinterpret_cast<const EnvironmentalZone2D*>(blob.data + hdr->zoneTableOffset)
        : nullptr;
    m_tileSize    = std::max(0.125f, static_cast<f32>(hdr->tileSize_q8) * (1.0f / 256.0f));
    m_invTileSize = 1.0f / m_tileSize;
    return {};
}

void SpatialIndex2D::ClearCollisionMap() noexcept {
    m_mapHeader   = nullptr;
    m_chunks      = nullptr;
    m_fineTiles   = nullptr;
    m_zones       = nullptr;
    m_tileSize    = 1.0f;
    m_invTileSize = 1.0f;
}

CookedTileCell SpatialIndex2D::GetTileAt(i32 tx, i32 ty) const noexcept {
    if (m_mapHeader == nullptr || tx < 0 || ty < 0) {
        return CookedTileCell{};
    }
    const u32 cx = static_cast<u32>(tx) >> 3u;
    const u32 cy = static_cast<u32>(ty) >> 3u;
    if (cx >= m_mapHeader->chunkCountX || cy >= m_mapHeader->chunkCountY) {
        return CookedTileCell{};
    }

    const CookedCollisionChunk8x8& chunk = m_chunks[cy * m_mapHeader->chunkCountX + cx];
    const u32 bitIdx = ((static_cast<u32>(ty) & 7u) << 3u) | (static_cast<u32>(tx) & 7u);
    const u64 bit    = 1ULL << bitIdx;
    if ((chunk.solidBitmask & bit) == 0ULL) {
        return CookedTileCell{};
    }
    if ((chunk.pureFullBlockBitmask & bit) != 0ULL) {
        return CookedTileCell{TileShape::FullSolid, TileMaterialFlags::SafeAnchor};
    }
    return m_fineTiles[chunk.fineTileBaseIndex + bitIdx];
}

void SpatialIndex2D::RebuildDynamicGrid(
    const world::World& world,
    SpatialRebuildMode mode) noexcept {
    if (m_cellCounts == nullptr) {
        return;
    }

    const auto& rigPool = world.GetPool<HitboxRig2D>();
    const auto rigs     = rigPool.Components();

    // In CombatOnlyIfActive mode (Stage 5), skip rebuilding completely if no entity has an active hitbox!
    // Stage 4's carrier boxes remain intact in m_boxes for post-tick Raycast queries.
    if (mode == SpatialRebuildMode::CombatOnlyIfActive) {
        bool anyActiveHitbox = false;
        for (const HitboxRig2D& r : rigs) {
            if ((r.rigFlags & static_cast<u8>(HitboxRigFlags::HitboxActive)) != 0) {
                anyActiveHitbox = true;
                break;
            }
        }
        if (!anyActiveHitbox) {
            return;
        }
    }

    std::memset(m_cellCounts, 0, sizeof(u16) * (kGridCells + 1u));

    GridBox* __restrict stagingBoxes = m_boxes + kMaxCellRefs;
    u16* __restrict stagingCells     = reinterpret_cast<u16*>(stagingBoxes + kMaxCellRefs);
    u32 totalRefs = 0;

    const auto* __restrict worldTf = world.WorldTransforms().data();

    auto emitBox = [&](world::EntityId id, f32 cx, f32 cy, f32 hw, f32 hh,
                       u16 mask, RefKind kind, u8 aux) noexcept {
        const GridBox box{cx - hw, cy - hh, cx + hw, cy + hh, id, mask, static_cast<u8>(kind), aux};
        const GridCellSpan span = ComputeGridSpan(box.minX, box.minY, box.maxX, box.maxY);

        for (i32 gy = span.y0; gy <= span.y1; ++gy) {
            for (i32 gx = span.x0; gx <= span.x1; ++gx) {
                if (totalRefs >= kMaxCellRefs) [[unlikely]] return;
                const u32 cell = HashCellCoords(gx, gy);
                ++m_cellCounts[cell];
                stagingBoxes[totalRefs] = box;
                stagingCells[totalRefs] = static_cast<u16>(cell);
                ++totalRefs;
            }
        }
    };

    const auto& carrierPool = world.GetPool<KinematicCarrier2D>();
    const auto carrierIds   = carrierPool.Entities();
    const auto carriers     = carrierPool.Components();
    for (u16 i = 0, n = carrierPool.Size(); i < n; ++i) {
        const world::EntityId id = carrierIds[i];
        const auto& b            = carriers[i].bounds;
        const auto& wt           = worldTf[id.index];
        emitBox(id, wt.x + b.OffsetX(), wt.y + b.OffsetY(), b.HalfW(), b.HalfH(), 0xFFFFu, RefKind::Carrier, 0);
    }

    if (mode != SpatialRebuildMode::CombatOnlyIfActive) {
        const auto& motorPool = world.GetPool<KinematicMotor2D>();
        const auto motorIds   = motorPool.Entities();
        const auto motors     = motorPool.Components();
        for (u16 i = 0, n = motorPool.Size(); i < n; ++i) {
            const auto& motor = motors[i];
            if (!HasMotorFlag(motor.flags, MotorFlags::SoftPushbox)) continue;
            const world::EntityId id = motorIds[i];
            const auto& b            = motor.bounds;
            const auto& wt           = worldTf[id.index];
            emitBox(id, wt.x + b.OffsetX(), wt.y + b.OffsetY(), b.HalfW(), b.HalfH(),
                    motor.solidMask, RefKind::Pushbox, motor.pushboxGroup);
        }
    }

    if (mode != SpatialRebuildMode::KinematicsOnly) {
        constexpr world::ComponentMask kMotionBit = world::MakeComponentMask<MotionStack2D>();
        const auto& motionPool = world.GetPool<MotionStack2D>();
        const auto rigIds      = rigPool.Entities();

        for (u16 i = 0, n = rigPool.Size(); i < n; ++i) {
            const world::EntityId id = rigIds[i];
            const auto& rig          = rigs[i];
            const auto& wt           = worldTf[id.index];
            const f32 facing = ((world.GetComponentMaskByIndex(id.index) & kMotionBit) != 0)
                ? static_cast<f32>(motionPool.DirectAtEntityIndex(id.index).FacingSign())
                : 1.0f;

            if ((rig.rigFlags & static_cast<u8>(HitboxRigFlags::HurtboxActive)) != 0) {
                const auto& b = rig.hurtbox;
                emitBox(id, wt.x + b.OffsetX() * facing, wt.y + b.OffsetY(), b.HalfW(), b.HalfH(),
                        rig.hurtLayerMask, RefKind::Hurtbox, 0);
            }
            if ((rig.rigFlags & static_cast<u8>(HitboxRigFlags::HitboxActive)) != 0) {
                const auto& b = rig.primaryHitbox;
                emitBox(id, wt.x + b.OffsetX() * facing, wt.y + b.OffsetY(), b.HalfW(), b.HalfH(),
                        rig.hitLayerMask, RefKind::Hitbox, rig.activeSliceIndex);
            }
        }
    }

    u16 running = 0;
    for (u32 c = 0; c <= kGridCells; ++c) {
        const u16 count  = m_cellCounts[c];
        m_cellOffsets[c] = running;
        m_cellCounts[c]  = running;
        running = static_cast<u16>(running + count);
    }

    for (u32 i = 0; i < totalRefs; ++i) {
        m_boxes[m_cellCounts[stagingCells[i]]++] = stagingBoxes[i];
    }
}

SlabImpact2D SpatialIndex2D::SweepAabb(
    world::EntityId ignoreEntity,
    f32 centerX, f32 centerY, f32 halfW, f32 halfH,
    f32 deltaX, f32 deltaY, u16 solidMask, bool ignoreOneWay) const noexcept {

    SlabImpact2D best{};
    const f32 sweepMinX = std::min(centerX, centerX + deltaX) - halfW;
    const f32 sweepMinY = std::min(centerY, centerY + deltaY) - halfH;
    const f32 sweepMaxX = std::max(centerX, centerX + deltaX) + halfW;
    const f32 sweepMaxY = std::max(centerY, centerY + deltaY) + halfH;

    if (m_mapHeader != nullptr && (solidMask & 0x0001u) != 0) {
        const i32 maxTileX = static_cast<i32>(m_mapHeader->chunkCountX) * 8 - 1;
        const i32 maxTileY = static_cast<i32>(m_mapHeader->chunkCountY) * 8 - 1;
        const i32 tx0 = std::max(0, static_cast<i32>(std::floor((sweepMinX - m_mapHeader->originX) * m_invTileSize)));
        const i32 ty0 = std::max(0, static_cast<i32>(std::floor((sweepMinY - m_mapHeader->originY) * m_invTileSize)));
        const i32 tx1 = std::min(maxTileX, static_cast<i32>(std::floor((sweepMaxX - m_mapHeader->originX) * m_invTileSize)));
        const i32 ty1 = std::min(maxTileY, static_cast<i32>(std::floor((sweepMaxY - m_mapHeader->originY) * m_invTileSize)));

        for (i32 ty = ty0; ty <= ty1; ++ty) {
            for (i32 cx = tx0; cx <= tx1; ++cx) {
                const CookedTileCell cell = GetTileAt(cx, ty);
                if (cell.shape == TileShape::Empty ||
                    (cell.shape == TileShape::OneWayTop && (ignoreOneWay || deltaY > 0.0f))) {
                    continue;
                }

                const f32 tileMinX = m_mapHeader->originX + static_cast<f32>(cx) * m_tileSize;
                const f32 tileMinY = m_mapHeader->originY + static_cast<f32>(ty) * m_tileSize;
                const f32 tileMaxX = tileMinX + m_tileSize;
                const f32 tileMaxY = tileMinY + m_tileSize;

                if (IsSlopeTile(cell.shape)) {
                    const f32 endCenterX = centerX + deltaX;
                    if (endCenterX + halfW < tileMinX || endCenterX - halfW > tileMaxX) continue;
                    const f32 u = std::clamp((endCenterX - tileMinX) * m_invTileSize, 0.0f, 1.0f);
                    const SlopeCoeffs& sc  = kSlopeTable[static_cast<u8>(cell.shape) - static_cast<u8>(TileShape::Slope45Asc)];
                    const f32 slopeSurfaceY = tileMinY + (sc.baseH + sc.slopeH * u) * m_tileSize;
                    const f32 footStartY    = centerY - halfH;
                    const f32 footEndY      = footStartY + deltaY;

                    if (deltaY <= 0.0f && footEndY <= slopeSurfaceY && footStartY >= tileMinY - 0.1f) {
                        const f32 toi = (std::abs(deltaY) > 1e-6f)
                            ? std::clamp((slopeSurfaceY - footStartY) / deltaY, 0.0f, 1.0f)
                            : 0.0f;
                        if (toi < best.toi) {
                            best = {toi, sc.normX, sc.normY, world::kNullEntity, cell.shape, cell.material, true};
                        }
                    }
                    continue;
                }

                if (cell.shape == TileShape::OneWayTop && (centerY - halfH) < tileMaxY - 0.002f) {
                    continue;
                }

                const SlabImpact2D hit = IntersectRayExpandedAabb(
                    centerX, centerY, deltaX, deltaY,
                    tileMinX - halfW, tileMinY - halfH, tileMaxX + halfW, tileMaxY + halfH);
                if (hit.hit && hit.toi < best.toi) {
                    best = hit; best.tileShape = cell.shape; best.material = cell.material;
                }
            }
        }
    }

    if (TotalRefs() > 0) {
        const GridCellSpan span = ComputeGridSpan(sweepMinX, sweepMinY, sweepMaxX, sweepMaxY);
        for (i32 gy = span.y0; gy <= span.y1; ++gy) {
            for (i32 gx = span.x0; gx <= span.x1; ++gx) {
                const u32 cell = HashCellCoords(gx, gy);
                for (u16 i = m_cellOffsets[cell], end = m_cellOffsets[cell + 1u]; i < end; ++i) {
                    const GridBox& box = m_boxes[i];
                    if (box.kind != static_cast<u8>(RefKind::Carrier) || box.id == ignoreEntity) continue;
                    const SlabImpact2D hit = IntersectRayExpandedAabb(
                        centerX, centerY, deltaX, deltaY,
                        box.minX - halfW, box.minY - halfH, box.maxX + halfW, box.maxY + halfH);
                    if (hit.hit && hit.toi < best.toi) {
                        best = hit; best.carrierId = box.id; best.tileShape = TileShape::FullSolid;
                    }
                }
            }
        }
    }

    return best;
}

bool SpatialIndex2D::CheckStaticPointSolid(f32 worldX, f32 worldY, bool requireSafeAnchor) const noexcept {
    if (m_mapHeader == nullptr) return false;
    const i32 tx = static_cast<i32>(std::floor((worldX - m_mapHeader->originX) * m_invTileSize));
    const i32 ty = static_cast<i32>(std::floor((worldY - m_mapHeader->originY) * m_invTileSize));
    const CookedTileCell cell = GetTileAt(tx, ty);
    if (cell.shape == TileShape::Empty || cell.shape == TileShape::OneWayTop || cell.shape == TileShape::HazardSolid) {
        return false;
    }
    return !requireSafeAnchor || HasFlag(cell.material, TileMaterialFlags::SafeAnchor);
}

bool SpatialIndex2D::DetectLedgeCorner(f32 chestWallX, f32 headTopY, f32 clearanceCenterX) const noexcept {
    return CheckStaticPointSolid(chestWallX, headTopY - 0.15f, false) &&
           !CheckStaticPointSolid(chestWallX, headTopY + 0.12f, false) &&
           !CheckStaticPointSolid(clearanceCenterX, headTopY + 0.12f, false);
}

ZoneSample2D SpatialIndex2D::SampleEnvironmentalZones(f32 centerX, f32 centerY) const noexcept {
    ZoneSample2D out{};
    if (m_mapHeader == nullptr || m_zones == nullptr) return out;
    u8 highestPrio = 0;
    for (u32 i = 0; i < m_mapHeader->zoneCount; ++i) {
        const EnvironmentalZone2D& z = m_zones[i];
        if (centerX >= z.minX && centerX <= z.maxX && centerY >= z.minY && centerY <= z.maxY &&
            z.priority >= highestPrio) {
            highestPrio   = z.priority;
            out.forceVx   = static_cast<f32>(z.forceVx_q8) * (1.0f / 256.0f);
            out.forceVy   = static_cast<f32>(z.forceVy_q8) * (1.0f / 256.0f);
            out.dragScale = static_cast<f32>(z.dragScale_q8) * (1.0f / 256.0f);
            out.submerged = (z.zoneFlags & 0x01u) != 0;
        }
    }
    return out;
}

void SpatialIndex2D::ComputeSoftPushboxSeparation(
    world::EntityId selfId, u8 pushboxGroup,
    f32 centerX, f32 centerY, f32 halfW, f32 halfH, f32& inoutSepVx) const noexcept {
    if (TotalRefs() == 0) return;

    const f32 myMinX = centerX - halfW, myMaxX = centerX + halfW;
    const f32 myMinY = centerY - halfH, myMaxY = centerY + halfH;
    const GridCellSpan span = ComputeGridSpan(myMinX, myMinY, myMaxX, myMaxY);

    u64 seenBloom = 0;
    for (i32 cy = span.y0; cy <= span.y1; ++cy) {
        for (i32 cx = span.x0; cx <= span.x1; ++cx) {
            const u32 cell = HashCellCoords(cx, cy);
            for (u16 i = m_cellOffsets[cell], end = m_cellOffsets[cell + 1u]; i < end; ++i) {
                const GridBox& box = m_boxes[i];
                if (box.kind != static_cast<u8>(RefKind::Pushbox) ||
                    box.aux != pushboxGroup || box.id == selfId) {
                    continue;
                }
                const u64 bloomBits = HitboxRig2D::ComputeEntityBloomBits(box.id);
                if ((seenBloom & bloomBits) != bloomBits &&
                    myMaxX > box.minX && myMinX < box.maxX &&
                    myMaxY > box.minY && myMinY < box.maxY) {
                    seenBloom |= bloomBits;
                    inoutSepVx += (centerX >= 0.5f * (box.minX + box.maxX)) ? 3.5f : -3.5f;
                }
            }
        }
    }
}

RayHit2D SpatialIndex2D::Raycast(
    f32 originX, f32 originY, f32 dirX, f32 dirY,
    f32 maxDist, u16 layerMask, world::EntityId ignoreId) const noexcept {
    RayHit2D out{};
    const f32 deltaX = dirX * maxDist, deltaY = dirY * maxDist;
    const SlabImpact2D hit = SweepAabb(ignoreId, originX, originY, 0.001f, 0.001f, deltaX, deltaY, layerMask, false);
    if (hit.hit) {
        out.hit      = true;
        out.distance = hit.toi * maxDist;
        out.pointX   = originX + deltaX * hit.toi;
        out.pointY   = originY + deltaY * hit.toi;
        out.normalX  = hit.normalX;
        out.normalY  = hit.normalY;
        out.entity   = hit.carrierId;
    }
    return out;
}

u32 SpatialIndex2D::GatherCombatOverlaps(
    const world::World& world,
    const assets::AssetBank& assetBank,
    std::span<ResolvedHitEvent> outHits) const noexcept {
    if (TotalRefs() == 0 || outHits.empty()) return 0;

    u32 hitCount = 0;
    const auto* __restrict worldTf = world.WorldTransforms().data();
    constexpr world::ComponentMask kMotionBit = world::MakeComponentMask<MotionStack2D>();
    constexpr world::ComponentMask kFsmBit    = world::MakeComponentMask<StateMachine2D>();

    const auto& motionPool = world.GetPool<MotionStack2D>();
    const auto& fsmPool    = world.GetPool<StateMachine2D>();
    const auto& rigPool    = world.GetPool<HitboxRig2D>();
    const auto rigIds      = rigPool.Entities();
    const auto rigs        = rigPool.Components();

    for (u16 r = 0, rigCount = rigPool.Size(); r < rigCount; ++r) {
        const HitboxRig2D& attRig = rigs[r];
        if ((attRig.rigFlags & static_cast<u8>(HitboxRigFlags::HitboxActive)) == 0) continue;

        const world::EntityId attId    = rigIds[r];
        const world::ComponentMask msk = world.GetComponentMaskByIndex(attId.index);
        const auto& attWt              = worldTf[attId.index];
        const f32 attFacing            = ((msk & kMotionBit) != 0)
            ? static_cast<f32>(motionPool.DirectAtEntityIndex(attId.index).FacingSign())
            : 1.0f;

        const f32 unionCx = attWt.x + attRig.primaryHitbox.OffsetX() * attFacing;
        const f32 unionCy = attWt.y + attRig.primaryHitbox.OffsetY();
        const f32 minX = unionCx - attRig.primaryHitbox.HalfW(), maxX = unionCx + attRig.primaryHitbox.HalfW();
        const f32 minY = unionCy - attRig.primaryHitbox.HalfH(), maxY = unionCy + attRig.primaryHitbox.HalfH();

        const CookedHitboxSocket* sliceSockets = nullptr;
        u8 sliceSocketCount = 0;

        if ((msk & kFsmBit) != 0) {
            const assets::BlobRecord& blob = assetBank.GetBlob(fsmPool.DirectAtEntityIndex(attId.index).rigBlob);
            if (blob.data != nullptr && blob.size >= sizeof(CookedAnimRigHeader)) {
                const auto* hdr = reinterpret_cast<const CookedAnimRigHeader*>(blob.data);
                if (hdr->magic == CookedAnimRigHeader::kMagic && attRig.activeSliceIndex < hdr->hitboxSliceCount) {
                    const auto* slices  = reinterpret_cast<const CookedHitboxSlice*>(blob.data + hdr->hitboxSliceOffset);
                    const auto* sockets = reinterpret_cast<const CookedHitboxSocket*>(blob.data + hdr->hitboxSocketOffset);
                    const CookedHitboxSlice& sl = slices[attRig.activeSliceIndex];
                    if (sl.socketCount > 0 && sl.firstSocketIndex + sl.socketCount <= hdr->hitboxSocketCount) {
                        sliceSockets     = &sockets[sl.firstSocketIndex];
                        sliceSocketCount = sl.socketCount;
                    }
                }
            }
        }

        const GridCellSpan span = ComputeGridSpan(minX, minY, maxX, maxY);
        u64 localHitBloom = attRig.hitBloomMask;

        for (i32 cy = span.y0; cy <= span.y1; ++cy) {
            for (i32 cx = span.x0; cx <= span.x1; ++cx) {
                const u32 cell = HashCellCoords(cx, cy);
                for (u16 i = m_cellOffsets[cell], end = m_cellOffsets[cell + 1u]; i < end; ++i) {
                    const GridBox& box = m_boxes[i];
                    if (box.kind != static_cast<u8>(RefKind::Hurtbox) &&
                        box.kind != static_cast<u8>(RefKind::Hitbox)) {
                        continue;
                    }
                    const world::EntityId defId = box.id;
                    if (defId == attId || (attRig.hitLayerMask & box.mask) == 0) continue;

                    const u64 defBloom = HitboxRig2D::ComputeEntityBloomBits(defId);
                    if ((localHitBloom & defBloom) == defBloom ||
                        maxX <= box.minX || minX >= box.maxX ||
                        maxY <= box.minY || minY >= box.maxY) {
                        continue;
                    }

                    u16 bestDmgMult = 256;
                    i16 bestKbAngle = 35;
                    u8  bestKbSpeed = 48, bestHitstop = 6, bestFlags = 0;
                    f32 contactX = 0.5f * (std::max(minX, box.minX) + std::min(maxX, box.maxX));
                    f32 contactY = 0.5f * (std::max(minY, box.minY) + std::min(maxY, box.maxY));

                    if (sliceSockets != nullptr && sliceSocketCount > 0) {
                        bool socketHit = false;
                        u8 highestPrio = 0;
                        for (u8 s = 0; s < sliceSocketCount; ++s) {
                            const CookedHitboxSocket& sk = sliceSockets[s];
                            const f32 scx   = attWt.x + sk.localBox.OffsetX() * attFacing;
                            const f32 scy   = attWt.y + sk.localBox.OffsetY();
                            const f32 sMinX = scx - sk.localBox.HalfW(), sMaxX = scx + sk.localBox.HalfW();
                            const f32 sMinY = scy - sk.localBox.HalfH(), sMaxY = scy + sk.localBox.HalfH();

                            if (sMaxX > box.minX && sMinX < box.maxX &&
                                sMaxY > box.minY && sMinY < box.maxY &&
                                (!socketHit || sk.priority >= highestPrio)) {
                                socketHit   = true;
                                highestPrio = sk.priority;
                                bestDmgMult = sk.damageMultiplier_q8;
                                bestKbAngle = sk.knockbackAngleDeg;
                                bestKbSpeed = sk.knockbackSpeed_q2;
                                bestHitstop = sk.hitstopTicks;
                                bestFlags   = sk.socketFlags;
                                contactX    = 0.5f * (std::max(sMinX, box.minX) + std::min(sMaxX, box.maxX));
                                contactY    = 0.5f * (std::max(sMinY, box.minY) + std::min(sMaxY, box.maxY));
                            }
                        }
                        if (!socketHit) continue;
                    }

                    localHitBloom |= defBloom;
                    if (hitCount < outHits.size()) {
                        outHits[hitCount++] = ResolvedHitEvent{
                            attId, defId, contactX, contactY,
                            bestDmgMult, bestKbAngle, bestKbSpeed, bestHitstop, bestFlags,
                            (box.kind == static_cast<u8>(RefKind::Hitbox)) ? u8{1} : u8{0}
                        };
                    }
                }
            }
        }
    }

    return hitCount;
}

} // namespace en::gameplay
