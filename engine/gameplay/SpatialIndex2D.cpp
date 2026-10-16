#include "gameplay/SpatialIndex2D.h"

#include "assets/AssetBank.h"
#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace en::gameplay {

namespace {

constexpr f32 kGridCellSize    = 4.0f;
constexpr f32 kInvGridCellSize = 0.25f;

[[nodiscard]] constexpr SlabImpact2D IntersectRayExpandedAabb(
    f32 origX, f32 origY,
    f32 deltaX, f32 deltaY,
    f32 minX, f32 minY,
    f32 maxX, f32 maxY) noexcept {

    SlabImpact2D res{};
    f32 tNear = 0.0f;
    f32 tFar  = 1.0f;
    f32 normX = 0.0f;
    f32 normY = 0.0f;

    if (std::abs(deltaX) < 1e-6f) {
        if (origX <= minX || origX >= maxX) {
            return res;
        }
    } else {
        const f32 invDx = 1.0f / deltaX;
        f32 t1 = (minX - origX) * invDx;
        f32 t2 = (maxX - origX) * invDx;
        f32 nx = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            nx = 1.0f;
        }
        if (t1 > tNear) {
            tNear = t1;
            normX = nx;
            normY = 0.0f;
        }
        tFar = std::min(tFar, t2);
        if (tNear > tFar) {
            return res;
        }
    }

    if (std::abs(deltaY) < 1e-6f) {
        if (origY <= minY || origY >= maxY) {
            return res;
        }
    } else {
        const f32 invDy = 1.0f / deltaY;
        f32 t1 = (minY - origY) * invDy;
        f32 t2 = (maxY - origY) * invDy;
        f32 ny = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            ny = 1.0f;
        }
        if (t1 > tNear) {
            tNear = t1;
            normX = 0.0f;
            normY = ny;
        }
        tFar = std::min(tFar, t2);
        if (tNear > tFar) {
            return res;
        }
    }

    if (tNear >= 0.0f && tNear <= 1.0f && (normX != 0.0f || normY != 0.0f)) {
        res.toi     = tNear;
        res.normalX = normX;
        res.normalY = normY;
        res.hit     = true;
    }
    return res;
}

[[nodiscard]] constexpr bool IsSlopeTile(TileShape s) noexcept {
    return s >= TileShape::Slope45Asc && s <= TileShape::Slope26HiDesc;
}

// Evaluates local normalized [0..1] slope height and unit surface normal for 45-deg and 26.5-deg tiles.
constexpr void EvaluateSlopeSurface(
    TileShape shape, f32 localU01, f32& outLocalHeight01, f32& outNormX, f32& outNormY) noexcept {
    const f32 u = std::clamp(localU01, 0.0f, 1.0f);
    constexpr f32 kInvSqrt2 = 0.70710678f;
    constexpr f32 kNx26     = 0.44721360f;
    constexpr f32 kNy26     = 0.89442719f;

    switch (shape) {
        case TileShape::Slope45Asc:
            outLocalHeight01 = u;
            outNormX = -kInvSqrt2;
            outNormY =  kInvSqrt2;
            break;
        case TileShape::Slope45Desc:
            outLocalHeight01 = 1.0f - u;
            outNormX = kInvSqrt2;
            outNormY = kInvSqrt2;
            break;
        case TileShape::Slope26LoAsc:
            outLocalHeight01 = 0.5f * u;
            outNormX = -kNx26;
            outNormY =  kNy26;
            break;
        case TileShape::Slope26HiAsc:
            outLocalHeight01 = 0.5f + 0.5f * u;
            outNormX = -kNx26;
            outNormY =  kNy26;
            break;
        case TileShape::Slope26LoDesc:
            outLocalHeight01 = 0.5f * (1.0f - u);
            outNormX = kNx26;
            outNormY = kNy26;
            break;
        case TileShape::Slope26HiDesc:
            outLocalHeight01 = 1.0f - 0.5f * u;
            outNormX = kNx26;
            outNormY = kNy26;
            break;
        default:
            outLocalHeight01 = 1.0f;
            outNormX = 0.0f;
            outNormY = 1.0f;
            break;
    }
}

