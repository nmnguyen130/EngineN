#include "physics/SpatialIndex2D.h"

#include "physics/Narrowphase2D.h"

#include <cstring>

namespace en {

namespace {

[[nodiscard]] SweepHit2D SweepCircleVsAabb(
    f32 centerX, f32 centerY, f32 radius,
    f32 deltaX, f32 deltaY,
    f32 minBX, f32 minBY, f32 maxBX, f32 maxBY) noexcept {
    const SweepHit2D aabbHit = Narrowphase2D::SweepAabbVsAabb(
        centerX, centerY, radius, radius, deltaX, deltaY, minBX, minBY, maxBX, maxBY);
    if (!aabbHit.Hit() || aabbHit.startPenetrating != 0U) {
        return aabbHit;
    }

    const f32 hitX = centerX + deltaX * aabbHit.toi;
    const f32 hitY = centerY + deltaY * aabbHit.toi;
    if ((hitX >= minBX && hitX <= maxBX) || (hitY >= minBY && hitY <= maxBY)) {
        return aabbHit;
    }

    const f32 cornerX = (hitX < minBX) ? minBX : maxBX;
    const f32 cornerY = (hitY < minBY) ? minBY : maxBY;
    const f32 moveLen = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    if (moveLen <= 1e-6f) {
        return {};
    }

    const RayHit2D rHit = Narrowphase2D::RaycastCircle(
        centerX, centerY, deltaX / moveLen, deltaY / moveLen, moveLen, cornerX, cornerY, radius);
    if (!rHit.Hit()) {
        return {};
    }
    return {rHit.fraction, rHit.pointX, rHit.pointY, rHit.normalX, rHit.normalY, 0.0f, kNullEntity, TileMaterialFlags::Empty, rHit.startSolid, 1U, 0U};
}

template <typename F>
void VisitCandidateBoxes(
    const SpatialIndex2D& spatial,
    f32 minX, f32 minY, f32 maxX, f32 maxY,
    F&& fn) noexcept {
    const auto boxes = spatial.ActiveBoxes();
    if (boxes.empty()) {
        return;
    }
    const f32 invCell = spatial.InvCellSize();
    const i32 minCX = static_cast<i32>(std::floor(minX * invCell));
    const i32 maxCX = static_cast<i32>(std::floor(maxX * invCell));
    const i32 minCY = static_cast<i32>(std::floor(minY * invCell));
    const i32 maxCY = static_cast<i32>(std::floor(maxY * invCell));

    if ((maxCX - minCX) <= 16 && (maxCY - minCY) <= 16) {
        const auto offsets = spatial.CellOffsets();
        const auto indices = spatial.CellBoxIndices();
        for (i32 cy = minCY; cy <= maxCY; ++cy) {
            for (i32 cx = minCX; cx <= maxCX; ++cx) {
                const u32 cell = SpatialIndex2D::HashCellCoords(cx, cy);
                for (u32 r = offsets[cell], end = offsets[cell + 1U]; r < end; ++r) {
                    fn(boxes[indices[r]]);
                }
            }
        }
        for (const u16 bIdx : spatial.OversizedBoxIndices()) {
            fn(boxes[bIdx]);
        }
    } else {
        for (const SpatialBox2D& b : boxes) {
            fn(b);
        }
    }
}

template <typename F>
void VisitTileRuns(
    const SpatialIndex2D& spatial,
    f32 minX, f32 minY, f32 maxX, f32 maxY,
    F&& fn) noexcept {
    const u16 w = spatial.WidthTiles();
    const u16 h = spatial.HeightTiles();
    const f32 tileSize = spatial.TileSizeMeters();
    if (w == 0U || h == 0U || tileSize <= 0.0f) {
        return;
    }
    const f32 origX = spatial.OriginX();
    const f32 origY = spatial.OriginY();
    const f32 invTile = 1.0f / tileSize;
    const i32 minTX = std::max<i32>(0, static_cast<i32>(std::floor((minX - origX) * invTile)));
    const i32 maxTX = std::min<i32>(static_cast<i32>(w) - 1, static_cast<i32>(std::floor((maxX - origX) * invTile)));
    const i32 minTY = std::max<i32>(0, static_cast<i32>(std::floor((minY - origY) * invTile)));
    const i32 maxTY = std::min<i32>(static_cast<i32>(h) - 1, static_cast<i32>(std::floor((maxY - origY) * invTile)));

    for (i32 ty = minTY; ty <= maxTY; ++ty) {
        const f32 tileMinY = origY + static_cast<f32>(ty) * tileSize;
        const f32 tileMaxY = tileMinY + tileSize;
        for (i32 tx = minTX; tx <= maxTX; ++tx) {
            const TileMaterialFlags flags = spatial.SampleTileCoord(tx, ty);
            if (flags == TileMaterialFlags::Empty) {
                continue;
            }
            i32 runEnd = tx;
            while (runEnd + 1 <= maxTX && spatial.SampleTileCoord(runEnd + 1, ty) == flags) {
                ++runEnd;
            }
            const f32 tileMinX = origX + static_cast<f32>(tx) * tileSize;
            const f32 tileMaxX = origX + static_cast<f32>(runEnd + 1) * tileSize;
            fn(tx, runEnd, ty, tileMinX, tileMinY, tileMaxX, tileMaxY, flags);
            tx = runEnd;
        }
    }
}

void FilterInternalTileSeamHit(
    const SpatialIndex2D& spatial,
    i32 spanStart, i32 runEnd, i32 ty,
    f32 tileMinX, f32 tileMaxX,
    f32 centerX, f32 halfW, f32 deltaX,
    SweepHit2D& ioHit) noexcept {
    const bool topCovered = HasFlag(spatial.SampleTileCoord(spanStart, ty + 1), TileMaterialFlags::Solid) &&
                            HasFlag(spatial.SampleTileCoord(runEnd, ty + 1), TileMaterialFlags::Solid);
    const bool botCovered = HasFlag(spatial.SampleTileCoord(spanStart, ty - 1), TileMaterialFlags::Solid) &&
                            HasFlag(spatial.SampleTileCoord(runEnd, ty - 1), TileMaterialFlags::Solid);
    if ((ioHit.normalY > 0.5f && topCovered) || (ioHit.normalY < -0.5f && botCovered)) {
        if (deltaX < -1e-6f && !HasFlag(spatial.SampleTileCoord(runEnd + 1, ty), TileMaterialFlags::Solid) &&
            (centerX - halfW) >= tileMaxX - 2e-3f) {
            ioHit.normalX = 1.0f;
            ioHit.normalY = 0.0f;
        } else if (deltaX > 1e-6f && !HasFlag(spatial.SampleTileCoord(spanStart - 1, ty), TileMaterialFlags::Solid) &&
                   (centerX + halfW) <= tileMinX + 2e-3f) {
            ioHit.normalX = -1.0f;
            ioHit.normalY = 0.0f;
        } else {
            ioHit.hitValid = 0U;
        }
    }
}

[[nodiscard]] SweepHit2D SweepVolumeCommon(
    const SpatialIndex2D& spatial,
    EntityId ignoreEntity,
    f32 centerX, f32 centerY, f32 halfW, f32 halfH,
    f32 deltaX, f32 deltaY,
    u16 solidMask, bool ignoreOneWay, bool isCircle) noexcept {
    SweepHit2D best{};
    const f32 sweepMinX = std::min(centerX, centerX + deltaX) - halfW - 0.05f;
    const f32 sweepMaxX = std::max(centerX, centerX + deltaX) + halfW + 0.05f;
    const f32 sweepMinY = std::min(centerY, centerY + deltaY) - halfH - 0.05f;
    const f32 sweepMaxY = std::max(centerY, centerY + deltaY) + halfH + 0.05f;

    if ((solidMask & 0x0001U) != 0U) {
        VisitTileRuns(spatial, sweepMinX, sweepMinY, sweepMaxX, sweepMaxY,
            [&](i32 spanStart, i32 runEnd, i32 ty, f32 tileMinX, f32 tileMinY, f32 tileMaxX, f32 tileMaxY, TileMaterialFlags flags) noexcept {
                const bool isOneWay = HasFlag(flags, TileMaterialFlags::OneWay);
                if (isOneWay && (ignoreOneWay || deltaY > 0.0f || (centerY - halfH) < tileMaxY - 1e-3f)) {
                    return;
                }
                SweepHit2D h = isCircle
                    ? SweepCircleVsAabb(centerX, centerY, halfW, deltaX, deltaY, tileMinX, tileMinY, tileMaxX, tileMaxY)
                    : Narrowphase2D::SweepAabbVsAabb(centerX, centerY, halfW, halfH, deltaX, deltaY, tileMinX, tileMinY, tileMaxX, tileMaxY);
                if (h.Hit() && !isOneWay) {
                    FilterInternalTileSeamHit(spatial, spanStart, runEnd, ty, tileMinX, tileMaxX, centerX, halfW, deltaX, h);
                }
                if (h.Hit() && (!isOneWay || h.normalY > 0.5f) && h.toi < best.toi) {
                    best = h;
                    best.tileFlags = flags;
                    best.isOneWay = isOneWay ? 1U : 0U;
                }
            });
    }

    const f32 moveLen = isCircle ? std::sqrt(deltaX * deltaX + deltaY * deltaY) : 0.0f;
    VisitCandidateBoxes(spatial, sweepMinX, sweepMinY, sweepMaxX, sweepMaxY, [&](const SpatialBox2D& b) noexcept {
        if (b.entity == ignoreEntity || b.entity == best.hitEntity ||
            b.IsSensor() || (b.categoryBits & solidMask) == 0U) {
            return;
        }
        if (b.BoundMaxX() < sweepMinX || b.BoundMinX() > sweepMaxX ||
            b.BoundMaxY() < sweepMinY || b.BoundMinY() > sweepMaxY) {
            return;
        }

        const bool isOneWay = b.IsOneWay();
        if (isOneWay && ignoreOneWay) {
            return;
        }

        SweepHit2D h{};
        if (b.shape == ShapeKind2D::Segment) {
            h = Narrowphase2D::SweepAabbVsSegment(
                centerX, centerY, halfW, halfH, deltaX, deltaY, {b.minX, b.minY}, {b.maxX, b.maxY}, isOneWay);
        } else if (isCircle && b.shape == ShapeKind2D::Circle && moveLen > 1e-6f) {
            const RayHit2D rHit = Narrowphase2D::RaycastCircle(
                centerX, centerY, deltaX / moveLen, deltaY / moveLen, moveLen,
                0.5f * (b.minX + b.maxX), 0.5f * (b.minY + b.maxY), halfW + 0.5f * (b.maxX - b.minX));
            if (rHit.Hit()) {
                h = {rHit.fraction, rHit.pointX, rHit.pointY, rHit.normalX, rHit.normalY, 0.0f, kNullEntity, TileMaterialFlags::Empty, rHit.startSolid, 1U, 0U};
            }
        } else {
            if (isOneWay && (deltaY > 0.0f || (centerY - halfH) < b.maxY - 1e-3f)) {
                return;
            }
            h = isCircle
                ? SweepCircleVsAabb(centerX, centerY, halfW, deltaX, deltaY, b.minX, b.minY, b.maxX, b.maxY)
                : Narrowphase2D::SweepAabbVsAabb(centerX, centerY, halfW, halfH, deltaX, deltaY, b.minX, b.minY, b.maxX, b.maxY);
        }

        if (h.Hit() && h.toi < best.toi) {
            best = h;
            best.hitEntity = b.entity;
            best.isOneWay = isOneWay ? 1U : 0U;
        }
    });

    return best;
}

template <typename Pred>
[[nodiscard]] u32 QueryBoxesCommon(
    const SpatialIndex2D& spatial,
    f32 minX, f32 minY, f32 maxX, f32 maxY,
    u16 categoryMask, std::span<EntityId> outEntities,
    EntityId ignoreEntity, bool includeSensors, Pred&& pred) noexcept {
    u32 count = 0U;
    VisitCandidateBoxes(spatial, minX, minY, maxX, maxY, [&](const SpatialBox2D& b) noexcept {
        if (b.entity == ignoreEntity || (b.categoryBits & categoryMask) == 0U ||
            (!includeSensors && b.IsSensor()) || !pred(b)) {
            return;
        }
        const u32 limit = std::min<u32>(count, static_cast<u32>(outEntities.size()));
        for (u32 k = 0U; k < limit; ++k) {
            if (outEntities[k] == b.entity) {
                return;
            }
        }
        if (count < outEntities.size()) {
            outEntities[count] = b.entity;
        }
        ++count;
    });
    return count;
}

} // namespace

bool SpatialIndex2D::Init(AlignedArena& arena, u16 maxProxies, f32 cellSizeMeters) noexcept {
    if (maxProxies == 0U || cellSizeMeters <= 0.05f) {
        return false;
    }
    const u32 marker = arena.SaveMarker();
    auto boxesSpan = arena.Carve<SpatialBox2D>(static_cast<usize>(maxProxies) * 2U);
    auto offsetsSpan = arena.Carve<u32>(kGridCells + 2U);
    auto refsSpan = arena.Carve<u16>(static_cast<usize>(maxProxies) * 32U);
    if (boxesSpan.empty() || offsetsSpan.empty() || refsSpan.empty()) {
        arena.RollbackToMarker(marker);
        return false;
    }

    *this = SpatialIndex2D{};
    m_boxes = boxesSpan.data();
    m_cellOffsets = offsetsSpan.data();
    m_cellBoxIndices = refsSpan.data();
    m_invCellSize = 1.0f / cellSizeMeters;
    m_maxBoxes = maxProxies;
    Clear();
    return true;
}

void SpatialIndex2D::Clear() noexcept {
    m_boxCount = 0U;
    if (m_cellOffsets) {
        std::memset(m_cellOffsets, 0, (kGridCells + 2U) * sizeof(u32));
    }
}

Result<void, PhysicsErrc> SpatialIndex2D::BindCollisionMap(
    AlignedArena& arena,
    std::span<const u8> blobBytes) noexcept {
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

    const usize bitplaneWords = static_cast<usize>(expectedChunkCols) * expectedChunkRows;
    const usize bitplaneBytes = bitplaneWords * sizeof(u64);
    const usize tileArrayBytes = static_cast<usize>(hdr.widthTiles) * hdr.heightTiles;
    const usize payloadBytes = bitplaneBytes + tileArrayBytes;
    if (blobBytes.size() < sizeof(CookedCollisionMapHeader) + payloadBytes) {
        return Err(PhysicsErrc::CorruptedCollisionBlob);
    }

    if (hdr.payloadChecksum != 0U &&
        ComputeCrc32(blobBytes.subspan(sizeof(CookedCollisionMapHeader), payloadBytes)) != hdr.payloadChecksum) {
        return Err(PhysicsErrc::CorruptedCollisionBlob);
    }

    const auto* payload = blobBytes.data() + sizeof(CookedCollisionMapHeader);
    if ((reinterpret_cast<uintptr_t>(payload) & 7U) == 0U) {
        m_chunkBitplanes = reinterpret_cast<const u64*>(payload);
        m_tileFlags = payload + bitplaneBytes;
    } else {
        const u32 marker = arena.SaveMarker();
        auto bitplaneSpan = arena.Carve<u64>(bitplaneWords);
        auto flagSpan = arena.Carve<u8>(tileArrayBytes);
        if (bitplaneSpan.empty() || flagSpan.empty()) {
            arena.RollbackToMarker(marker);
            return Err(PhysicsErrc::OutOfMemory);
        }
        std::memcpy(bitplaneSpan.data(), payload, bitplaneBytes);
        std::memcpy(flagSpan.data(), payload + bitplaneBytes, tileArrayBytes);
        m_chunkBitplanes = bitplaneSpan.data();
        m_tileFlags = flagSpan.data();
    }

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
    const usize bitplaneWords = static_cast<usize>(chunkCols) * chunkRows;
    const usize tileArrayBytes = static_cast<usize>(widthTiles) * heightTiles;

    u64* dstBitplanes = nullptr;
    u8* dstFlags = nullptr;
    if (m_chunkBitplanes && m_tileFlags &&
        static_cast<usize>(ChunkCols()) * ChunkRows() >= bitplaneWords &&
        static_cast<usize>(m_widthTiles) * m_heightTiles >= tileArrayBytes) {
        dstBitplanes = const_cast<u64*>(m_chunkBitplanes);
        dstFlags = const_cast<u8*>(m_tileFlags);
    } else {
        const u32 marker = arena.SaveMarker();
        auto bitplaneSpan = arena.Carve<u64>(bitplaneWords);
        auto flagSpan = arena.Carve<u8>(tileArrayBytes);
        if (bitplaneSpan.empty() || flagSpan.empty()) {
            arena.RollbackToMarker(marker);
            return Err(PhysicsErrc::OutOfMemory);
        }
        dstBitplanes = bitplaneSpan.data();
        dstFlags = flagSpan.data();
    }

    std::memset(dstBitplanes, 0, bitplaneWords * sizeof(u64));
    for (u16 ty = 0U; ty < heightTiles; ++ty) {
        for (u16 tx = 0U; tx < widthTiles; ++tx) {
            const usize idx = static_cast<usize>(ty) * widthTiles + tx;
            const u8 f = static_cast<u8>(tiles[idx]);
            dstFlags[idx] = f;
            if (f != 0U) {
                const u8 bit = static_cast<u8>(((ty & 7U) << 3U) | (tx & 7U));
                dstBitplanes[static_cast<usize>(ty >> 3U) * chunkCols + (tx >> 3U)] |= (1ULL << bit);
            }
        }
    }

    m_chunkBitplanes = dstBitplanes;
    m_tileFlags = dstFlags;
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
    const u8 bit = static_cast<u8>(((ty & 7) << 3) | (tx & 7));
    if ((m_chunkBitplanes[static_cast<usize>(ty >> 3) * ChunkCols() + static_cast<usize>(tx >> 3)] & (1ULL << bit)) == 0ULL) {
        return TileMaterialFlags::Empty;
    }
    return static_cast<TileMaterialFlags>(m_tileFlags[static_cast<usize>(ty) * m_widthTiles + static_cast<usize>(tx)]);
}

TileMaterialFlags SpatialIndex2D::SampleTileAtWorld(f32 worldX, f32 worldY) const noexcept {
    if (!m_tileFlags || m_tileSize <= 0.0f) {
        return TileMaterialFlags::Empty;
    }
    const f32 invTile = 1.0f / m_tileSize;
    return SampleTileCoord(
        static_cast<i32>(std::floor((worldX - m_originX) * invTile)),
        static_cast<i32>(std::floor((worldY - m_originY) * invTile)));
}

void SpatialIndex2D::RebuildDynamicGrid(const World& world, f32 dt) noexcept {
    if (!m_boxes || !m_cellOffsets || !m_cellBoxIndices) {
        return;
    }
    Clear();
    if ((world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) == 0ULL) {
        return;
    }

    const usize maxRefCap = static_cast<usize>(m_maxBoxes) * 32U;
    auto* spanMeta = reinterpret_cast<i16*>(m_boxes + m_maxBoxes);

    const auto& colPool = world.GetPool<Collider2D>();
    const auto entities = colPool.Entities();
    const auto cols = colPool.Components();
    const bool hasBodies = (world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) != 0ULL;

    for (usize i = 0U; i < entities.size() && m_boxCount < m_maxBoxes; ++i) {
        const EntityId id = entities[i];
        const Collider2D& col = cols[i];
        const RigidBody2D* rb = (hasBodies && (world.GetComponentMaskByIndex(id.index) & (1ULL << RigidBody2D::kTypeId)) != 0ULL)
            ? &world.GetPool<RigidBody2D>().DirectAtEntityIndex(id.index)
            : nullptr;
        if (rb && !HasFlag(rb->flags, BodyFlags2D::Enabled)) {
            continue;
        }

        const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        const u16 bIdx = m_boxCount++;

        f32 aabbMinX = 0.0f;
        f32 aabbMinY = 0.0f;
        f32 aabbMaxX = 0.0f;
        f32 aabbMaxY = 0.0f;

        if (col.shape == ShapeKind2D::Segment) {
            const Vec2 p0 = xf.TransformPoint(col.localBounds.SegmentP0());
            const Vec2 p1 = xf.TransformPoint(col.localBounds.SegmentP1());
            m_boxes[bIdx] = {p0.x, p0.y, p1.x, p1.y, id, col.filter.categoryBits, col.shape, col.flags};
            aabbMinX = std::min(p0.x, p1.x);
            aabbMinY = std::min(p0.y, p1.y);
            aabbMaxX = std::max(p0.x, p1.x);
            aabbMaxY = std::max(p0.y, p1.y);
        } else {
            const Vec2 center = xf.TransformPoint({col.localBounds.OffsetX(), col.localBounds.OffsetY()});
            const f32 hw = col.localBounds.HalfW() * std::abs(xf.scaleX);
            const f32 hh = col.localBounds.HalfH() * std::abs(xf.scaleY);
            const bool isRotated = (col.shape == ShapeKind2D::Obb || col.shape == ShapeKind2D::CapsuleY);
            const f32 extX = isRotated ? (hw * std::abs(xf.cosRot) + hh * std::abs(xf.sinRot)) : hw;
            const f32 extY = isRotated ? (hw * std::abs(xf.sinRot) + hh * std::abs(xf.cosRot)) : hh;
            aabbMinX = center.x - extX;
            aabbMinY = center.y - extY;
            aabbMaxX = center.x + extX;
            aabbMaxY = center.y + extY;
            m_boxes[bIdx] = {aabbMinX, aabbMinY, aabbMaxX, aabbMaxY, id, col.filter.categoryBits, col.shape, col.flags};

            if (dt > 0.0f && rb && rb->IsBullet()) {
                const f32 dx = rb->vx * dt;
                const f32 dy = rb->vy * dt;
                if (dx < 0.0f) aabbMinX += dx;
                else           aabbMaxX += dx;
                if (dy < 0.0f) aabbMinY += dy;
                else           aabbMaxY += dy;
            }
        }

        const i32 minCX = static_cast<i32>(std::floor(aabbMinX * m_invCellSize));
        const i32 minCY = static_cast<i32>(std::floor(aabbMinY * m_invCellSize));
        const i32 maxCX = static_cast<i32>(std::floor(aabbMaxX * m_invCellSize));
        const i32 maxCY = static_cast<i32>(std::floor(aabbMaxY * m_invCellSize));

        i16* meta = spanMeta + static_cast<usize>(bIdx) * 4U;
        if ((maxCX - minCX) >= static_cast<i32>(kGridDim) || (maxCY - minCY) >= static_cast<i32>(kGridDim)) {
            meta[0] = 1;
            meta[2] = 0;
            continue;
        }

        meta[0] = static_cast<i16>(minCX);
        meta[1] = static_cast<i16>(minCY);
        meta[2] = static_cast<i16>(maxCX);
        meta[3] = static_cast<i16>(maxCY);

        for (i32 y = minCY; y <= maxCY; ++y) {
            for (i32 x = minCX; x <= maxCX; ++x) {
                ++m_cellOffsets[HashCellCoords(x, y)];
            }
        }
    }

    u32 runningOffset = 0U;
    for (u32 c = 0U; c < kGridCells; ++c) {
        const u32 count = m_cellOffsets[c];
        m_cellOffsets[c] = runningOffset;
        runningOffset = std::min<u32>(runningOffset + count, static_cast<u32>(maxRefCap));
    }
    m_cellOffsets[kGridCells] = runningOffset;

    for (u16 bIdx = 0U; bIdx < m_boxCount; ++bIdx) {
        const i16* meta = spanMeta + static_cast<usize>(bIdx) * 4U;
        if (meta[0] > meta[2]) {
            continue;
        }
        for (i16 y = meta[1]; y <= meta[3]; ++y) {
            for (i16 x = meta[0]; x <= meta[2]; ++x) {
                const u32 cell = HashCellCoords(x, y);
                const u32 writePos = m_cellOffsets[cell];
                if (writePos < maxRefCap) {
                    m_cellBoxIndices[writePos] = bIdx;
                    m_cellOffsets[cell] = writePos + 1U;
                }
            }
        }
    }

    auto* oversizedOut = reinterpret_cast<u16*>(m_boxes + m_maxBoxes);
    u32 oversizedCount = 0U;
    for (u16 bIdx = 0U; bIdx < m_boxCount; ++bIdx) {
        const i16* meta = spanMeta + static_cast<usize>(bIdx) * 4U;
        if (meta[0] > meta[2]) {
            oversizedOut[oversizedCount++] = bIdx;
        }
    }

    for (u32 c = kGridCells; c > 0U; --c) {
        m_cellOffsets[c] = m_cellOffsets[c - 1U];
    }
    m_cellOffsets[0] = 0U;
    m_cellOffsets[kGridCells + 1U] = oversizedCount;
}

RayHit2D SpatialIndex2D::Raycast(
    f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist,
    u16 categoryMask, EntityId ignoreEntity, bool includeSensors) const noexcept {
    RayHit2D bestHit{};
    bestHit.distance = maxDist;

    const f32 endX = originX + dirX * maxDist;
    const f32 endY = originY + dirY * maxDist;
    const f32 rayMinX = std::min(originX, endX);
    const f32 rayMaxX = std::max(originX, endX);
    const f32 rayMinY = std::min(originY, endY);
    const f32 rayMaxY = std::max(originY, endY);

    if ((categoryMask & 0x0001U) != 0U) {
        VisitTileRuns(*this, rayMinX, rayMinY, rayMaxX, rayMaxY,
            [&](i32, i32, i32, f32 tileMinX, f32 tileMinY, f32 tileMaxX, f32 tileMaxY, TileMaterialFlags flags) noexcept {
                if (!HasFlag(flags, TileMaterialFlags::Solid)) {
                    return;
                }
                const RayHit2D h = Narrowphase2D::RaycastAabb(
                    originX, originY, dirX, dirY, bestHit.distance, tileMinX, tileMinY, tileMaxX, tileMaxY);
                if (h.Hit() && h.distance < bestHit.distance) {
                    bestHit = h;
                    bestHit.fraction = (maxDist > 1e-8f) ? (h.distance / maxDist) : 0.0f;
                    bestHit.tileFlags = flags;
                }
            });
    }

    VisitCandidateBoxes(*this, rayMinX, rayMinY, rayMaxX, rayMaxY, [&](const SpatialBox2D& b) noexcept {
        if (b.entity == ignoreEntity || (b.categoryBits & categoryMask) == 0U ||
            (!includeSensors && b.IsSensor())) {
            return;
        }
        if (b.BoundMaxX() < rayMinX || b.BoundMinX() > rayMaxX ||
            b.BoundMaxY() < rayMinY || b.BoundMinY() > rayMaxY) {
            return;
        }
        RayHit2D h{};
        if (b.shape == ShapeKind2D::Circle) {
            h = Narrowphase2D::RaycastCircle(
                originX, originY, dirX, dirY, bestHit.distance,
                0.5f * (b.minX + b.maxX), 0.5f * (b.minY + b.maxY), 0.5f * (b.maxX - b.minX));
        } else if (b.shape == ShapeKind2D::Segment) {
            h = Narrowphase2D::RaycastSegment(
                originX, originY, dirX, dirY, bestHit.distance, {b.minX, b.minY}, {b.maxX, b.maxY});
        } else {
            h = Narrowphase2D::RaycastAabb(
                originX, originY, dirX, dirY, bestHit.distance, b.minX, b.minY, b.maxX, b.maxY);
        }
        if (h.Hit() && h.distance < bestHit.distance) {
            bestHit = h;
            bestHit.fraction = (maxDist > 1e-8f) ? (h.distance / maxDist) : 0.0f;
            bestHit.hitEntity = b.entity;
        }
    });

    return bestHit;
}

SweepHit2D SpatialIndex2D::SweepAabb(
    EntityId ignoreEntity,
    f32 centerX, f32 centerY, f32 halfW, f32 halfH,
    f32 deltaX, f32 deltaY,
    u16 solidMask, bool ignoreOneWay) const noexcept {
    return SweepVolumeCommon(*this, ignoreEntity, centerX, centerY, halfW, halfH, deltaX, deltaY, solidMask, ignoreOneWay, false);
}

SweepHit2D SpatialIndex2D::CastCircle(
    EntityId ignoreEntity,
    f32 centerX, f32 centerY, f32 radius,
    f32 deltaX, f32 deltaY,
    u16 solidMask, bool ignoreOneWay) const noexcept {
    return SweepVolumeCommon(*this, ignoreEntity, centerX, centerY, radius, radius, deltaX, deltaY, solidMask, ignoreOneWay, true);
}

u32 SpatialIndex2D::QueryAabb(
    f32 minX, f32 minY, f32 maxX, f32 maxY,
    u16 categoryMask, std::span<EntityId> outEntities,
    EntityId ignoreEntity, bool includeSensors) const noexcept {
    return QueryBoxesCommon(*this, minX, minY, maxX, maxY, categoryMask, outEntities, ignoreEntity, includeSensors,
        [&](const SpatialBox2D& b) noexcept {
            return b.BoundMaxX() >= minX && b.BoundMinX() <= maxX &&
                   b.BoundMaxY() >= minY && b.BoundMinY() <= maxY;
        });
}

u32 SpatialIndex2D::QueryCircle(
    f32 centerX, f32 centerY, f32 radius,
    u16 categoryMask, std::span<EntityId> outEntities,
    EntityId ignoreEntity, bool includeSensors) const noexcept {
    const f32 rSq = radius * radius;
    return QueryBoxesCommon(*this, centerX - radius, centerY - radius, centerX + radius, centerY + radius,
        categoryMask, outEntities, ignoreEntity, includeSensors,
        [&](const SpatialBox2D& b) noexcept {
            const f32 dx = centerX - std::clamp(centerX, b.BoundMinX(), b.BoundMaxX());
            const f32 dy = centerY - std::clamp(centerY, b.BoundMinY(), b.BoundMaxY());
            return dx * dx + dy * dy <= rSq;
        });
}

} // namespace en
