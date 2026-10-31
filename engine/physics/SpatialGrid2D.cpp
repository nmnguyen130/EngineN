#include "physics/SpatialGrid2D.h"

namespace en {

bool SpatialGrid2D::Init(Arena& arena, u16 maxProxies, f32 cellSize) noexcept {
    if (maxProxies == 0U || cellSize <= 0.05f) return false;
    const u32 mark = arena.Mark();
    auto proxies   = arena.Alloc<SpatialProxy2D>(maxProxies);
    auto offsets   = arena.Alloc<u32>(kGridCells + 1U);
    auto refs      = arena.Alloc<u16>(static_cast<usize>(maxProxies) * 17U);
    if (proxies.empty() || offsets.empty() || refs.empty()) {
        arena.Rollback(mark);
        return false;
    }

    *this         = {};
    m_proxies     = proxies.data();
    m_cellOffsets = offsets.data();
    m_cellRefs    = refs.data();
    m_invCellSize = 1.0f / cellSize;
    m_maxProxies  = maxProxies;
    Clear();
    return true;
}

void SpatialGrid2D::Clear() noexcept {
    m_proxyCount     = 0U;
    m_oversizedCount = 0U;
    if (m_cellOffsets) {
        std::fill_n(m_cellOffsets, kGridCells + 1U, 0U);
    }
}

Result<void, PhysicsErrc> SpatialGrid2D::BuildTileMap(
    Arena& arena,
    u16 width,
    u16 height,
    f32 tileSize,
    Vec2 origin,
    std::span<const TileFlag> tiles) noexcept {
    if (width == 0U || height == 0U || tileSize <= 0.01f) {
        return Err(PhysicsErrc::InvalidConfig);
    }
    const usize count = static_cast<usize>(width) * height;
    if (tiles.size() < count || count > 0xFFFFU) {
        return Err(PhysicsErrc::InvalidConfig);
    }

    if (!m_tiles || m_tileCap < count) {
        auto alloc = arena.Alloc<TileFlag>(count);
        if (alloc.empty()) return Err(PhysicsErrc::OutOfMemory);
        m_tiles   = alloc.data();
        m_tileCap = static_cast<u16>(count);
    }

    std::memcpy(m_tiles, tiles.data(), count * sizeof(TileFlag));
    m_widthTiles  = width;
    m_heightTiles = height;
    m_tileSize    = tileSize;
    m_originX     = origin.x;
    m_originY     = origin.y;
    return {};
}

Result<void, PhysicsErrc> SpatialGrid2D::LoadTileMapBlob(
    Arena& arena,
    std::span<const u8> blobBytes) noexcept {
    if (blobBytes.size() < sizeof(TileMapHeader)) {
        return Err(PhysicsErrc::CorruptedBlob);
    }
    TileMapHeader hdr{};
    std::memcpy(&hdr, blobBytes.data(), sizeof(TileMapHeader));
    if (hdr.magic != kTileMapMagic || hdr.version != 1U ||
        hdr.width == 0U || hdr.height == 0U || hdr.tileSize <= 0.01f) {
        return Err(PhysicsErrc::CorruptedBlob);
    }
    const usize count = static_cast<usize>(hdr.width) * hdr.height;
    if (blobBytes.size() < sizeof(TileMapHeader) + count) {
        return Err(PhysicsErrc::CorruptedBlob);
    }
    const std::span<const TileFlag> tiles{
        reinterpret_cast<const TileFlag*>(blobBytes.data() + sizeof(TileMapHeader)),
        count,
    };
    return BuildTileMap(arena, hdr.width, hdr.height, hdr.tileSize, hdr.origin, tiles);
}

void SpatialGrid2D::SetTile(i32 tx, i32 ty, TileFlag flag) noexcept {
    if (!m_tiles || tx < 0 || ty < 0 ||
        tx >= static_cast<i32>(m_widthTiles) || ty >= static_cast<i32>(m_heightTiles)) {
        return;
    }
    m_tiles[static_cast<usize>(ty) * m_widthTiles + static_cast<usize>(tx)] = flag;
}

TileFlag SpatialGrid2D::SampleTile(i32 tx, i32 ty) const noexcept {
    if (!m_tiles || tx < 0 || ty < 0 ||
        tx >= static_cast<i32>(m_widthTiles) || ty >= static_cast<i32>(m_heightTiles)) {
        return TileFlag::Empty;
    }
    return m_tiles[static_cast<usize>(ty) * m_widthTiles + static_cast<usize>(tx)];
}

TileFlag SpatialGrid2D::SampleTileWorld(Vec2 worldPos) const noexcept {
    if (!m_tiles || m_tileSize <= 0.0f) return TileFlag::Empty;
    const f32 inv = 1.0f / m_tileSize;
    const i32 tx  = static_cast<i32>(std::floor((worldPos.x - m_originX) * inv));
    const i32 ty  = static_cast<i32>(std::floor((worldPos.y - m_originY) * inv));
    return SampleTile(tx, ty);
}

Rect SpatialGrid2D::TileWorldBounds(i32 tx, i32 ty) const noexcept {
    return {
        m_originX + static_cast<f32>(tx) * m_tileSize,
        m_originY + static_cast<f32>(ty) * m_tileSize,
        m_tileSize,
        m_tileSize,
    };
}

void SpatialGrid2D::Rebuild(const World& world) noexcept {
    if (!m_proxies || !m_cellOffsets || !m_cellRefs) return;
    Clear();
    if ((world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) == 0ULL) return;

    const auto& colPool   = world.GetPool<Collider2D>();
    const auto  entities  = colPool.Entities();
    const auto  cols      = colPool.Components();
    const bool  hasBody   = (world.RegisteredPoolsMask() & (1ULL << PhysicsBody2D::kTypeId)) != 0ULL;
    const u32   maxRefs   = static_cast<u32>(m_maxProxies) * 16U;
    u16*        oversized = m_cellRefs + maxRefs;

    for (usize i = 0U; i < entities.size() && m_proxyCount < m_maxProxies; ++i) {
        const EntityId id = entities[i];
        if (hasBody && (world.GetComponentMaskByIndex(id.index) & (1ULL << PhysicsBody2D::kTypeId)) != 0ULL) {
            if (world.GetPool<PhysicsBody2D>().DirectAtEntityIndex(id.index).enabled == 0U) {
                continue;
            }
        }

        const Collider2D&       col = cols[i];
        const WorldTransform2D& wt  = world.GetWorldByIndex(id.index);
        const Rect              box = col.WorldBounds({wt.x, wt.y});
        if (box.IsEmpty()) continue;

        const u16 pIdx   = m_proxyCount++;
        const u8  oneWay = (col.userTag == 1U) ? 1U : 0U;
        m_proxies[pIdx]  = SpatialProxy2D{
            .bounds    = box,
            .entity    = id,
            .layerMask = col.layerMask,
            .isTrigger = col.isTrigger,
            .oneWay    = oneWay,
        };

        const i32 minCX = static_cast<i32>(std::floor(box.x * m_invCellSize));
        const i32 minCY = static_cast<i32>(std::floor(box.y * m_invCellSize));
        const i32 maxCX = static_cast<i32>(std::floor(box.Right() * m_invCellSize));
        const i32 maxCY = static_cast<i32>(std::floor(box.Bottom() * m_invCellSize));

        if ((maxCX - minCX) >= 4 || (maxCY - minCY) >= 4) {
            oversized[m_oversizedCount++] = pIdx;
            continue;
        }
        for (i32 cy = minCY; cy <= maxCY; ++cy) {
            for (i32 cx = minCX; cx <= maxCX; ++cx) {
                ++m_cellOffsets[HashCell(cx, cy)];
            }
        }
    }

    u32 accum = 0U;
    for (u32 c = 0U; c < kGridCells; ++c) {
        const u32 count = m_cellOffsets[c];
        m_cellOffsets[c] = accum;
        accum = std::min(accum + count, maxRefs);
    }
    m_cellOffsets[kGridCells] = accum;

    for (u16 pIdx = 0U; pIdx < m_proxyCount; ++pIdx) {
        const Rect& box   = m_proxies[pIdx].bounds;
        const i32   minCX = static_cast<i32>(std::floor(box.x * m_invCellSize));
        const i32   minCY = static_cast<i32>(std::floor(box.y * m_invCellSize));
        const i32   maxCX = static_cast<i32>(std::floor(box.Right() * m_invCellSize));
        const i32   maxCY = static_cast<i32>(std::floor(box.Bottom() * m_invCellSize));
        if ((maxCX - minCX) >= 4 || (maxCY - minCY) >= 4) continue;

        for (i32 cy = minCY; cy <= maxCY; ++cy) {
            for (i32 cx = minCX; cx <= maxCX; ++cx) {
                const u32 cell = HashCell(cx, cy);
                const u32 pos  = m_cellOffsets[cell];
                if (pos < maxRefs) {
                    m_cellRefs[pos]     = pIdx;
                    m_cellOffsets[cell] = pos + 1U;
                }
            }
        }
    }

    for (u32 c = kGridCells; c > 0U; --c) {
        m_cellOffsets[c] = m_cellOffsets[c - 1U];
    }
    m_cellOffsets[0] = 0U;
}

template <typename Fn>
void SpatialGrid2D::VisitCandidates(const Rect& area, Fn&& fn) const noexcept {
    if (m_proxyCount == 0U || area.IsEmpty()) return;

    const i32 minCX = static_cast<i32>(std::floor(area.x * m_invCellSize));
    const i32 minCY = static_cast<i32>(std::floor(area.y * m_invCellSize));
    const i32 maxCX = static_cast<i32>(std::floor(area.Right() * m_invCellSize));
    const i32 maxCY = static_cast<i32>(std::floor(area.Bottom() * m_invCellSize));

    if ((maxCX - minCX) <= 16 && (maxCY - minCY) <= 16) {
        for (i32 cy = minCY; cy <= maxCY; ++cy) {
            for (i32 cx = minCX; cx <= maxCX; ++cx) {
                const u32 cell = HashCell(cx, cy);
                for (u32 r = m_cellOffsets[cell], end = m_cellOffsets[cell + 1U]; r < end; ++r) {
                    fn(m_proxies[m_cellRefs[r]]);
                }
            }
        }
        for (const u16 idx : OversizedRefs()) {
            fn(m_proxies[idx]);
        }
    } else {
        for (u16 i = 0U; i < m_proxyCount; ++i) {
            fn(m_proxies[i]);
        }
    }
}

template <typename Fn>
void SpatialGrid2D::VisitTileRuns(const Rect& area, Fn&& fn) const noexcept {
    if (!m_tiles || m_widthTiles == 0U || m_heightTiles == 0U || m_tileSize <= 0.0f) return;

    const f32 inv   = 1.0f / m_tileSize;
    const i32 minTX = std::max<i32>(0, static_cast<i32>(std::floor((area.x - m_originX) * inv)));
    const i32 maxTX = std::min<i32>(static_cast<i32>(m_widthTiles) - 1,
                                    static_cast<i32>(std::floor((area.Right() - m_originX) * inv)));
    const i32 minTY = std::max<i32>(0, static_cast<i32>(std::floor((area.y - m_originY) * inv)));
    const i32 maxTY = std::min<i32>(static_cast<i32>(m_heightTiles) - 1,
                                    static_cast<i32>(std::floor((area.Bottom() - m_originY) * inv)));

    for (i32 ty = minTY; ty <= maxTY; ++ty) {
        const f32 rowY = m_originY + static_cast<f32>(ty) * m_tileSize;
        for (i32 tx = minTX; tx <= maxTX; ++tx) {
            const TileFlag flags = SampleTile(tx, ty);
            if (flags == TileFlag::Empty) continue;
            i32 runEnd = tx;
            if (!IsSlopeTile(flags)) {
                while (runEnd + 1 <= maxTX && SampleTile(runEnd + 1, ty) == flags) {
                    ++runEnd;
                }
            }
            const f32 runX = m_originX + static_cast<f32>(tx) * m_tileSize;
            const f32 runW = static_cast<f32>(runEnd - tx + 1) * m_tileSize;
            fn(tx, runEnd, ty, Rect{runX, rowY, runW, m_tileSize}, flags);
            tx = runEnd;
        }
    }
}

RayHit2D SpatialGrid2D::Raycast(
    Vec2 origin,
    Vec2 dir,
    f32 maxDist,
    u16 queryMask,
    EntityId ignoreEntity,
    bool includeTriggers) const noexcept {
    const f32 dirLen = dir.Length();
    if (dirLen <= 1e-8f || maxDist < 0.0f) return {};
    const Vec2 unitDir = dir * (1.0f / dirLen);

    RayHit2D best{};
    best.distance = maxDist;

    const Vec2 endPt = origin + unitDir * maxDist;
    const Rect searchArea{
        std::min(origin.x, endPt.x) - 0.05f,
        std::min(origin.y, endPt.y) - 0.05f,
        std::abs(endPt.x - origin.x) + 0.10f,
        std::abs(endPt.y - origin.y) + 0.10f,
    };

    if ((queryMask & 0x0001U) != 0U) {
        VisitTileRuns(searchArea, [&](i32, i32, i32, const Rect& runBox, TileFlag flags) noexcept {
            if (IsSlopeTile(flags)) {
                const RayHit2D boxHit = RaycastBox(origin, unitDir, best.distance, runBox);
                if (!boxHit) return;
                const Vec2 pt = boxHit.point;
                const f32  u  = (pt.x - runBox.x) / runBox.w;
                const f32  sy = runBox.y + SampleSlopeHeight(flags, u, runBox.h);
                if (unitDir.y < -1e-5f && origin.y >= sy - 1e-3f) {
                    const f32 dist = (origin.y - sy) / (-unitDir.y);
                    if (dist >= 0.0f && dist <= best.distance) {
                        const Vec2 hitPt = origin + unitDir * dist;
                        if (hitPt.x >= runBox.x - 1e-3f && hitPt.x <= runBox.Right() + 1e-3f) {
                            best.point    = {hitPt.x, runBox.y + SampleSlopeHeight(flags, (hitPt.x - runBox.x) / runBox.w, runBox.h)};
                            best.normal   = SlopeNormal(flags);
                            best.distance = dist;
                            best.fraction = (maxDist > 1e-8f) ? (dist / maxDist) : 0.0f;
                            best.tile     = flags;
                            best.hit      = 1U;
                        }
                    }
                }
                return;
            }
            if (!HasFlag(flags, TileFlag::Solid)) return;
            const RayHit2D h = RaycastBox(origin, unitDir, best.distance, runBox);
            if (h && h.distance <= best.distance) {
                best          = h;
                best.fraction = (maxDist > 1e-8f) ? (h.distance / maxDist) : 0.0f;
                best.tile     = flags;
            }
        });
    }

    VisitCandidates(searchArea, [&](const SpatialProxy2D& p) noexcept {
        if (p.entity == ignoreEntity || (p.layerMask & queryMask) == 0U) return;
        if (p.isTrigger != 0U && !includeTriggers) return;
        if (!p.bounds.Overlaps(searchArea)) return;

        const RayHit2D h = RaycastBox(origin, unitDir, best.distance, p.bounds);
        if (h && h.distance <= best.distance) {
            best          = h;
            best.fraction = (maxDist > 1e-8f) ? (h.distance / maxDist) : 0.0f;
            best.entity   = p.entity;
        }
    });

    return best;
}

u32 SpatialGrid2D::RaycastAll(
    Vec2 origin,
    Vec2 dir,
    f32 maxDist,
    u16 queryMask,
    std::span<RayHit2D> outHits,
    EntityId ignoreEntity,
    bool includeTriggers) const noexcept {
    const f32 dirLen = dir.Length();
    if (dirLen <= 1e-8f || maxDist < 0.0f || outHits.empty()) return 0U;
    const Vec2 unitDir = dir * (1.0f / dirLen);

    const Vec2 endPt = origin + unitDir * maxDist;
    const Rect searchArea{
        std::min(origin.x, endPt.x) - 0.05f,
        std::min(origin.y, endPt.y) - 0.05f,
        std::abs(endPt.x - origin.x) + 0.10f,
        std::abs(endPt.y - origin.y) + 0.10f,
    };

    u32 count = 0U;
    VisitCandidates(searchArea, [&](const SpatialProxy2D& p) noexcept {
        if (p.entity == ignoreEntity || (p.layerMask & queryMask) == 0U) return;
        if (p.isTrigger != 0U && !includeTriggers) return;
        if (!p.bounds.Overlaps(searchArea)) return;

        const u32 n = std::min<u32>(count, static_cast<u32>(outHits.size()));
        for (u32 i = 0U; i < n; ++i) {
            if (outHits[i].entity == p.entity) return;
        }

        RayHit2D h = RaycastBox(origin, unitDir, maxDist, p.bounds);
        if (!h) return;
        h.entity = p.entity;

        if (count < outHits.size()) outHits[count] = h;
        ++count;
    });

    const u32 stored = std::min<u32>(count, static_cast<u32>(outHits.size()));
    std::sort(outHits.begin(), outHits.begin() + stored,
              [](const RayHit2D& a, const RayHit2D& b) noexcept {
                  if (a.distance != b.distance) return a.distance < b.distance;
                  return a.entity < b.entity;
              });
    return count;
}

SweepResult2D SpatialGrid2D::SweepBox(
    const Rect& movingBox,
    Vec2 delta,
    u16 queryMask,
    EntityId ignoreEntity,
    bool ignoreOneWay) const noexcept {
    if (movingBox.IsEmpty()) return {};

    SweepResult2D best{};
    const Rect searchArea{
        std::min(movingBox.x, movingBox.x + delta.x) - 0.05f,
        std::min(movingBox.y, movingBox.y + delta.y) - 0.05f,
        movingBox.w + std::abs(delta.x) + 0.10f,
        movingBox.h + std::abs(delta.y) + 0.10f,
    };

    if ((queryMask & 0x0001U) != 0U) {
        VisitTileRuns(searchArea, [&](i32 tx0, i32 tx1, i32 ty, const Rect& runBox, TileFlag flags) noexcept {
            // Slopes are resolved analytically via heightmap evaluation in PhysicsSolver::StepDynamicBodies.
            if (IsSlopeTile(flags)) return;

            const bool isOneWay = HasFlag(flags, TileFlag::OneWay) && !HasFlag(flags, TileFlag::Solid);
            if (!HasFlag(flags, TileFlag::Solid | TileFlag::OneWay)) return;
            if (isOneWay) {
                if (ignoreOneWay || delta.y > 0.0f || movingBox.y < runBox.Bottom() - 2e-3f) return;
            }

            // When the actor's foot center X is inside an adjacent slope tile in the same row,
            // that slope is the authoritative ground surface and suppresses adjacent flat overhang collisions.
            if (!isOneWay && m_tileSize > 0.0f) {
                const f32 footX  = movingBox.Center().x + delta.x;
                const i32 footTX = static_cast<i32>(std::floor((footX - m_originX) / m_tileSize));
                if ((footTX == tx0 - 1 || footTX == tx1 + 1) &&
                    (IsSlopeTile(SampleTile(footTX, ty)) || IsSlopeTile(SampleTile(footTX, ty - 1)))) {
                    return;
                }
            }

            SweepResult2D h = SweepBoxVsBox(movingBox, delta, runBox);
            if (!h) return;

            if (!isOneWay && h.startSolid == 0U) {
                const bool topCovered = HasFlag(SampleTile(tx0, ty + 1), TileFlag::Solid) &&
                                        HasFlag(SampleTile(tx1, ty + 1), TileFlag::Solid);
                const bool botCovered = HasFlag(SampleTile(tx0, ty - 1), TileFlag::Solid) &&
                                        HasFlag(SampleTile(tx1, ty - 1), TileFlag::Solid);
                if ((h.normal.y > 0.5f && topCovered) || (h.normal.y < -0.5f && botCovered)) {
                    return;
                }
            }
            if (isOneWay && h.normal.y < 0.5f) return;

            if (h.time < best.time || (h.startSolid != 0U && best.startSolid == 0U)) {
                best        = h;
                best.tile   = flags;
                best.oneWay = isOneWay ? 1U : 0U;
            }
        });
    }

    VisitCandidates(searchArea, [&](const SpatialProxy2D& p) noexcept {
        if (p.entity == ignoreEntity || p.isTrigger != 0U || (p.layerMask & queryMask) == 0U) return;
        if (!p.bounds.Overlaps(searchArea)) return;

        const bool isOneWay = p.oneWay != 0U;
        if (isOneWay) {
            if (ignoreOneWay || delta.y > 0.0f || movingBox.y < p.bounds.Bottom() - 2e-3f) return;
        }

        SweepResult2D h = SweepBoxVsBox(movingBox, delta, p.bounds);
        if (!h || (isOneWay && h.normal.y < 0.5f)) return;

        if (h.time < best.time || (h.startSolid != 0U && best.startSolid == 0U)) {
            best        = h;
            best.entity = p.entity;
            best.oneWay = isOneWay ? 1U : 0U;
        }
    });

    return best;
}

u32 SpatialGrid2D::QueryBox(
    const Rect& queryBox,
    u16 queryMask,
    std::span<EntityId> outEntities,
    EntityId ignoreEntity,
    bool includeTriggers) const noexcept {
    if (queryBox.IsEmpty()) return 0U;
    u32 count = 0U;
    VisitCandidates(queryBox, [&](const SpatialProxy2D& p) noexcept {
        if (p.entity == ignoreEntity || (p.layerMask & queryMask) == 0U) return;
        if (p.isTrigger != 0U && !includeTriggers) return;
        if (!queryBox.Overlaps(p.bounds)) return;

        const u32 n = std::min<u32>(count, static_cast<u32>(outEntities.size()));
        for (u32 i = 0U; i < n; ++i) {
            if (outEntities[i] == p.entity) return;
        }
        if (count < outEntities.size()) outEntities[count] = p.entity;
        ++count;
    });
    return count;
}

u32 SpatialGrid2D::QueryCircle(
    Vec2 center,
    f32 radius,
    u16 queryMask,
    std::span<EntityId> outEntities,
    EntityId ignoreEntity,
    bool includeTriggers) const noexcept {
    if (radius <= 0.0f) return 0U;
    const Rect searchBox = Rect::FromCenterHalf(center, {radius, radius});
    const f32  rSq       = radius * radius;
    u32        count     = 0U;

    VisitCandidates(searchBox, [&](const SpatialProxy2D& p) noexcept {
        if (p.entity == ignoreEntity || (p.layerMask & queryMask) == 0U) return;
        if (p.isTrigger != 0U && !includeTriggers) return;

        const f32 cx = std::clamp(center.x, p.bounds.x, p.bounds.Right());
        const f32 cy = std::clamp(center.y, p.bounds.y, p.bounds.Bottom());
        const f32 dx = center.x - cx;
        const f32 dy = center.y - cy;
        if (dx * dx + dy * dy > rSq) return;

        const u32 n = std::min<u32>(count, static_cast<u32>(outEntities.size()));
        for (u32 i = 0U; i < n; ++i) {
            if (outEntities[i] == p.entity) return;
        }
        if (count < outEntities.size()) outEntities[count] = p.entity;
        ++count;
    });
    return count;
}

} // namespace en
