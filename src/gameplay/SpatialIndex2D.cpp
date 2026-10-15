#include "gameplay/SpatialIndex2D.h"

#include "assets/AssetBank.h"
#include "world/World.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace en::gameplay {

namespace {

constexpr f32 kCellSize = 2.0f;
constexpr f32 kInvCellSize = 0.5f;
constexpr f32 kRayEps = 1e-6f;

[[nodiscard]] constexpr i32 FastFloorToInt(f32 v) noexcept {
    const i32 i = static_cast<i32>(v);
    return (v < static_cast<f32>(i)) ? (i - 1) : i;
}

[[nodiscard]] constexpr bool TestMinkowskiSlab(
    f32 startX, f32 startY,
    f32 deltaX, f32 deltaY,
    f32 boxMinX, f32 boxMinY,
    f32 boxMaxX, f32 boxMaxY,
    f32& outToi, f32& outNx, f32& outNy) noexcept {
    f32 tEntryX = -1e30f;
    f32 tExitX  =  1e30f;
    f32 nx = 0.0f;

    if (std::abs(deltaX) < kRayEps) {
        if (startX <= boxMinX || startX >= boxMaxX) {
            return false;
        }
    } else {
        const f32 invDx = 1.0f / deltaX;
        const f32 t1 = (boxMinX - startX) * invDx;
        const f32 t2 = (boxMaxX - startX) * invDx;
        if (t1 < t2) {
            tEntryX = t1;
            tExitX  = t2;
            nx = -1.0f;
        } else {
            tEntryX = t2;
            tExitX  = t1;
            nx = 1.0f;
        }
    }

    f32 tEntryY = -1e30f;
    f32 tExitY  =  1e30f;
    f32 ny = 0.0f;

    if (std::abs(deltaY) < kRayEps) {
        if (startY <= boxMinY || startY >= boxMaxY) {
            return false;
        }
    } else {
        const f32 invDy = 1.0f / deltaY;
        const f32 t1 = (boxMinY - startY) * invDy;
        const f32 t2 = (boxMaxY - startY) * invDy;
        if (t1 < t2) {
            tEntryY = t1;
            tExitY  = t2;
            ny = -1.0f;
        } else {
            tEntryY = t2;
            tExitY  = t1;
            ny = 1.0f;
        }
    }

    const f32 tEntry = std::max(tEntryX, tEntryY);
    const f32 tExit  = std::min(tExitX, tExitY);

    if (tEntry > tExit || tExit <= 0.0f || tEntry > 1.0f) {
        return false;
    }

    if (tEntry < 0.0f) {
        return false;
    }

    outToi = tEntry;
    if (tEntryX > tEntryY) {
        outNx = nx;
        outNy = 0.0f;
    } else {
        outNx = 0.0f;
        outNy = ny;
    }
    return true;
}

} // namespace

void SpatialIndex2D::BindArena(std::span<u8> arenaSlice) noexcept {
    u8* ptr = arenaSlice.data();
    const auto alignPtr = [&ptr](usize align) noexcept {
        const usize addr = reinterpret_cast<usize>(ptr);
        const usize aligned = (addr + (align - 1u)) & ~(align - 1u);
        ptr = reinterpret_cast<u8*>(aligned);
    };

    alignPtr(64);
    m_cellCounts = reinterpret_cast<u16*>(ptr);
    ptr += sizeof(u16) * (kGridCells + 1u);

    alignPtr(64);
    m_cellOffsets = reinterpret_cast<u16*>(ptr);
    ptr += sizeof(u16) * (kGridCells + 1u);

    alignPtr(64);
    m_sortedEntities = reinterpret_cast<world::EntityId*>(ptr);
    ptr += sizeof(world::EntityId) * kMaxCellRefs;

    alignPtr(64);
    m_boxMinX = reinterpret_cast<f32*>(ptr);
    ptr += sizeof(f32) * kMaxCellRefs;

    m_boxMinY = reinterpret_cast<f32*>(ptr);
    ptr += sizeof(f32) * kMaxCellRefs;

    m_boxMaxX = reinterpret_cast<f32*>(ptr);
    ptr += sizeof(f32) * kMaxCellRefs;

    m_boxMaxY = reinterpret_cast<f32*>(ptr);
    ptr += sizeof(f32) * kMaxCellRefs;

    alignPtr(32);
    m_boxMask = reinterpret_cast<u16*>(ptr);
    ptr += sizeof(u16) * kMaxCellRefs;

    m_boxKind = ptr;
    ptr += sizeof(u8) * kMaxCellRefs;

    m_boxAux = ptr;
    ptr += sizeof(u8) * kMaxCellRefs;

    std::memset(m_cellCounts, 0, sizeof(u16) * (kGridCells + 1u));
    std::memset(m_cellOffsets, 0, sizeof(u16) * (kGridCells + 1u));
    m_totalRefs = 0;
}

