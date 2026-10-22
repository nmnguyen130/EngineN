#include "physics/SpatialIndex2D.h"
#include "physics/Narrowphase2D.h"

#include <cstring>

namespace en {

bool SpatialIndex2D::Init(AlignedArena& arena, u16 maxProxies, f32 cellSizeMeters) noexcept {
    if (maxProxies == 0U || cellSizeMeters <= 0.05f) return false;
    auto boxesSpan = arena.Carve<SpatialBox2D>(static_cast<usize>(maxProxies) * 2U);
    auto offsetsSpan = arena.Carve<u32>(kGridCells + 1U);
    auto refsSpan = arena.Carve<u16>(static_cast<usize>(maxProxies) * 8U);

    if (boxesSpan.empty() || offsetsSpan.empty() || refsSpan.empty()) {
        return false;
    }

    m_boxes = boxesSpan.data();
    m_cellOffsets = offsetsSpan.data();
    m_cellBoxIndices = refsSpan.data();
    m_chunkBitplanes = nullptr;
    m_tileFlags = nullptr;
    m_originX = 0.0f;
    m_originY = 0.0f;
    m_tileSize = 1.0f;
    m_invCellSize = 1.0f / cellSizeMeters;
    m_widthTiles = 0U;
    m_heightTiles = 0U;
    m_boxCount = 0U;
    m_maxBoxes = maxProxies;

    Clear();
    return true;
}

void SpatialIndex2D::Clear() noexcept {
    m_boxCount = 0U;
    if (m_cellOffsets) {
        std::memset(m_cellOffsets, 0, (kGridCells + 1U) * sizeof(u32));
    }
}

Result<void, PhysicsErrc> SpatialIndex2D::BindCollisionMap(std::span<const u8> blobBytes) noexcept {
    if (blobBytes.size() < sizeof(CookedCollisionMapHeader)) {
        return Err(PhysicsErrc::CorruptedCollisionBlob);
    }
    CookedCollisionMapHeader hdr{};
    std::memcpy(&hdr, blobBytes.data(), sizeof(CookedCollisionMapHeader));
    if (hdr.magic != CookedCollisionMapHeader::kMagic ||
        hdr.version != CookedCollisionMapHeader::kVersion ||
        hdr.widthTiles == 0U || hdr.heightTiles == 0U || hdr.tileSizeMeters <= 0.01f) {
        return Err(PhysicsErrc::CorruptedCollisionBlob);
    }

    const u16 expectedChunkCols = static_cast<u16>((hdr.widthTiles + 7U) >> 3U);
    const u16 expectedChunkRows = static_cast<u16>((hdr.heightTiles + 7U) >> 3U);
    if (hdr.chunkCols != expectedChunkCols || hdr.chunkRows != expectedChunkRows) {
        return Err(PhysicsErrc::CorruptedCollisionBlob);
    }

    const usize bitplaneBytes = static_cast<usize>(expectedChunkCols) * expectedChunkRows * sizeof(u64);
    const usize tileArrayBytes = static_cast<usize>(hdr.widthTiles) * hdr.heightTiles;
    const usize totalRequired = sizeof(CookedCollisionMapHeader) + bitplaneBytes + tileArrayBytes;
    if (blobBytes.size() < totalRequired || (reinterpret_cast<usize>(blobBytes.data()) & 7U) != 0U) {
        return Err(PhysicsErrc::CorruptedCollisionBlob);
    }

    m_chunkBitplanes = reinterpret_cast<const u64*>(blobBytes.data() + sizeof(CookedCollisionMapHeader));
    m_tileFlags = blobBytes.data() + sizeof(CookedCollisionMapHeader) + bitplaneBytes;
    m_originX = hdr.originX;
    m_originY = hdr.originY;
    m_tileSize = hdr.tileSizeMeters;
    m_widthTiles = hdr.widthTiles;
    m_heightTiles = hdr.heightTiles;
    return {};
}

Result<void, PhysicsErrc> SpatialIndex2D::BuildTileMapFromGrid(
    AlignedArena& arena,
    u16 widthTiles,
    u16 heightTiles,
    f32 tileSizeMeters,
    f32 originX,
    f32 originY,
    std::span<const TileMaterialFlags> tiles) noexcept {
    if (widthTiles == 0U || heightTiles == 0U || tileSizeMeters <= 0.01f ||
        tiles.size() < static_cast<usize>(widthTiles) * heightTiles) {
        return Err(PhysicsErrc::InvalidConfig);
    }

    const u16 chunkCols = static_cast<u16>((widthTiles + 7U) >> 3U);
    const u16 chunkRows = static_cast<u16>((heightTiles + 7U) >> 3U);
    auto bitplaneSpan = arena.Carve<u64>(static_cast<usize>(chunkCols) * chunkRows);
    auto flagSpan = arena.Carve<u8>(static_cast<usize>(widthTiles) * heightTiles);
    if (bitplaneSpan.empty() || flagSpan.empty()) {
        return Err(PhysicsErrc::OutOfMemory);
    }

    std::memset(bitplaneSpan.data(), 0, bitplaneSpan.size_bytes());
    for (u16 ty = 0U; ty < heightTiles; ++ty) {
        for (u16 tx = 0U; tx < widthTiles; ++tx) {
            const usize idx = static_cast<usize>(ty) * widthTiles + tx;
            const u8 f = static_cast<u8>(tiles[idx]);
            flagSpan[idx] = f;
            if (f != 0U) {
                const u16 cx = static_cast<u16>(tx >> 3U);
                const u16 cy = static_cast<u16>(ty >> 3U);
                const u8 bit = static_cast<u8>(((ty & 7U) << 3U) | (tx & 7U));
                bitplaneSpan[static_cast<usize>(cy) * chunkCols + cx] |= (1ULL << bit);
            }
        }
    }

    m_chunkBitplanes = bitplaneSpan.data();
    m_tileFlags = flagSpan.data();
    m_originX = originX;
    m_originY = originY;
    m_tileSize = tileSizeMeters;
    m_widthTiles = widthTiles;
    m_heightTiles = heightTiles;
    return {};
}

TileMaterialFlags SpatialIndex2D::SampleTileCoord(i32 tx, i32 ty) const noexcept {
    if (!m_tileFlags || tx < 0 || ty < 0 ||
        tx >= static_cast<i32>(m_widthTiles) || ty >= static_cast<i32>(m_heightTiles)) {
        return TileMaterialFlags::Empty;
    }
    const u16 cx = static_cast<u16>(tx >> 3);
    const u16 cy = static_cast<u16>(ty >> 3);
    const u8 bit = static_cast<u8>(((ty & 7) << 3) | (tx & 7));
    const u16 chunkCols = ChunkCols();
    if ((m_chunkBitplanes[static_cast<usize>(cy) * chunkCols + cx] & (1ULL << bit)) == 0ULL) {
        return TileMaterialFlags::Empty;
    }
    return static_cast<TileMaterialFlags>(
        m_tileFlags[static_cast<usize>(ty) * m_widthTiles + static_cast<usize>(tx)]);
}

TileMaterialFlags SpatialIndex2D::SampleTileAtWorld(f32 worldX, f32 worldY) const noexcept {
    if (!m_tileFlags || m_tileSize <= 0.0f) return TileMaterialFlags::Empty;
    const f32 invTile = 1.0f / m_tileSize;
    const i32 tx = static_cast<i32>(std::floor((worldX - m_originX) * invTile));
    const i32 ty = static_cast<i32>(std::floor((worldY - m_originY) * invTile));
    return SampleTileCoord(tx, ty);
}

void SpatialIndex2D::RebuildDynamicGrid(const World& world, f32 dt) noexcept {
    if (!m_boxes || !m_cellOffsets || !m_cellBoxIndices) return;
    Clear();
    if ((world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) == 0ULL) return;

    const usize maxRefCap = static_cast<usize>(m_maxBoxes) * 8U;
    struct CellSpanMeta {
        i16 minCX;
        i16 minCY;
        i16 maxCX;
        i16 maxCY;
    };
    static_assert(sizeof(CellSpanMeta) <= sizeof(SpatialBox2D));
    auto* spanMeta = reinterpret_cast<CellSpanMeta*>(m_boxes + m_maxBoxes);

    const auto& colPool = world.GetPool<Collider2D>();
    const auto entities = colPool.Entities();
    const auto cols = colPool.Components();
    const bool hasBodies = (world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) != 0ULL;

    for (usize i = 0U; i < entities.size(); ++i) {
        if (m_boxCount >= m_maxBoxes) break;
        const EntityId id = entities[i];
        const Collider2D& col = cols[i];
        const WorldTransform2D& xf = world.GetWorldByIndex(id.index);

        const u16 bIdx = m_boxCount++;
        SpatialBox2D& box = m_boxes[bIdx];
        box.entity = id;
        box.categoryBits = col.filter.categoryBits;
        box.shape = col.shape;
        box.flags = col.flags;

        f32 aabbMinX = 0.0f;
        f32 aabbMinY = 0.0f;
        f32 aabbMaxX = 0.0f;
        f32 aabbMaxY = 0.0f;

        if (col.shape == ShapeKind2D::Segment) {
            const Vec2 p0 = xf.TransformPoint(col.localBounds.SegmentP0());
            const Vec2 p1 = xf.TransformPoint(col.localBounds.SegmentP1());
            box.minX = p0.x;
            box.minY = p0.y;
            box.maxX = p1.x;
            box.maxY = p1.y;
            aabbMinX = std::min(p0.x, p1.x);
            aabbMinY = std::min(p0.y, p1.y);
            aabbMaxX = std::max(p0.x, p1.x);
            aabbMaxY = std::max(p0.y, p1.y);
        } else {
            const Vec2 center = xf.TransformPoint({col.localBounds.OffsetX(), col.localBounds.OffsetY()});
            const f32 hw = col.localBounds.HalfW() * std::abs(xf.scaleX);
            const f32 hh = col.localBounds.HalfH() * std::abs(xf.scaleY);
            // Expand OBB AABB by rotation matrix abs values so rotated boxes never clip out of their broadphase bounds
            const f32 extX = (col.shape == ShapeKind2D::Obb)
                                 ? (hw * std::abs(xf.cosRot) + hh * std::abs(xf.sinRot))
                                 : hw;
            const f32 extY = (col.shape == ShapeKind2D::Obb)
                                 ? (hw * std::abs(xf.sinRot) + hh * std::abs(xf.cosRot))
                                 : hh;
            box.minX = center.x - extX;
            box.minY = center.y - extY;
            box.maxX = center.x + extX;
            box.maxY = center.y + extY;
            aabbMinX = box.minX;
            aabbMinY = box.minY;
            aabbMaxX = box.maxX;
            aabbMaxY = box.maxY;

            // Expand broadphase cell span for BulletCCD bodies by their velocity displacement over dt
            if (dt > 0.0f && hasBodies &&
                (world.GetComponentMaskByIndex(id.index) & (1ULL << RigidBody2D::kTypeId)) != 0ULL) {
                const RigidBody2D& rb = world.GetPool<RigidBody2D>().DirectAtEntityIndex(id.index);
                if (rb.IsBullet()) {
                    const f32 dx = rb.vx * dt;
                    const f32 dy = rb.vy * dt;
                    if (dx < 0.0f) aabbMinX += dx; else aabbMaxX += dx;
                    if (dy < 0.0f) aabbMinY += dy; else aabbMaxY += dy;
                }
            }
        }

        const i16 minCX = static_cast<i16>(std::floor(aabbMinX * m_invCellSize));
        const i16 minCY = static_cast<i16>(std::floor(aabbMinY * m_invCellSize));
        const i16 maxCX = static_cast<i16>(std::floor(aabbMaxX * m_invCellSize));
        const i16 maxCY = static_cast<i16>(std::floor(aabbMaxY * m_invCellSize));
        const i16 clampedMaxCX = static_cast<i16>(std::min<i32>(maxCX, minCX + 3));
        const i16 clampedMaxCY = static_cast<i16>(std::min<i32>(maxCY, minCY + 3));
        spanMeta[bIdx] = {minCX, minCY, clampedMaxCX, clampedMaxCY};

        for (i16 y = minCY; y <= clampedMaxCY; ++y) {
            for (i16 x = minCX; x <= clampedMaxCX; ++x) {
                const u32 cell = HashCellCoords(x, y);
                ++m_cellOffsets[cell];
            }
        }
    }

    // Pass 2: Exclusive prefix sum over m_cellOffsets[0..kGridCells]
    u32 runningOffset = 0U;
    for (u32 c = 0U; c < kGridCells; ++c) {
        const u32 count = m_cellOffsets[c];
        m_cellOffsets[c] = runningOffset;
        runningOffset = std::min<u32>(runningOffset + count, static_cast<u32>(maxRefCap));
    }
    m_cellOffsets[kGridCells] = runningOffset;

    // Pass 3: Scatter box indices into m_cellBoxIndices, then restore prefix offsets
    for (u16 bIdx = 0U; bIdx < m_boxCount; ++bIdx) {
        const CellSpanMeta& meta = spanMeta[bIdx];
        for (i16 y = meta.minCY; y <= meta.maxCY; ++y) {
            for (i16 x = meta.minCX; x <= meta.maxCX; ++x) {
                const u32 cell = HashCellCoords(x, y);
                const u32 writePos = m_cellOffsets[cell];
                if (writePos < maxRefCap) {
                    m_cellBoxIndices[writePos] = bIdx;
                    m_cellOffsets[cell] = writePos + 1U;
                }
            }
        }
    }

    for (u32 c = kGridCells; c > 0U; --c) {
        m_cellOffsets[c] = m_cellOffsets[c - 1U];
    }
    m_cellOffsets[0] = 0U;
}

RayHit2D SpatialIndex2D::Raycast(
    f32 originX,
    f32 originY,
    f32 dirX,
    f32 dirY,
    f32 maxDist,
    u16 categoryMask,
    EntityId ignoreEntity) const noexcept {
    RayHit2D bestHit{};
    bestHit.fraction = 1.0f;
    bestHit.distance = maxDist;

    const f32 endX = originX + dirX * maxDist;
    const f32 endY = originY + dirY * maxDist;
    const f32 rayMinX = std::min(originX, endX);
    const f32 rayMaxX = std::max(originX, endX);
    const f32 rayMinY = std::min(originY, endY);
    const f32 rayMaxY = std::max(originY, endY);

    // 1. Test against static tilemap along the ray bounding box
    if (m_tileFlags && m_tileSize > 0.0f && (categoryMask & 0x0001U) != 0U) {
        const f32 invTile = 1.0f / m_tileSize;
        const i32 minTX = std::max<i32>(0, static_cast<i32>(std::floor((rayMinX - m_originX) * invTile)));
        const i32 maxTX = std::min<i32>(static_cast<i32>(m_widthTiles) - 1, static_cast<i32>(std::floor((rayMaxX - m_originX) * invTile)));
        const i32 minTY = std::max<i32>(0, static_cast<i32>(std::floor((rayMinY - m_originY) * invTile)));
        const i32 maxTY = std::min<i32>(static_cast<i32>(m_heightTiles) - 1, static_cast<i32>(std::floor((rayMaxY - m_originY) * invTile)));

        for (i32 ty = minTY; ty <= maxTY; ++ty) {
            for (i32 tx = minTX; tx <= maxTX; ++tx) {
                const TileMaterialFlags flags = SampleTileCoord(tx, ty);
                if (!HasPhysicsFlag(flags, TileMaterialFlags::Solid)) continue;
                const f32 tileMinX = m_originX + static_cast<f32>(tx) * m_tileSize;
                const f32 tileMinY = m_originY + static_cast<f32>(ty) * m_tileSize;
                const RayHit2D h = Narrowphase2D::RaycastAabb(
                    originX, originY, dirX, dirY, bestHit.distance,
                    tileMinX, tileMinY, tileMinX + m_tileSize, tileMinY + m_tileSize);
                if (h.Hit() && h.distance < bestHit.distance) {
                    bestHit = h;
                    bestHit.fraction = (maxDist > 1e-8f) ? (h.distance / maxDist) : 0.0f;
                    bestHit.tileFlags = flags;
                }
            }
        }
    }

    // 2. Probe spatial grid cells covered by the ray segment (falls back to linear scan only if span > 8x8 cells)
    const i32 minCX = static_cast<i32>(std::floor(rayMinX * m_invCellSize));
    const i32 maxCX = static_cast<i32>(std::floor(rayMaxX * m_invCellSize));
    const i32 minCY = static_cast<i32>(std::floor(rayMinY * m_invCellSize));
    const i32 maxCY = static_cast<i32>(std::floor(rayMaxY * m_invCellSize));

    const auto testBox = [&](const SpatialBox2D& b) noexcept {
        if (b.entity == ignoreEntity || (b.categoryBits & categoryMask) == 0U) return;
        RayHit2D h{};
        if (b.shape == ShapeKind2D::Circle) {
            const f32 cx = 0.5f * (b.minX + b.maxX);
            const f32 cy = 0.5f * (b.minY + b.maxY);
            const f32 r = 0.5f * (b.maxX - b.minX);
            h = Narrowphase2D::RaycastCircle(originX, originY, dirX, dirY, bestHit.distance, cx, cy, r);
        } else if (b.shape == ShapeKind2D::Segment) {
            h = Narrowphase2D::RaycastSegment(originX, originY, dirX, dirY, bestHit.distance, {b.minX, b.minY}, {b.maxX, b.maxY});
        } else {
            h = Narrowphase2D::RaycastAabb(
                originX, originY, dirX, dirY, bestHit.distance, b.minX, b.minY, b.maxX, b.maxY);
        }
        if (h.Hit() && h.distance < bestHit.distance) {
            bestHit = h;
            bestHit.fraction = (maxDist > 1e-8f) ? (h.distance / maxDist) : 0.0f;
            bestHit.hitEntity = b.entity;
        }
    };

    if ((maxCX - minCX) <= 8 && (maxCY - minCY) <= 8) {
        for (i32 cy = minCY; cy <= maxCY; ++cy) {
            for (i32 cx = minCX; cx <= maxCX; ++cx) {
                const u32 cell = HashCellCoords(cx, cy);
                const u32 begin = m_cellOffsets[cell];
                const u32 end = m_cellOffsets[cell + 1U];
                for (u32 r = begin; r < end; ++r) {
                    testBox(m_boxes[m_cellBoxIndices[r]]);
                }
            }
        }
    } else {
        for (u16 i = 0U; i < m_boxCount; ++i) {
            testBox(m_boxes[i]);
        }
    }

    return bestHit;
}

SweepHit2D SpatialIndex2D::SweepAabb(
    EntityId ignoreEntity,
    f32 centerX,
    f32 centerY,
    f32 halfW,
    f32 halfH,
    f32 deltaX,
    f32 deltaY,
    u16 solidMask,
    bool ignoreOneWay) const noexcept {
    SweepHit2D best{};
    best.toi = 1.0f;

    const f32 sweepMinX = std::min(centerX, centerX + deltaX) - halfW - 0.05f;
    const f32 sweepMaxX = std::max(centerX, centerX + deltaX) + halfW + 0.05f;
    const f32 sweepMinY = std::min(centerY, centerY + deltaY) - halfH - 0.05f;
    const f32 sweepMaxY = std::max(centerY, centerY + deltaY) + halfH + 0.05f;

    // 1. Sweep against static tilemap
    if (m_tileFlags && m_tileSize > 0.0f && (solidMask & 0x0001U) != 0U) {
        const f32 invTile = 1.0f / m_tileSize;
        const i32 minTX = std::max<i32>(0, static_cast<i32>(std::floor((sweepMinX - m_originX) * invTile)));
        const i32 maxTX = std::min<i32>(static_cast<i32>(m_widthTiles) - 1, static_cast<i32>(std::floor((sweepMaxX - m_originX) * invTile)));
        const i32 minTY = std::max<i32>(0, static_cast<i32>(std::floor((sweepMinY - m_originY) * invTile)));
        const i32 maxTY = std::min<i32>(static_cast<i32>(m_heightTiles) - 1, static_cast<i32>(std::floor((sweepMaxY - m_originY) * invTile)));

        for (i32 ty = minTY; ty <= maxTY; ++ty) {
            for (i32 tx = minTX; tx <= maxTX; ++tx) {
                const TileMaterialFlags flags = SampleTileCoord(tx, ty);
                if (flags == TileMaterialFlags::Empty) continue;
                const f32 tileMinX = m_originX + static_cast<f32>(tx) * m_tileSize;
                const f32 tileMinY = m_originY + static_cast<f32>(ty) * m_tileSize;
                const f32 tileMaxX = tileMinX + m_tileSize;
                const f32 tileMaxY = tileMinY + m_tileSize;

                const bool isOneWay = HasPhysicsFlag(flags, TileMaterialFlags::OneWay);
                if (isOneWay) {
                    if (ignoreOneWay || deltaY > 0.0f || (centerY - halfH) < tileMaxY - 1e-3f) {
                        continue;
                    }
                }

                const SweepHit2D h = Narrowphase2D::SweepAabbVsAabb(
                    centerX, centerY, halfW, halfH, deltaX, deltaY,
                    tileMinX, tileMinY, tileMaxX, tileMaxY);
                if (h.Hit() && (!isOneWay || h.normalY > 0.5f) && h.toi < best.toi) {
                    best = h;
                    best.tileFlags = flags;
                    best.isOneWay = isOneWay ? 1U : 0U;
                }
            }
        }
    }

    // 2. Sweep against dynamic SpatialBox2D proxies via spatial hash cells
    const i16 minCX = static_cast<i16>(std::floor(sweepMinX * m_invCellSize));
    const i16 maxCX = static_cast<i16>(std::min<i32>(static_cast<i32>(std::floor(sweepMaxX * m_invCellSize)), minCX + 4));
    const i16 minCY = static_cast<i16>(std::floor(sweepMinY * m_invCellSize));
    const i16 maxCY = static_cast<i16>(std::min<i32>(static_cast<i32>(std::floor(sweepMaxY * m_invCellSize)), minCY + 4));

    for (i16 cy = minCY; cy <= maxCY; ++cy) {
        for (i16 cx = minCX; cx <= maxCX; ++cx) {
            const u32 cell = HashCellCoords(cx, cy);
            const u32 begin = m_cellOffsets[cell];
            const u32 end = m_cellOffsets[cell + 1U];
            for (u32 r = begin; r < end; ++r) {
                const SpatialBox2D& b = m_boxes[m_cellBoxIndices[r]];
                if (b.entity == ignoreEntity || HasPhysicsFlag(b.flags, ColliderFlags2D::IsSensor)) continue;
                if ((b.categoryBits & solidMask) == 0U) continue;

                const bool isOneWay = HasPhysicsFlag(b.flags, ColliderFlags2D::IsOneWay);
                if (isOneWay && ignoreOneWay) continue;

                SweepHit2D h{};
                if (b.shape == ShapeKind2D::Segment) {
                    h = Narrowphase2D::SweepAabbVsSegment(
                        centerX, centerY, halfW, halfH, deltaX, deltaY,
                        {b.minX, b.minY}, {b.maxX, b.maxY}, isOneWay);
                } else {
                    if (isOneWay && (deltaY > 0.0f || (centerY - halfH) < b.maxY - 1e-3f)) {
                        continue;
                    }
                    h = Narrowphase2D::SweepAabbVsAabb(
                        centerX, centerY, halfW, halfH, deltaX, deltaY,
                        b.minX, b.minY, b.maxX, b.maxY);
                }

                if (h.Hit() && h.toi < best.toi) {
                    best = h;
                    best.hitEntity = b.entity;
                    best.isOneWay = isOneWay ? 1U : 0U;
                }
            }
        }
    }

    return best;
}

u32 SpatialIndex2D::QueryAabb(
    f32 minX,
    f32 minY,
    f32 maxX,
    f32 maxY,
    u16 categoryMask,
    std::span<EntityId> outEntities,
    EntityId ignoreEntity) const noexcept {
    u32 count = 0U;
    const i32 minCX = static_cast<i32>(std::floor(minX * m_invCellSize));
    const i32 maxCX = static_cast<i32>(std::floor(maxX * m_invCellSize));
    const i32 minCY = static_cast<i32>(std::floor(minY * m_invCellSize));
    const i32 maxCY = static_cast<i32>(std::floor(maxY * m_invCellSize));

    const auto checkAndAppend = [&](const SpatialBox2D& b) noexcept {
        if (b.entity == ignoreEntity || (b.categoryBits & categoryMask) == 0U) return;
        if (b.BoundMaxX() < minX || b.BoundMinX() > maxX ||
            b.BoundMaxY() < minY || b.BoundMinY() > maxY) {
            return;
        }
        for (u32 k = 0U; k < std::min<u32>(count, static_cast<u32>(outEntities.size())); ++k) {
            if (outEntities[k] == b.entity) return;
        }
        if (count < outEntities.size()) {
            outEntities[count] = b.entity;
        }
        ++count;
    };

    if ((maxCX - minCX) <= 6 && (maxCY - minCY) <= 6) {
        for (i32 cy = minCY; cy <= maxCY; ++cy) {
            for (i32 cx = minCX; cx <= maxCX; ++cx) {
                const u32 cell = HashCellCoords(cx, cy);
                const u32 begin = m_cellOffsets[cell];
                const u32 end = m_cellOffsets[cell + 1U];
                for (u32 r = begin; r < end; ++r) {
                    checkAndAppend(m_boxes[m_cellBoxIndices[r]]);
                }
            }
        }
    } else {
        for (u16 i = 0U; i < m_boxCount; ++i) {
            checkAndAppend(m_boxes[i]);
        }
    }
    return count;
}

u32 SpatialIndex2D::QueryCircle(
    f32 centerX,
    f32 centerY,
    f32 radius,
    u16 categoryMask,
    std::span<EntityId> outEntities,
    EntityId ignoreEntity) const noexcept {
    u32 count = 0U;
    const f32 rSq = radius * radius;
    for (u16 i = 0U; i < m_boxCount; ++i) {
        const SpatialBox2D& b = m_boxes[i];
        if (b.entity == ignoreEntity || (b.categoryBits & categoryMask) == 0U) continue;
        const f32 closestX = std::clamp(centerX, b.BoundMinX(), b.BoundMaxX());
        const f32 closestY = std::clamp(centerY, b.BoundMinY(), b.BoundMaxY());
        const f32 dx = centerX - closestX;
        const f32 dy = centerY - closestY;
        if (dx * dx + dy * dy <= rSq) {
            if (count < outEntities.size()) {
                outEntities[count] = b.entity;
            }
            ++count;
        }
    }
    return count;
}

} // namespace en