struct PendingGridBox {
    world::EntityId id;
    f32 minX;
    f32 minY;
    f32 maxX;
    f32 maxY;
    u16 mask;
    u8 kind;
    u8 aux;
};

} // namespace

void SpatialIndex2D::BindArena(std::span<u8> arenaSlice) noexcept {
    u8* ptr = arenaSlice.data();
    auto alignPtr = [&ptr](usize bytes, usize align = 16u) noexcept -> u8* {
        const usize addr = reinterpret_cast<usize>(ptr);
        const usize aligned = (addr + align - 1u) & ~(align - 1u);
        ptr = reinterpret_cast<u8*>(aligned + bytes);
        return reinterpret_cast<u8*>(aligned);
    };

    m_cellCounts     = reinterpret_cast<u16*>(alignPtr(sizeof(u16) * (kGridCells + 1u)));
    m_cellOffsets    = reinterpret_cast<u16*>(alignPtr(sizeof(u16) * (kGridCells + 1u)));
    m_sortedEntities = reinterpret_cast<world::EntityId*>(alignPtr(sizeof(world::EntityId) * kMaxCellRefs));
    m_boxMinX        = reinterpret_cast<f32*>(alignPtr(sizeof(f32) * kMaxCellRefs));
    m_boxMinY        = reinterpret_cast<f32*>(alignPtr(sizeof(f32) * kMaxCellRefs));
    m_boxMaxX        = reinterpret_cast<f32*>(alignPtr(sizeof(f32) * kMaxCellRefs));
    m_boxMaxY        = reinterpret_cast<f32*>(alignPtr(sizeof(f32) * kMaxCellRefs));
    m_boxMask        = reinterpret_cast<u16*>(alignPtr(sizeof(u16) * kMaxCellRefs));
    m_boxKind        = reinterpret_cast<u8*>(alignPtr(sizeof(u8) * kMaxCellRefs));
    m_boxAux         = reinterpret_cast<u8*>(alignPtr(sizeof(u8) * kMaxCellRefs));
    m_totalRefs      = 0;
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

void SpatialIndex2D::RebuildDynamicGrid(const world::World& world) noexcept {
    if (m_cellCounts == nullptr) {
        return;
    }
    std::memset(m_cellCounts, 0, sizeof(u16) * (kGridCells + 1u));
    m_totalRefs = 0;

    // Collect world-space AABBs once into a stack buffer to avoid double ECS View iteration
    std::array<PendingGridBox, 1024> pending{};
    u32 pendingCount = 0;
    const auto* __restrict worldTf = world.WorldTransforms().data();

    auto pushBox = [&](world::EntityId id, f32 minX, f32 minY, f32 maxX, f32 maxY,
                       u16 mask, RefKind kind, u8 aux) noexcept {
        if (pendingCount >= pending.size()) [[unlikely]] {
            return;
        }
        pending[pendingCount++] = PendingGridBox{
            .id   = id,
            .minX = minX,
            .minY = minY,
            .maxX = maxX,
            .maxY = maxY,
            .mask = mask,
            .kind = static_cast<u8>(kind),
            .aux  = aux,
        };

        const i32 cx0 = static_cast<i32>(std::floor(minX * kInvGridCellSize));
        const i32 cy0 = static_cast<i32>(std::floor(minY * kInvGridCellSize));
        const i32 cx1 = static_cast<i32>(std::floor(maxX * kInvGridCellSize));
        const i32 cy1 = static_cast<i32>(std::floor(maxY * kInvGridCellSize));

        for (i32 cy = cy0; cy <= cy1; ++cy) {
            for (i32 cx = cx0; cx <= cx1; ++cx) {
                if (m_totalRefs >= kMaxCellRefs) [[unlikely]] {
                    return;
                }
                ++m_cellCounts[HashCellCoords(cx, cy)];
                ++m_totalRefs;
            }
        }
    };

    for (auto [id, carrier] : world.View<KinematicCarrier2D>()) {
        const auto& wt = worldTf[id.index];
        const f32 cx = wt.x + carrier.bounds.OffsetX();
        const f32 cy = wt.y + carrier.bounds.OffsetY();
        const f32 hw = carrier.bounds.HalfW();
        const f32 hh = carrier.bounds.HalfH();
        pushBox(id, cx - hw, cy - hh, cx + hw, cy + hh, 0xFFFFu, RefKind::Carrier, 0);
    }

    for (auto [id, motor] : world.View<KinematicMotor2D>()) {
        if (!HasMotorFlag(motor.flags, MotorFlags::SoftPushbox)) {
            continue;
        }
        const auto& wt = worldTf[id.index];
        const f32 cx = wt.x + motor.bounds.OffsetX();
        const f32 cy = wt.y + motor.bounds.OffsetY();
        const f32 hw = motor.bounds.HalfW();
        const f32 hh = motor.bounds.HalfH();
        pushBox(id, cx - hw, cy - hh, cx + hw, cy + hh, motor.solidMask, RefKind::Pushbox, motor.pushboxGroup);
    }

    for (auto [id, rig] : world.View<HitboxRig2D>()) {
        const auto& wt = worldTf[id.index];
        const auto* motion = world.TryGet<MotionStack2D>(id);
        const f32 facing = motion != nullptr ? static_cast<f32>(motion->FacingSign()) : 1.0f;

        if ((rig.rigFlags & static_cast<u8>(HitboxRigFlags::HurtboxActive)) != 0) {
            const f32 cx = wt.x + rig.hurtbox.OffsetX() * facing;
            const f32 cy = wt.y + rig.hurtbox.OffsetY();
            const f32 hw = rig.hurtbox.HalfW();
            const f32 hh = rig.hurtbox.HalfH();
            pushBox(id, cx - hw, cy - hh, cx + hw, cy + hh, rig.hurtLayerMask, RefKind::Hurtbox, 0);
        }
        if ((rig.rigFlags & static_cast<u8>(HitboxRigFlags::HitboxActive)) != 0) {
            const f32 cx = wt.x + rig.primaryHitbox.OffsetX() * facing;
            const f32 cy = wt.y + rig.primaryHitbox.OffsetY();
            const f32 hw = rig.primaryHitbox.HalfW();
            const f32 hh = rig.primaryHitbox.HalfH();
            pushBox(id, cx - hw, cy - hh, cx + hw, cy + hh, rig.hitLayerMask, RefKind::Hitbox, rig.activeSliceIndex);
        }
    }

    // Pass 2: Exclusive prefix sum
    u16 running = 0;
    for (u32 c = 0; c <= kGridCells; ++c) {
        const u16 count  = m_cellCounts[c];
        m_cellOffsets[c] = running;
        m_cellCounts[c]  = running;
        running = static_cast<u16>(running + count);
    }

    // Pass 3: Scatter cached PendingGridBox array into flat SoA arrays
    u32 emitted = 0;
    for (u32 i = 0; i < pendingCount; ++i) {
        const PendingGridBox& b = pending[i];
        const i32 cx0 = static_cast<i32>(std::floor(b.minX * kInvGridCellSize));
        const i32 cy0 = static_cast<i32>(std::floor(b.minY * kInvGridCellSize));
        const i32 cx1 = static_cast<i32>(std::floor(b.maxX * kInvGridCellSize));
        const i32 cy1 = static_cast<i32>(std::floor(b.maxY * kInvGridCellSize));

        for (i32 cy = cy0; cy <= cy1; ++cy) {
            for (i32 cx = cx0; cx <= cx1; ++cx) {
                if (emitted >= kMaxCellRefs) [[unlikely]] {
                    return;
                }
                const u32 cell = HashCellCoords(cx, cy);
                const u16 dst  = m_cellCounts[cell]++;
                m_sortedEntities[dst] = b.id;
                m_boxMinX[dst]        = b.minX;
                m_boxMinY[dst]        = b.minY;
                m_boxMaxX[dst]        = b.maxX;
                m_boxMaxY[dst]        = b.maxY;
                m_boxMask[dst]        = b.mask;
                m_boxKind[dst]        = b.kind;
                m_boxAux[dst]         = b.aux;
                ++emitted;
            }
        }
    }
}

SlabImpact2D SpatialIndex2D::SweepAabb(
    world::EntityId ignoreEntity,
    f32 centerX, f32 centerY,
    f32 halfW, f32 halfH,
    f32 deltaX, f32 deltaY,
    u16 solidMask,
    bool ignoreOneWay) const noexcept {

    SlabImpact2D best{};
    const f32 sweepMinX = std::min(centerX, centerX + deltaX) - halfW;
    const f32 sweepMinY = std::min(centerY, centerY + deltaY) - halfH;
    const f32 sweepMaxX = std::max(centerX, centerX + deltaX) + halfW;
    const f32 sweepMaxY = std::max(centerY, centerY + deltaY) + halfH;

    // 1. Static tile map sweep (Full blocks, One-Way platforms, and Analytical Slopes)
    if (m_mapHeader != nullptr && (solidMask & 0x0001u) != 0) {
        const i32 tx0 = static_cast<i32>(std::floor((sweepMinX - m_mapHeader->originX) * m_invTileSize));
        const i32 ty0 = static_cast<i32>(std::floor((sweepMinY - m_mapHeader->originY) * m_invTileSize));
        const i32 tx1 = static_cast<i32>(std::floor((sweepMaxX - m_mapHeader->originX) * m_invTileSize));
        const i32 ty1 = static_cast<i32>(std::floor((sweepMaxY - m_mapHeader->originY) * m_invTileSize));

        for (i32 ty = ty0; ty <= ty1; ++ty) {
            for (i32 cx = tx0; cx <= tx1; ++cx) {
                const CookedTileCell cell = GetTileAt(cx, ty);
                if (cell.shape == TileShape::Empty) {
                    continue;
                }
                if (cell.shape == TileShape::OneWayTop && (ignoreOneWay || deltaY > 0.0f)) {
                    continue;
                }

                const f32 tileMinX = m_mapHeader->originX + static_cast<f32>(cx) * m_tileSize;
                const f32 tileMinY = m_mapHeader->originY + static_cast<f32>(ty) * m_tileSize;
                const f32 tileMaxX = tileMinX + m_tileSize;
                const f32 tileMaxY = tileMinY + m_tileSize;

                if (IsSlopeTile(cell.shape)) {
                    // Evaluate slope height at actor horizontal center
                    const f32 endCenterX = centerX + deltaX;
                    if (endCenterX + halfW < tileMinX || endCenterX - halfW > tileMaxX) {
                        continue;
                    }
                    const f32 localU = (endCenterX - tileMinX) * m_invTileSize;
                    f32 h01 = 1.0f, nx = 0.0f, ny = 1.0f;
                    EvaluateSlopeSurface(cell.shape, localU, h01, nx, ny);
                    const f32 slopeSurfaceY = tileMinY + h01 * m_tileSize;
                    const f32 footStartY    = centerY - halfH;
                    const f32 footEndY      = footStartY + deltaY;

                    if (deltaY <= 0.0f && footEndY <= slopeSurfaceY && footStartY >= tileMinY - 0.1f) {
                        const f32 toi = (std::abs(deltaY) > 1e-6f)
                            ? std::clamp((slopeSurfaceY - footStartY) / deltaY, 0.0f, 1.0f)
                            : 0.0f;
                        if (toi < best.toi) {
                            best.toi       = toi;
                            best.normalX   = nx;
                            best.normalY   = ny;
                            best.tileShape = cell.shape;
                            best.material  = cell.material;
                            best.hit       = true;
                        }
                    }
                    continue;
                }

                if (cell.shape == TileShape::OneWayTop && (centerY - halfH) < tileMaxY - 0.002f) {
                    continue;
                }

                const SlabImpact2D hit = IntersectRayExpandedAabb(
                    centerX, centerY, deltaX, deltaY,
                    tileMinX - halfW, tileMinY - halfH,
                    tileMaxX + halfW, tileMaxY + halfH
                );
                if (hit.hit && hit.toi < best.toi) {
                    best           = hit;
                    best.tileShape = cell.shape;
                    best.material  = cell.material;
                }
            }
        }
    }

    // 2. Dynamic carrier sweep
    if (m_cellOffsets != nullptr && m_totalRefs > 0) {
        const i32 gx0 = static_cast<i32>(std::floor(sweepMinX * kInvGridCellSize));
        const i32 gy0 = static_cast<i32>(std::floor(sweepMinY * kInvGridCellSize));
        const i32 gx1 = static_cast<i32>(std::floor(sweepMaxX * kInvGridCellSize));
        const i32 gy1 = static_cast<i32>(std::floor(sweepMaxY * kInvGridCellSize));

        for (i32 gy = gy0; gy <= gy1; ++gy) {
            for (i32 gx = gx0; gx <= gx1; ++gx) {
                const u32 cell = HashCellCoords(gx, gy);
                const u16 beg  = m_cellOffsets[cell];
                const u16 end  = m_cellOffsets[cell + 1u];
                for (u16 i = beg; i < end; ++i) {
                    if (m_boxKind[i] != static_cast<u8>(RefKind::Carrier) ||
                        m_sortedEntities[i] == ignoreEntity) {
                        continue;
                    }
                    const SlabImpact2D hit = IntersectRayExpandedAabb(
                        centerX, centerY, deltaX, deltaY,
                        m_boxMinX[i] - halfW, m_boxMinY[i] - halfH,
                        m_boxMaxX[i] + halfW, m_boxMaxY[i] + halfH
                    );
                    if (hit.hit && hit.toi < best.toi) {
                        best           = hit;
                        best.carrierId = m_sortedEntities[i];
                        best.tileShape = TileShape::FullSolid;
                    }
                }
            }
        }
    }

    return best;
}

bool SpatialIndex2D::CheckStaticPointSolid(
    f32 worldX, f32 worldY, bool requireSafeAnchor) const noexcept {
    if (m_mapHeader == nullptr) {
        return false;
    }
    const i32 tx = static_cast<i32>(std::floor((worldX - m_mapHeader->originX) * m_invTileSize));
    const i32 ty = static_cast<i32>(std::floor((worldY - m_mapHeader->originY) * m_invTileSize));
    const CookedTileCell cell = GetTileAt(tx, ty);
    if (cell.shape == TileShape::Empty || cell.shape == TileShape::OneWayTop || cell.shape == TileShape::HazardSolid) {
        return false;
    }
    if (requireSafeAnchor &&
        (static_cast<u8>(cell.material) & static_cast<u8>(TileMaterialFlags::SafeAnchor)) == 0) {
        return false;
    }
    return true;
}

bool SpatialIndex2D::DetectLedgeCorner(
    f32 chestWallX, f32 headTopY, f32 clearanceCenterX) const noexcept {
    return CheckStaticPointSolid(chestWallX, headTopY - 0.15f, false) &&
           !CheckStaticPointSolid(chestWallX, headTopY + 0.12f, false) &&
           !CheckStaticPointSolid(clearanceCenterX, headTopY + 0.12f, false);
}

ZoneSample2D SpatialIndex2D::SampleEnvironmentalZones(f32 centerX, f32 centerY) const noexcept {
    ZoneSample2D out{};
    if (m_mapHeader == nullptr || m_zones == nullptr) {
        return out;
    }
    u8 highestPrio = 0;
    for (u32 i = 0; i < m_mapHeader->zoneCount; ++i) {
        const EnvironmentalZone2D& z = m_zones[i];
        if (centerX >= z.minX && centerX <= z.maxX && centerY >= z.minY && centerY <= z.maxY) {
            if (z.priority >= highestPrio) {
                highestPrio   = z.priority;
                out.forceVx   = static_cast<f32>(z.forceVx_q8) * (1.0f / 256.0f);
                out.forceVy   = static_cast<f32>(z.forceVy_q8) * (1.0f / 256.0f);
                out.dragScale = static_cast<f32>(z.dragScale_q8) * (1.0f / 256.0f);
                out.submerged = (z.zoneFlags & 0x01u) != 0;
            }
        }
    }
    return out;
}

void SpatialIndex2D::ComputeSoftPushboxSeparation(
    world::EntityId selfId,
    u8 pushboxGroup,
    f32 centerX, f32 centerY,
    f32 halfW, f32 halfH,
    f32& inoutSepVx) const noexcept {
    if (m_cellOffsets == nullptr || m_totalRefs == 0) {
        return;
    }
    const i32 cx = static_cast<i32>(std::floor(centerX * kInvGridCellSize));
    const i32 cy = static_cast<i32>(std::floor(centerY * kInvGridCellSize));
    const u32 cell = HashCellCoords(cx, cy);
    const u16 beg  = m_cellOffsets[cell];
    const u16 end  = m_cellOffsets[cell + 1u];

    const f32 myMinX = centerX - halfW;
    const f32 myMaxX = centerX + halfW;
    const f32 myMinY = centerY - halfH;
    const f32 myMaxY = centerY + halfH;

    for (u16 i = beg; i < end; ++i) {
        if (m_boxKind[i] != static_cast<u8>(RefKind::Pushbox) ||
            m_boxAux[i] != pushboxGroup ||
            m_sortedEntities[i] == selfId) {
            continue;
        }
        if (myMaxX > m_boxMinX[i] && myMinX < m_boxMaxX[i] &&
            myMaxY > m_boxMinY[i] && myMinY < m_boxMaxY[i]) {
            const f32 otherCx = 0.5f * (m_boxMinX[i] + m_boxMaxX[i]);
            const f32 dir = (centerX >= otherCx) ? 1.0f : -1.0f;
            inoutSepVx += dir * 3.5f;
        }
    }
}

RayHit2D SpatialIndex2D::Raycast(
    f32 originX, f32 originY,
    f32 dirX, f32 dirY,
    f32 maxDist,
    u16 layerMask,
    world::EntityId ignoreId) const noexcept {
    RayHit2D out{};
    const f32 deltaX = dirX * maxDist;
    const f32 deltaY = dirY * maxDist;
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
    if (m_cellOffsets == nullptr || m_totalRefs == 0 || outHits.empty()) {
        return 0;
    }

    u32 hitCount = 0;
    const auto* __restrict worldTf = world.WorldTransforms().data();

    for (auto [attId, attRig] : world.View<HitboxRig2D>()) {
        if ((attRig.rigFlags & static_cast<u8>(HitboxRigFlags::HitboxActive)) == 0) {
            continue;
        }

        const auto& attWt     = worldTf[attId.index];
        const auto* attMotion = world.TryGet<MotionStack2D>(attId);
        const f32 attFacing   = attMotion != nullptr ? static_cast<f32>(attMotion->FacingSign()) : 1.0f;

        const f32 unionCx = attWt.x + attRig.primaryHitbox.OffsetX() * attFacing;
        const f32 unionCy = attWt.y + attRig.primaryHitbox.OffsetY();
        const f32 minX    = unionCx - attRig.primaryHitbox.HalfW();
        const f32 maxX    = unionCx + attRig.primaryHitbox.HalfW();
        const f32 minY    = unionCy - attRig.primaryHitbox.HalfH();
        const f32 maxY    = unionCy + attRig.primaryHitbox.HalfH();

        const CookedHitboxSocket* sliceSockets = nullptr;
        u8 sliceSocketCount = 0;

        if (const auto* fsm = world.TryGet<StateMachine2D>(attId)) {
            const assets::BlobRecord& blob = assetBank.GetBlob(fsm->rigBlob);
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

        const i32 cx0 = static_cast<i32>(std::floor(minX * kInvGridCellSize));
        const i32 cy0 = static_cast<i32>(std::floor(minY * kInvGridCellSize));
        const i32 cx1 = static_cast<i32>(std::floor(maxX * kInvGridCellSize));
        const i32 cy1 = static_cast<i32>(std::floor(maxY * kInvGridCellSize));

        for (i32 cy = cy0; cy <= cy1; ++cy) {
            for (i32 cx = cx0; cx <= cx1; ++cx) {
                const u32 cell = HashCellCoords(cx, cy);
                const u16 beg  = m_cellOffsets[cell];
                const u16 end  = m_cellOffsets[cell + 1u];

                for (u16 i = beg; i < end; ++i) {
                    const u8 kind = m_boxKind[i];
                    if (kind != static_cast<u8>(RefKind::Hurtbox) && kind != static_cast<u8>(RefKind::Hitbox)) {
                        continue;
                    }
                    const world::EntityId defId = m_sortedEntities[i];
                    if (defId == attId || (attRig.hitLayerMask & m_boxMask[i]) == 0) {
                        continue;
                    }
                    if (attRig.HasAlreadyHit(defId)) {
                        continue;
                    }
                    if (maxX <= m_boxMinX[i] || minX >= m_boxMaxX[i] ||
                        maxY <= m_boxMinY[i] || minY >= m_boxMaxY[i]) {
                        continue;
                    }

                    bool alreadyQueued = false;
                    for (u32 h = 0; h < hitCount; ++h) {
                        if (outHits[h].attacker == attId && outHits[h].defender == defId) {
                            alreadyQueued = true;
                            break;
                        }
                    }
                    if (alreadyQueued) {
                        continue;
                    }

                    // Narrowphase multi-socket evaluation (picks highest-priority overlapping socket)
                    u16 bestDmgMult = 256;
                    i16 bestKbAngle = 35;
                    u8  bestKbSpeed = 48;
                    u8  bestHitstop = 6;
                    u8  bestFlags   = 0;
                    f32 contactX    = 0.5f * (std::max(minX, m_boxMinX[i]) + std::min(maxX, m_boxMaxX[i]));
                    f32 contactY    = 0.5f * (std::max(minY, m_boxMinY[i]) + std::min(maxY, m_boxMaxY[i]));

                    if (sliceSockets != nullptr && sliceSocketCount > 0) {
                        bool socketHit = false;
                        u8 highestPrio = 0;
                        for (u8 s = 0; s < sliceSocketCount; ++s) {
                            const CookedHitboxSocket& sk = sliceSockets[s];
                            const f32 scx  = attWt.x + sk.localBox.OffsetX() * attFacing;
                            const f32 scy  = attWt.y + sk.localBox.OffsetY();
                            const f32 sMinX = scx - sk.localBox.HalfW();
                            const f32 sMaxX = scx + sk.localBox.HalfW();
                            const f32 sMinY = scy - sk.localBox.HalfH();
                            const f32 sMaxY = scy + sk.localBox.HalfH();

                            if (sMaxX > m_boxMinX[i] && sMinX < m_boxMaxX[i] &&
                                sMaxY > m_boxMinY[i] && sMinY < m_boxMaxY[i]) {
                                if (!socketHit || sk.priority >= highestPrio) {
                                    socketHit   = true;
                                    highestPrio = sk.priority;
                                    bestDmgMult = sk.damageMultiplier_q8;
                                    bestKbAngle = sk.knockbackAngleDeg;
                                    bestKbSpeed = sk.knockbackSpeed_q2;
                                    bestHitstop = sk.hitstopTicks;
                                    bestFlags   = sk.socketFlags;
                                    contactX    = 0.5f * (std::max(sMinX, m_boxMinX[i]) + std::min(sMaxX, m_boxMaxX[i]));
                                    contactY    = 0.5f * (std::max(sMinY, m_boxMinY[i]) + std::min(sMaxY, m_boxMaxY[i]));
                                }
                            }
                        }
                        if (!socketHit) {
                            continue;
                        }
                    }

                    const u8 isClash = (kind == static_cast<u8>(RefKind::Hitbox)) ? 1u : 0u;
                    if (hitCount < outHits.size()) {
                        outHits[hitCount++] = ResolvedHitEvent{
                            .attacker            = attId,
                            .defender            = defId,
                            .contactX            = contactX,
                            .contactY            = contactY,
                            .damageMultiplier_q8 = bestDmgMult,
                            .knockbackAngleDeg   = bestKbAngle,
                            .knockbackSpeed_q2   = bestKbSpeed,
                            .hitstopTicks        = bestHitstop,
                            .socketFlags         = bestFlags,
                            .isClashParry        = isClash,
                        };
                    }
                }
            }
        }
    }

    return hitCount;
}

} // namespace en::gameplay