Result<void, GameplayErrc> SpatialIndex2D::BindCollisionMapBlob(
    const assets::BlobRecord& blob) noexcept {
    if (blob.data == nullptr || blob.size < sizeof(CookedCollisionMapHeader)) {
        return Err(GameplayErrc::CorruptedBlobHeader);
    }

    const auto* hdr = reinterpret_cast<const CookedCollisionMapHeader*>(blob.data);
    if (hdr->magic != CookedCollisionMapHeader::kMagic ||
        hdr->version != CookedCollisionMapHeader::kVersion ||
        hdr->tileSize_q8 == 0) {
        return Err(GameplayErrc::CorruptedBlobHeader);
    }

    const usize chunkCount = static_cast<usize>(hdr->chunkCountX) * static_cast<usize>(hdr->chunkCountY);
    const usize chunksEnd = sizeof(CookedCollisionMapHeader) + chunkCount * sizeof(CookedCollisionChunk8x8);
    if (blob.size < chunksEnd || hdr->fineTileTableOffset < chunksEnd || hdr->fineTileTableOffset > blob.size) {
        return Err(GameplayErrc::CorruptedBlobPayload);
    }

    m_mapHeader = hdr;
    m_chunks = reinterpret_cast<const CookedCollisionChunk8x8*>(blob.data + sizeof(CookedCollisionMapHeader));
    m_fineTiles = reinterpret_cast<const CookedTileCell*>(blob.data + hdr->fineTileTableOffset);
    if (hdr->zoneCount > 0 && hdr->zoneTableOffset + hdr->zoneCount * sizeof(EnvironmentalZone2D) <= blob.size) {
        m_zones = reinterpret_cast<const EnvironmentalZone2D*>(blob.data + hdr->zoneTableOffset);
    } else {
        m_zones = nullptr;
    }

    m_tileSize = static_cast<f32>(hdr->tileSize_q8) * (1.0f / 256.0f);
    m_invTileSize = 1.0f / m_tileSize;
    return Ok;
}

void SpatialIndex2D::ClearCollisionMap() noexcept {
    m_mapHeader = nullptr;
    m_chunks = nullptr;
    m_fineTiles = nullptr;
    m_zones = nullptr;
}

CookedTileCell SpatialIndex2D::GetTileAt(i32 tx, i32 ty) const noexcept {
    if (m_mapHeader == nullptr || tx < 0 || ty < 0) {
        return {};
    }
    const i32 chunkX = tx >> 3;
    const i32 chunkY = ty >> 3;
    if (chunkX >= static_cast<i32>(m_mapHeader->chunkCountX) ||
        chunkY >= static_cast<i32>(m_mapHeader->chunkCountY)) {
        return {};
    }

    const CookedCollisionChunk8x8& chunk =
        m_chunks[chunkY * static_cast<i32>(m_mapHeader->chunkCountX) + chunkX];
    const u32 localIdx = static_cast<u32>((ty & 7) * 8 + (tx & 7));
    const u64 bit = 1ULL << localIdx;

    if ((chunk.solidBitmask & bit) == 0ULL) {
        return {};
    }
    if ((chunk.pureFullBlockBitmask & bit) != 0ULL) {
        return CookedTileCell{TileShape::FullSolid, TileMaterialFlags::SafeAnchor};
    }
    return m_fineTiles[chunk.fineTileBaseIndex + localIdx];
}

void SpatialIndex2D::RebuildDynamicGrid(const world::World& world) noexcept {
    if (m_cellCounts == nullptr) {
        return;
    }

    std::memset(m_cellCounts, 0, sizeof(u16) * (kGridCells + 1u));

    const auto tallyBox = [this](f32 minX, f32 minY, f32 maxX, f32 maxY) noexcept {
        const i32 x0 = FastFloorToInt(minX * kInvCellSize);
        const i32 y0 = FastFloorToInt(minY * kInvCellSize);
        const i32 x1 = FastFloorToInt(maxX * kInvCellSize);
        const i32 y1 = FastFloorToInt(maxY * kInvCellSize);
        for (i32 cy = y0; cy <= y1; ++cy) {
            for (i32 cx = x0; cx <= x1; ++cx) {
                const u32 bucket = HashCellCoords(cx, cy);
                ++m_cellCounts[bucket];
            }
        }
    };

    for (auto [id, carrier] : world.View<KinematicCarrier2D>()) {
        const auto& wt = world.GetWorldTransform(id);
        const f32 cx = wt.x + carrier.bounds.OffsetX();
        const f32 cy = wt.y + carrier.bounds.OffsetY();
        const f32 hw = carrier.bounds.HalfW();
        const f32 hh = carrier.bounds.HalfH();
        tallyBox(cx - hw, cy - hh, cx + hw, cy + hh);
    }

    for (auto [id, motor] : world.View<KinematicMotor2D>()) {
        if (motor.pushboxGroup == 0 || !HasMotorFlag(motor.flags, MotorFlags::SoftPushbox)) {
            continue;
        }
        const auto& wt = world.GetWorldTransform(id);
        const f32 cx = wt.x + motor.bounds.OffsetX();
        const f32 cy = wt.y + motor.bounds.OffsetY();
        const f32 hw = motor.bounds.HalfW();
        const f32 hh = motor.bounds.HalfH();
        tallyBox(cx - hw, cy - hh, cx + hw, cy + hh);
    }

    for (auto [id, rig] : world.View<HitboxRig2D>()) {
        const auto& wt = world.GetWorldTransform(id);
        if ((rig.rigFlags & static_cast<u8>(HitboxRigFlags::HurtboxActive)) != 0) {
            const f32 cx = wt.x + rig.hurtbox.OffsetX();
            const f32 cy = wt.y + rig.hurtbox.OffsetY();
            const f32 hw = rig.hurtbox.HalfW();
            const f32 hh = rig.hurtbox.HalfH();
            tallyBox(cx - hw, cy - hh, cx + hw, cy + hh);
        }
        if ((rig.rigFlags & static_cast<u8>(HitboxRigFlags::HitboxActive)) != 0) {
            const f32 cx = wt.x + rig.primaryHitbox.OffsetX();
            const f32 cy = wt.y + rig.primaryHitbox.OffsetY();
            const f32 hw = rig.primaryHitbox.HalfW();
            const f32 hh = rig.primaryHitbox.HalfH();
            tallyBox(cx - hw, cy - hh, cx + hw, cy + hh);
        }
    }

    u32 runningOffset = 0;
    for (u32 c = 0; c < kGridCells; ++c) {
        const u32 count = m_cellCounts[c];
        m_cellOffsets[c] = static_cast<u16>(std::min<u32>(runningOffset, kMaxCellRefs));
        runningOffset += count;
        m_cellCounts[c] = m_cellOffsets[c];
    }
    m_totalRefs = std::min<u32>(runningOffset, kMaxCellRefs);
    m_cellOffsets[kGridCells] = static_cast<u16>(m_totalRefs);

    const auto scatterBox = [this](
        world::EntityId id,
        f32 minX, f32 minY, f32 maxX, f32 maxY,
        u16 mask, RefKind kind, u8 aux) noexcept {
        const i32 x0 = FastFloorToInt(minX * kInvCellSize);
        const i32 y0 = FastFloorToInt(minY * kInvCellSize);
        const i32 x1 = FastFloorToInt(maxX * kInvCellSize);
        const i32 y1 = FastFloorToInt(maxY * kInvCellSize);
        for (i32 cy = y0; cy <= y1; ++cy) {
            for (i32 cx = x0; cx <= x1; ++cx) {
                const u32 bucket = HashCellCoords(cx, cy);
                const u16 dst = m_cellCounts[bucket];
                if (dst < kMaxCellRefs) {
                    m_sortedEntities[dst] = id;
                    m_boxMinX[dst] = minX;
                    m_boxMinY[dst] = minY;
                    m_boxMaxX[dst] = maxX;
                    m_boxMaxY[dst] = maxY;
                    m_boxMask[dst] = mask;
                    m_boxKind[dst] = static_cast<u8>(kind);
                    m_boxAux[dst]  = aux;
                    m_cellCounts[bucket] = static_cast<u16>(dst + 1u);
                }
            }
        }
    };

    for (auto [id, carrier] : world.View<KinematicCarrier2D>()) {
        const auto& wt = world.GetWorldTransform(id);
        const f32 cx = wt.x + carrier.bounds.OffsetX();
        const f32 cy = wt.y + carrier.bounds.OffsetY();
        const f32 hw = carrier.bounds.HalfW();
        const f32 hh = carrier.bounds.HalfH();
        scatterBox(id, cx - hw, cy - hh, cx + hw, cy + hh, 0x0008u, RefKind::Carrier, 0);
    }

    for (auto [id, motor] : world.View<KinematicMotor2D>()) {
        if (motor.pushboxGroup == 0 || !HasMotorFlag(motor.flags, MotorFlags::SoftPushbox)) {
            continue;
        }
        const auto& wt = world.GetWorldTransform(id);
        const f32 cx = wt.x + motor.bounds.OffsetX();
        const f32 cy = wt.y + motor.bounds.OffsetY();
        const f32 hw = motor.bounds.HalfW();
        const f32 hh = motor.bounds.HalfH();
        scatterBox(id, cx - hw, cy - hh, cx + hw, cy + hh, 0, RefKind::Pushbox, motor.pushboxGroup);
    }

    for (auto [id, rig] : world.View<HitboxRig2D>()) {
        const auto& wt = world.GetWorldTransform(id);
        if ((rig.rigFlags & static_cast<u8>(HitboxRigFlags::HurtboxActive)) != 0) {
            const f32 cx = wt.x + rig.hurtbox.OffsetX();
            const f32 cy = wt.y + rig.hurtbox.OffsetY();
            const f32 hw = rig.hurtbox.HalfW();
            const f32 hh = rig.hurtbox.HalfH();
            scatterBox(id, cx - hw, cy - hh, cx + hw, cy + hh, rig.hurtLayerMask, RefKind::Hurtbox, rig.rigFlags);
        }
        if ((rig.rigFlags & static_cast<u8>(HitboxRigFlags::HitboxActive)) != 0) {
            const f32 cx = wt.x + rig.primaryHitbox.OffsetX();
            const f32 cy = wt.y + rig.primaryHitbox.OffsetY();
            const f32 hw = rig.primaryHitbox.HalfW();
            const f32 hh = rig.primaryHitbox.HalfH();
            scatterBox(id, cx - hw, cy - hh, cx + hw, cy + hh, rig.hitLayerMask, RefKind::Hitbox, rig.rigFlags);
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

    const f32 sweepMinX = std::min(centerX, centerX + deltaX) - halfW - 0.02f;
    const f32 sweepMinY = std::min(centerY, centerY + deltaY) - halfH - 0.02f;
    const f32 sweepMaxX = std::max(centerX, centerX + deltaX) + halfW + 0.02f;
    const f32 sweepMaxY = std::max(centerY, centerY + deltaY) + halfH + 0.02f;

    if (m_mapHeader != nullptr) {
        const i32 tx0 = std::max(0, FastFloorToInt((sweepMinX - m_mapHeader->originX) * m_invTileSize));
        const i32 ty0 = std::max(0, FastFloorToInt((sweepMinY - m_mapHeader->originY) * m_invTileSize));
        const i32 maxTileX = static_cast<i32>(m_mapHeader->chunkCountX) * 8 - 1;
        const i32 maxTileY = static_cast<i32>(m_mapHeader->chunkCountY) * 8 - 1;
        const i32 tx1 = std::min(maxTileX, FastFloorToInt((sweepMaxX - m_mapHeader->originX) * m_invTileSize));
        const i32 ty1 = std::min(maxTileY, FastFloorToInt((sweepMaxY - m_mapHeader->originY) * m_invTileSize));

        for (i32 ty = ty0; ty <= ty1; ++ty) {
            const i32 chunkY = ty >> 3;
            const u32 localY = static_cast<u32>(ty & 7);

            for (i32 tx = tx0; tx <= tx1; ++tx) {
                const i32 chunkX = tx >> 3;
                const CookedCollisionChunk8x8& chunk =
                    m_chunks[chunkY * static_cast<i32>(m_mapHeader->chunkCountX) + chunkX];

                if (chunk.solidBitmask == 0ULL) {
                    tx = ((chunkX + 1) << 3) - 1;
                    continue;
                }

                const u32 localIdx = (localY << 3u) | static_cast<u32>(tx & 7);
                const u64 bit = 1ULL << localIdx;
                if ((chunk.solidBitmask & bit) == 0ULL) {
                    continue;
                }

                const CookedTileCell cell = ((chunk.pureFullBlockBitmask & bit) != 0ULL)
                    ? CookedTileCell{TileShape::FullSolid, TileMaterialFlags::SafeAnchor}
                    : m_fineTiles[chunk.fineTileBaseIndex + localIdx];

                const f32 tileMinX = m_mapHeader->originX + static_cast<f32>(tx) * m_tileSize;
                const f32 tileMinY = m_mapHeader->originY + static_cast<f32>(ty) * m_tileSize;
                const f32 tileMaxX = tileMinX + m_tileSize;
                const f32 tileMaxY = tileMinY + m_tileSize;

                if (cell.shape == TileShape::OneWayTop) {
                    if (ignoreOneWay || deltaY > 0.0f) {
                        continue;
                    }
                    const f32 actorBottom = centerY - halfH;
                    if (actorBottom < tileMaxY - 0.004f) {
                        continue;
                    }
                }

                if (cell.shape == TileShape::Slope45Asc || cell.shape == TileShape::Slope45Desc) {
                    const f32 footX = std::clamp(centerX + deltaX, tileMinX, tileMaxX);
                    const f32 localFracX = (footX - tileMinX) * m_invTileSize;
                    const f32 slopeTopY = (cell.shape == TileShape::Slope45Asc)
                        ? (tileMinY + localFracX * m_tileSize)
                        : (tileMaxY - localFracX * m_tileSize);
                    const f32 endBottomY = (centerY + deltaY) - halfH;
                    if (endBottomY <= slopeTopY && (centerY - halfH) >= tileMinY - 0.1f) {
                        const f32 reqCenterY = slopeTopY + halfH;
                        const f32 denom = std::abs(deltaY) > kRayEps ? deltaY : -1.0f;
                        const f32 t = std::clamp((reqCenterY - centerY) / denom, 0.0f, 1.0f);
                        if (t < best.toi) {
                            best.toi = t;
                            best.normalX = (cell.shape == TileShape::Slope45Asc) ? -0.70710678f : 0.70710678f;
                            best.normalY = 0.70710678f;
                            best.tileShape = cell.shape;
                            best.material = cell.material;
                            best.carrierId = world::kNullEntity;
                            best.hit = true;
                        }
                    }
                    continue;
                }

                f32 tHit = 1.0f;
                f32 nx = 0.0f;
                f32 ny = 0.0f;
                if (TestMinkowskiSlab(
                        centerX, centerY, deltaX, deltaY,
                        tileMinX - halfW, tileMinY - halfH,
                        tileMaxX + halfW, tileMaxY + halfH,
                        tHit, nx, ny)) {
                    if (cell.shape == TileShape::OneWayTop && ny <= 0.0f) {
                        continue;
                    }
                    if (tHit < best.toi) {
                        best.toi = tHit;
                        best.normalX = nx;
                        best.normalY = ny;
                        best.tileShape = cell.shape;
                        best.material = cell.material;
                        best.carrierId = world::kNullEntity;
                        best.hit = true;
                    }
                }
            }
        }
    }

    if ((solidMask & 0x0008u) != 0 && m_cellOffsets != nullptr) {
        const i32 cx0 = FastFloorToInt(sweepMinX * kInvCellSize);
        const i32 cy0 = FastFloorToInt(sweepMinY * kInvCellSize);
        const i32 cx1 = FastFloorToInt(sweepMaxX * kInvCellSize);
        const i32 cy1 = FastFloorToInt(sweepMaxY * kInvCellSize);

        for (i32 cy = cy0; cy <= cy1; ++cy) {
            for (i32 cx = cx0; cx <= cx1; ++cx) {
                const u32 bucket = HashCellCoords(cx, cy);
                const u16 begin = m_cellOffsets[bucket];
                const u16 end   = m_cellOffsets[bucket + 1u];
                for (u16 idx = begin; idx < end; ++idx) {
                    if (m_boxKind[idx] != static_cast<u8>(RefKind::Carrier) ||
                        m_sortedEntities[idx] == ignoreEntity) {
                        continue;
                    }
                    f32 tHit = 1.0f;
                    f32 nx = 0.0f;
                    f32 ny = 0.0f;
                    if (TestMinkowskiSlab(
                            centerX, centerY, deltaX, deltaY,
                            m_boxMinX[idx] - halfW, m_boxMinY[idx] - halfH,
                            m_boxMaxX[idx] + halfW, m_boxMaxY[idx] + halfH,
                            tHit, nx, ny)) {
                        if (tHit < best.toi) {
                            best.toi = tHit;
                            best.normalX = nx;
                            best.normalY = ny;
                            best.tileShape = TileShape::FullSolid;
                            best.material = TileMaterialFlags::None;
                            best.carrierId = m_sortedEntities[idx];
                            best.hit = true;
                        }
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
    const i32 tx = FastFloorToInt((worldX - m_mapHeader->originX) * m_invTileSize);
    const i32 ty = FastFloorToInt((worldY - m_mapHeader->originY) * m_invTileSize);
    const CookedTileCell cell = GetTileAt(tx, ty);
    if (cell.shape != TileShape::FullSolid) {
        return false;
    }
    if (requireSafeAnchor) {
        const u8 mat = static_cast<u8>(cell.material);
        const bool isSafe = (mat & static_cast<u8>(TileMaterialFlags::SafeAnchor)) != 0 &&
                            (mat & static_cast<u8>(TileMaterialFlags::Hazard)) == 0;
        return isSafe;
    }
    return true;
}

bool SpatialIndex2D::DetectLedgeCorner(
    f32 chestWallX, f32 headTopY, f32 clearanceCenterX) const noexcept {
    if (m_mapHeader == nullptr) {
        return false;
    }
    const i32 wallTx = FastFloorToInt((chestWallX - m_mapHeader->originX) * m_invTileSize);
    const i32 headTy = FastFloorToInt((headTopY - m_mapHeader->originY) * m_invTileSize);
    const i32 clearTx = FastFloorToInt((clearanceCenterX - m_mapHeader->originX) * m_invTileSize);

    const CookedTileCell chestCell = GetTileAt(wallTx, headTy - 1);
    const CookedTileCell aboveWallCell = GetTileAt(wallTx, headTy);
    const CookedTileCell headClearCell = GetTileAt(clearTx, headTy);

    return (chestCell.shape == TileShape::FullSolid) &&
           (aboveWallCell.shape == TileShape::Empty) &&
           (headClearCell.shape == TileShape::Empty);
}

ZoneSample2D SpatialIndex2D::SampleEnvironmentalZones(f32 centerX, f32 centerY) const noexcept {
    ZoneSample2D out{};
    if (m_mapHeader == nullptr || m_zones == nullptr) {
        return out;
    }
    u8 highestPrio = 0;
    for (u32 i = 0; i < m_mapHeader->zoneCount; ++i) {
        const EnvironmentalZone2D& z = m_zones[i];
        if (centerX >= z.minX && centerX <= z.maxX &&
            centerY >= z.minY && centerY <= z.maxY) {
            out.forceVx += static_cast<f32>(z.forceVx_q8) * (1.0f / 256.0f);
            out.forceVy += static_cast<f32>(z.forceVy_q8) * (1.0f / 256.0f);
            if (z.priority >= highestPrio) {
                highestPrio = z.priority;
                out.dragScale = static_cast<f32>(z.dragScale_q8) * (1.0f / 256.0f);
            }
            if ((z.zoneFlags & 0x01u) != 0) {
                out.submerged = true;
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
    if (pushboxGroup == 0 || m_cellOffsets == nullptr) {
        return;
    }
    const u32 bucket = HashCellCoords(
        FastFloorToInt(centerX * kInvCellSize),
        FastFloorToInt(centerY * kInvCellSize));
    const u16 begin = m_cellOffsets[bucket];
    const u16 end   = m_cellOffsets[bucket + 1u];

    const f32 minX = centerX - halfW;
    const f32 maxX = centerX + halfW;
    const f32 minY = centerY - halfH;
    const f32 maxY = centerY + halfH;

    f32 accumPushVx = 0.0f;
    for (u16 idx = begin; idx < end; ++idx) {
        if (m_boxKind[idx] != static_cast<u8>(RefKind::Pushbox) ||
            m_boxAux[idx] != pushboxGroup ||
            m_sortedEntities[idx] == selfId) {
            continue;
        }
        if (maxX <= m_boxMinX[idx] || minX >= m_boxMaxX[idx] ||
            maxY <= m_boxMinY[idx] || minY >= m_boxMaxY[idx]) {
            continue;
        }
        const f32 otherCenterX = 0.5f * (m_boxMinX[idx] + m_boxMaxX[idx]);
        const f32 diffX = centerX - otherCenterX;
        const f32 combinedHalfW = halfW + 0.5f * (m_boxMaxX[idx] - m_boxMinX[idx]);
        const f32 overlap = std::max(0.0f, combinedHalfW - std::abs(diffX));
        const f32 dir = (diffX > 0.0001f) ? 1.0f : ((diffX < -0.0001f) ? -1.0f :
                        (selfId.index < m_sortedEntities[idx].index ? -1.0f : 1.0f));
        accumPushVx += dir * std::min(overlap * 8.0f, 3.5f);
    }
    inoutSepVx += std::clamp(accumPushVx, -4.0f, 4.0f);
}

RayHit2D SpatialIndex2D::Raycast(
    f32 originX, f32 originY,
    f32 dirX, f32 dirY,
    f32 maxDist,
    u16 layerMask,
    world::EntityId ignoreId) const noexcept {
    RayHit2D result{};
    result.distance = maxDist;

    const f32 deltaX = dirX * maxDist;
    const f32 deltaY = dirY * maxDist;
    const SlabImpact2D impact = SweepAabb(
        ignoreId, originX, originY, 0.001f, 0.001f, deltaX, deltaY, layerMask, true);

    if (impact.hit) {
        result.hit = true;
        result.distance = impact.toi * maxDist;
        result.pointX = originX + deltaX * impact.toi;
        result.pointY = originY + deltaY * impact.toi;
        result.normalX = impact.normalX;
        result.normalY = impact.normalY;
        result.entity = impact.carrierId;
        result.layer = 1u;
    }
    return result;
}

u32 SpatialIndex2D::GatherCombatOverlaps(
    const world::World& world,
    const assets::AssetBank& assetBank,
    std::span<ResolvedHitEvent> outHits) const noexcept {
    if (m_cellOffsets == nullptr || outHits.empty()) {
        return 0;
    }

    u32 hitCount = 0;

    for (auto [attackerId, attackerRig] : world.View<HitboxRig2D>()) {
        if ((attackerRig.rigFlags & static_cast<u8>(HitboxRigFlags::HitboxActive)) == 0) {
            continue;
        }

        const auto& atkWt = world.GetWorldTransform(attackerId);
        f32 signX = 1.0f;
        if (const auto* motion = world.TryGet<MotionStack2D>(attackerId)) {
            signX = static_cast<f32>(motion->FacingSign());
        } else if (atkWt.scaleX < 0.0f) {
            signX = -1.0f;
        }

        const f32 unionCx = atkWt.x + attackerRig.primaryHitbox.OffsetX() * signX;
        const f32 unionCy = atkWt.y + attackerRig.primaryHitbox.OffsetY();
        const f32 unionHw = attackerRig.primaryHitbox.HalfW();
        const f32 unionHh = attackerRig.primaryHitbox.HalfH();
        const f32 atkMinX = unionCx - unionHw;
        const f32 atkMinY = unionCy - unionHh;
        const f32 atkMaxX = unionCx + unionHw;
        const f32 atkMaxY = unionCy + unionHh;

        const CookedAnimRigHeader* rigHdr = nullptr;
        const CookedHitboxSlice* activeSlice = nullptr;
        const CookedHitboxSocket* sockets = nullptr;

        if (attackerRig.activeSliceIndex != 0xFFu) {
            if (const auto* fsm = world.TryGet<StateMachine2D>(attackerId)) {
                const assets::BlobRecord& blob = assetBank.GetBlob(fsm->rigBlob);
                if (blob.data != nullptr && blob.size >= sizeof(CookedAnimRigHeader)) {
                    const auto* hdr = reinterpret_cast<const CookedAnimRigHeader*>(blob.data);
                    if (hdr->magic == CookedAnimRigHeader::kMagic &&
                        attackerRig.activeSliceIndex < hdr->hitboxSliceCount) {
                        rigHdr = hdr;
                        const auto* sliceTable = reinterpret_cast<const CookedHitboxSlice*>(
                            blob.data + hdr->hitboxSliceOffset);
                        activeSlice = &sliceTable[attackerRig.activeSliceIndex];
                        sockets = reinterpret_cast<const CookedHitboxSocket*>(
                            blob.data + hdr->hitboxSocketOffset);
                    }
                }
            }
        }

        const i32 cx0 = FastFloorToInt(atkMinX * kInvCellSize);
        const i32 cy0 = FastFloorToInt(atkMinY * kInvCellSize);
        const i32 cx1 = FastFloorToInt(atkMaxX * kInvCellSize);
        const i32 cy1 = FastFloorToInt(atkMaxY * kInvCellSize);

        for (i32 cy = cy0; cy <= cy1; ++cy) {
            for (i32 cx = cx0; cx <= cx1; ++cx) {
                const u32 bucket = HashCellCoords(cx, cy);
                const u16 begin = m_cellOffsets[bucket];
                const u16 end   = m_cellOffsets[bucket + 1u];

                for (u16 idx = begin; idx < end; ++idx) {
                    const world::EntityId defenderId = m_sortedEntities[idx];
                    if (defenderId == attackerId) {
                        continue;
                    }

                    const u8 kind = m_boxKind[idx];
                    if (kind != static_cast<u8>(RefKind::Hurtbox) &&
                        kind != static_cast<u8>(RefKind::Hitbox)) {
                        continue;
                    }

                    if ((attackerRig.hitLayerMask & m_boxMask[idx]) == 0) {
                        continue;
                    }

                    if (attackerRig.HasAlreadyHit(defenderId)) {
                        continue;
                    }

                    if (atkMaxX < m_boxMinX[idx] || atkMinX > m_boxMaxX[idx] ||
                        atkMaxY < m_boxMinY[idx] || atkMinY > m_boxMaxY[idx]) {
                        continue;
                    }

                    const bool isClash = (kind == static_cast<u8>(RefKind::Hitbox));
                    if (isClash && ((attackerRig.rigFlags & static_cast<u8>(HitboxRigFlags::ClashParryable)) == 0 ||
                                    (m_boxAux[idx] & static_cast<u8>(HitboxRigFlags::ClashParryable)) == 0)) {
                        continue;
                    }

                    ResolvedHitEvent ev{};
                    ev.attacker = attackerId;
                    ev.defender = defenderId;
                    ev.contactX = 0.5f * (std::max(atkMinX, m_boxMinX[idx]) + std::min(atkMaxX, m_boxMaxX[idx]));
                    ev.contactY = 0.5f * (std::max(atkMinY, m_boxMinY[idx]) + std::min(atkMaxY, m_boxMaxY[idx]));
                    ev.damageMultiplier_q8 = 256;
                    ev.knockbackAngleDeg = signX >= 0.0f ? i16{25} : i16{155};
                    ev.knockbackSpeed_q2 = 44;
                    ev.hitstopTicks = 6;
                    ev.socketFlags = m_boxAux[idx];
                    ev.isClashParry = isClash ? u8{1} : u8{0};

                    if (rigHdr != nullptr && activeSlice != nullptr && sockets != nullptr && activeSlice->socketCount > 0) {
                        bool matchedSubSocket = false;
                        u8 bestPrio = 0;
                        for (u8 s = 0; s < activeSlice->socketCount; ++s) {
                            const CookedHitboxSocket& sock = sockets[activeSlice->firstSocketIndex + s];
                            const f32 scx = atkWt.x + sock.localBox.OffsetX() * signX;
                            const f32 scy = atkWt.y + sock.localBox.OffsetY();
                            const f32 shw = sock.localBox.HalfW();
                            const f32 shh = sock.localBox.HalfH();
                            if (scx + shw >= m_boxMinX[idx] && scx - shw <= m_boxMaxX[idx] &&
                                scy + shh >= m_boxMinY[idx] && scy - shh <= m_boxMaxY[idx]) {
                                if (!matchedSubSocket || sock.priority >= bestPrio) {
                                    matchedSubSocket = true;
                                    bestPrio = sock.priority;
                                    ev.damageMultiplier_q8 = sock.damageMultiplier_q8;
                                    ev.knockbackAngleDeg = signX >= 0.0f
                                        ? sock.knockbackAngleDeg
                                        : static_cast<i16>(180 - sock.knockbackAngleDeg);
                                    ev.knockbackSpeed_q2 = sock.knockbackSpeed_q2;
                                    ev.hitstopTicks = sock.hitstopTicks;
                                    ev.socketFlags |= sock.socketFlags;
                                }
                            }
                        }
                        if (!matchedSubSocket) {
                            continue;
                        }
                    }

                    if (hitCount < outHits.size()) {
                        outHits[hitCount++] = ev;
                    }
                }
            }
        }
    }

    if (hitCount > 1) {
        std::sort(outHits.begin(), outHits.begin() + hitCount,
            [](const ResolvedHitEvent& a, const ResolvedHitEvent& b) noexcept {
                if (a.defender.index != b.defender.index) return a.defender.index < b.defender.index;
                if (a.attacker.index != b.attacker.index) return a.attacker.index < b.attacker.index;
                return a.damageMultiplier_q8 > b.damageMultiplier_q8;
            });
    }

    return hitCount;
}

} // namespace en::gameplay
