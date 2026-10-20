#include "physics/SpatialIndex2D.h"
#include "core/Serialization.h"
#include <algorithm>
#include <cmath>

namespace en {

bool SpatialIndex2D::Init(AlignedArena& arena, u16 maxMapWidth, u16 maxMapHeight) noexcept {
    m_maxWidth = static_cast<u16>(AlignUp(maxMapWidth, 8));
    m_maxHeight = static_cast<u16>(AlignUp(maxMapHeight, 8));
    const usize totalTiles = static_cast<usize>(m_maxWidth) * m_maxHeight;
    const usize totalChunks = (m_maxWidth / 8U) * (m_maxHeight / 8U);

    m_tiles = arena.Carve<TileType>(totalTiles);
    m_solidChunks = arena.Carve<u64>(totalChunks);
    m_boxes = arena.Carve<GridBox>(kMaxBoxes);
    if (m_tiles.empty() || m_solidChunks.empty() || m_boxes.empty()) return false;

    SetupProceduralGrid(64, 32, 1.0f, {0.0f, 0.0f});
    return true;
}

void SpatialIndex2D::ClearTilemap() noexcept {
    for (TileType& t : m_tiles) t = TileType::Empty;
    for (u64& c : m_solidChunks) c = 0;
    m_boxCount = 0;
}

void SpatialIndex2D::SetupProceduralGrid(u16 width, u16 height, f32 tileSize, Vec2 origin) noexcept {
    m_width = std::min(width, m_maxWidth);
    m_height = std::min(height, m_maxHeight);
    m_chunksX = static_cast<u16>((m_width + 7U) / 8U);
    m_chunksY = static_cast<u16>((m_height + 7U) / 8U);
    m_tileSize = tileSize > 1e-4f ? tileSize : 1.0f;
    m_invTileSize = 1.0f / m_tileSize;
    m_origin = origin;
    ClearTilemap();
}

Result<void, WorldErrc> SpatialIndex2D::BindCollisionMapBlob(const BlobRecord& blob) noexcept {
    if (!blob.IsValid() || blob.size < sizeof(CookedCollisionHeader)) {
        return Err(WorldErrc::CorruptedPrefab);
    }
    BinaryReader r(blob.Bytes());
    CookedCollisionHeader hdr{};
    if (!r.Read(hdr) || hdr.magic != kCookedCollisionMagic) {
        return Err(WorldErrc::CorruptedPrefab);
    }
    SetupProceduralGrid(hdr.width, hdr.height, hdr.tileSize, {hdr.originX, hdr.originY});
    const usize count = static_cast<usize>(m_width) * m_height;
    if (r.Remaining() < count) return Err(WorldErrc::CorruptedPrefab);
    for (u16 y = 0; y < m_height; ++y) {
        for (u16 x = 0; x < m_width; ++x) {
            u8 raw = 0;
            (void)r.Read(raw);
            SetTile(x, y, static_cast<TileType>(raw));
        }
    }
    return {};
}

void SpatialIndex2D::SetTile(i32 tx, i32 ty, TileType type) noexcept {
    if (tx < 0 || ty < 0 || tx >= m_width || ty >= m_height) return;
    m_tiles[static_cast<usize>(ty) * m_width + static_cast<usize>(tx)] = type;
    const usize cx = static_cast<usize>(tx) >> 3U;
    const usize cy = static_cast<usize>(ty) >> 3U;
    const u32 bitIdx = static_cast<u32>(((ty & 7) << 3) | (tx & 7));
    const u64 bit = (1ULL << bitIdx);
    const bool isSolidLike = (type == TileType::Solid || type == TileType::OneWay ||
                              type == TileType::SlopeUpRight || type == TileType::SlopeUpLeft ||
                              type == TileType::ConveyorRight || type == TileType::ConveyorLeft);
    if (isSolidLike) {
        m_solidChunks[cy * m_chunksX + cx] |= bit;
    } else {
        m_solidChunks[cy * m_chunksX + cx] &= ~bit;
    }
}

TileType SpatialIndex2D::GetTile(i32 tx, i32 ty) const noexcept {
    if (tx < 0 || ty < 0 || tx >= m_width || ty >= m_height) return TileType::Empty;
    return m_tiles[static_cast<usize>(ty) * m_width + static_cast<usize>(tx)];
}

u8 SpatialIndex2D::ComputeAutoTileMask4(i32 tx, i32 ty, TileType matchType) const noexcept {
    u8 mask = 0;
    if (GetTile(tx, ty + 1) == matchType) mask |= 1U;
    if (GetTile(tx + 1, ty) == matchType) mask |= 2U;
    if (GetTile(tx, ty - 1) == matchType) mask |= 4U;
    if (GetTile(tx - 1, ty) == matchType) mask |= 8U;
    return mask;
}

bool SpatialIndex2D::IsChunkSolidEmpty(i32 cx, i32 cy) const noexcept {
    if (cx < 0 || cy < 0 || cx >= m_chunksX || cy >= m_chunksY) return true;
    return m_solidChunks[static_cast<usize>(cy) * m_chunksX + static_cast<usize>(cx)] == 0ULL;
}

void SpatialIndex2D::RebuildDynamicGrid(const World& world, SpatialRebuildMode /*mode*/) noexcept {
    m_boxCount = 0;
    world.Each<KinematicCarrier2D>([&](EntityId id, const KinematicCarrier2D& carrier) noexcept {
        if (m_boxCount >= kMaxBoxes) return;
        const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
        const Rect r = carrier.bounds.WorldRect({wt.x, wt.y});
        m_boxes[m_boxCount++] = GridBox{r.x, r.y, r.Right(), r.Bottom(), id, 1U, 1U};
    });

    world.Each<KinematicMotor2D>([&](EntityId id, const KinematicMotor2D& motor) noexcept {
        if (m_boxCount >= kMaxBoxes) return;
        const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
        const Rect r = motor.bounds.WorldRect({wt.x, wt.y});
        m_boxes[m_boxCount++] = GridBox{r.x, r.y, r.Right(), r.Bottom(), id, motor.bodyLayer, 0U};
    });
}

namespace {
[[nodiscard]] bool SweepMovingAabbVsStaticAabb(
    Vec2 center, Vec2 half, Vec2 delta, const Rect& target, bool oneWayTopOnly,
    f32& ioBestToi, Vec2& outNormal) noexcept {
    const f32 expMinX = target.x - half.x;
    const f32 expMaxX = target.Right() + half.x;
    const f32 expMinY = target.y - half.y;
    const f32 expMaxY = target.Bottom() + half.y;

    if (oneWayTopOnly) {
        if (delta.y >= 0.0f) return false;
        const f32 bottomStart = center.y - half.y;
        if (bottomStart < target.Bottom() - 0.04f) return false;
    }

    f32 tEnter = 0.0f;
    f32 tExit = 1.0f;
    Vec2 enterNorm{0.0f, 0.0f};

    if (std::abs(delta.x) < 1e-6f) {
        if (center.x <= expMinX || center.x >= expMaxX) return false;
    } else {
        const f32 invDx = 1.0f / delta.x;
        f32 t1 = (expMinX - center.x) * invDx;
        f32 t2 = (expMaxX - center.x) * invDx;
        f32 nx = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            nx = 1.0f;
        }
        if (t1 > tEnter) {
            tEnter = t1;
            enterNorm = {nx, 0.0f};
        }
        tExit = std::min(tExit, t2);
        if (tEnter > tExit) return false;
    }

    if (std::abs(delta.y) < 1e-6f) {
        if (center.y <= expMinY || center.y >= expMaxY) return false;
    } else {
        const f32 invDy = 1.0f / delta.y;
        f32 t1 = (expMinY - center.y) * invDy;
        f32 t2 = (expMaxY - center.y) * invDy;
        f32 ny = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            ny = 1.0f;
        }
        if (t1 > tEnter) {
            tEnter = t1;
            enterNorm = {0.0f, ny};
        }
        tExit = std::min(tExit, t2);
        if (tEnter > tExit) return false;
    }

    if (oneWayTopOnly && enterNorm.y <= 0.5f) return false;
    if (tEnter >= 0.0f && tEnter < ioBestToi && enterNorm.LengthSq() > 0.1f) {
        ioBestToi = tEnter;
        outNormal = enterNorm;
        return true;
    }
    return false;
}
} // namespace

SlabImpact2D SpatialIndex2D::SweepAabb(
    EntityId ignoreEntity, Vec2 center, Vec2 halfExtents, Vec2 delta,
    u16 /*solidMask*/, bool ignoreOneWay) const noexcept {
    SlabImpact2D result{};
    const Rect startRect = Rect::FromCenterHalf(center, halfExtents);
    const Rect endRect = Rect::FromCenterHalf(center + delta, halfExtents);
    const f32 minX = std::min(startRect.x, endRect.x) - m_origin.x;
    const f32 maxX = std::max(startRect.Right(), endRect.Right()) - m_origin.x;
    const f32 minY = std::min(startRect.y, endRect.y) - m_origin.y;
    const f32 maxY = std::max(startRect.Bottom(), endRect.Bottom()) - m_origin.y;

    const i32 tx0 = std::max(0, static_cast<i32>(std::floor(minX * m_invTileSize)) - 1);
    const i32 tx1 = std::min(static_cast<i32>(m_width) - 1, static_cast<i32>(std::floor(maxX * m_invTileSize)) + 1);
    const i32 ty0 = std::max(0, static_cast<i32>(std::floor(minY * m_invTileSize)) - 1);
    const i32 ty1 = std::min(static_cast<i32>(m_height) - 1, static_cast<i32>(std::floor(maxY * m_invTileSize)) + 1);

    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const TileType tt = GetTile(tx, ty);
            if (tt != TileType::Solid && tt != TileType::OneWay &&
                tt != TileType::ConveyorRight && tt != TileType::ConveyorLeft) {
                continue;
            }
            if (tt == TileType::OneWay && ignoreOneWay) continue;
            const Rect tileRect{
                m_origin.x + static_cast<f32>(tx) * m_tileSize,
                m_origin.y + static_cast<f32>(ty) * m_tileSize,
                m_tileSize,
                m_tileSize,
            };
            Vec2 n{};
            if (SweepMovingAabbVsStaticAabb(
                    center, halfExtents, delta, tileRect, tt == TileType::OneWay, result.toi, n)) {
                result.hit = true;
                result.normal = n;
                result.hitEntity = kNullEntity;
            }
        }
    }

    for (u32 i = 0; i < m_boxCount; ++i) {
        const GridBox& b = m_boxes[i];
        if ((b.flags & 1U) == 0 || b.entity == ignoreEntity) continue;
        Vec2 n{};
        if (SweepMovingAabbVsStaticAabb(center, halfExtents, delta, b.ToRect(), false, result.toi, n)) {
            result.hit = true;
            result.normal = n;
            result.hitEntity = b.entity;
        }
    }
    return result;
}

RayHit2D SpatialIndex2D::Raycast(
    Vec2 origin, Vec2 direction, f32 maxDist, u16 layerMask, EntityId ignoreEntity) const noexcept {
    RayHit2D best{};
    best.distance = maxDist;
    const Vec2 dir = direction.Normalized();
    if (dir.LengthSq() < 0.5f || maxDist <= 0.0f) return best;

    const u32 steps = std::max(8U, static_cast<u32>(std::ceil(maxDist / (m_tileSize * 0.25f))));
    const f32 stepLen = maxDist / static_cast<f32>(steps);
    Vec2 prevPt = origin;

    for (u32 s = 1; s <= steps; ++s) {
        const f32 d = static_cast<f32>(s) * stepLen;
        const Vec2 pt = origin + dir * d;
        const i32 tx = static_cast<i32>(std::floor((pt.x - m_origin.x) * m_invTileSize));
        const i32 ty = static_cast<i32>(std::floor((pt.y - m_origin.y) * m_invTileSize));
        const TileType tt = GetTile(tx, ty);
        if (tt == TileType::Solid || tt == TileType::SlopeUpRight || tt == TileType::SlopeUpLeft ||
            tt == TileType::ConveyorRight || tt == TileType::ConveyorLeft) {
            best.hit = true;
            best.distance = d - stepLen * 0.5f;
            best.point = (prevPt + pt) * 0.5f;
            best.normal = -dir;
            best.tile = tt;
            best.entity = kNullEntity;
            break;
        }
        prevPt = pt;
    }

    for (u32 i = 0; i < m_boxCount; ++i) {
        const GridBox& b = m_boxes[i];
        if (b.entity == ignoreEntity || (b.layerMask & layerMask) == 0) continue;
        const Rect r = b.ToRect();
        f32 t = best.distance / maxDist;
        Vec2 n{};
        if (SweepMovingAabbVsStaticAabb(origin, {0.01f, 0.01f}, dir * maxDist, r, false, t, n)) {
            const f32 hitDist = t * maxDist;
            if (hitDist < best.distance) {
                best.hit = true;
                best.distance = hitDist;
                best.point = origin + dir * hitDist;
                best.normal = n;
                best.entity = b.entity;
                best.tile = TileType::Empty;
            }
        }
    }
    return best;
}

RayHit2D SpatialIndex2D::CircleCast(
    Vec2 origin, f32 radius, Vec2 direction, f32 maxDist, u16 /*layerMask*/, EntityId ignoreEntity) const noexcept {
    RayHit2D out{};
    const Vec2 dir = direction.Normalized();
    const SlabImpact2D imp = SweepAabb(ignoreEntity, origin, {radius, radius}, dir * maxDist, 1U, false);
    if (imp.hit) {
        out.hit = true;
        out.distance = imp.toi * maxDist;
        out.point = origin + dir * out.distance;
        out.normal = imp.normal;
        out.entity = imp.hitEntity;
    }
    return out;
}

u32 SpatialIndex2D::OverlapAabb(
    const Rect& area, u16 layerMask, std::span<EntityId> outEntities, EntityId ignoreEntity) const noexcept {
    u32 count = 0;
    for (u32 i = 0; i < m_boxCount; ++i) {
        const GridBox& b = m_boxes[i];
        if (b.entity == ignoreEntity || (b.layerMask & layerMask) == 0) continue;
        if (area.Overlaps(b.ToRect())) {
            if (count < outEntities.size()) outEntities[count] = b.entity;
            ++count;
        }
    }
    return count;
}

u32 SpatialIndex2D::OverlapCircle(
    Circle circle, u16 layerMask, std::span<EntityId> outEntities, EntityId ignoreEntity) const noexcept {
    const Rect bound = Rect::FromCenterHalf(circle.center, {circle.radius, circle.radius});
    return OverlapAabb(bound, layerMask, outEntities, ignoreEntity);
}

ContactFlags SpatialIndex2D::SampleEnvironmentFlags(const Rect& worldBox) const noexcept {
    ContactFlags flags = ContactFlags::None;
    const i32 tx0 = static_cast<i32>(std::floor((worldBox.x - m_origin.x) * m_invTileSize));
    const i32 tx1 = static_cast<i32>(std::floor((worldBox.Right() - m_origin.x) * m_invTileSize));
    const i32 ty0 = static_cast<i32>(std::floor((worldBox.y - m_origin.y) * m_invTileSize));
    const i32 ty1 = static_cast<i32>(std::floor((worldBox.Bottom() - m_origin.y) * m_invTileSize));

    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const TileType tt = GetTile(tx, ty);
            if (tt == TileType::SpikeHazard) flags |= ContactFlags::InHazard;
            else if (tt == TileType::WaterVolume) flags |= ContactFlags::InWater;
        }
    }
    return flags;
}

f32 SpatialIndex2D::SampleSlopeSurfaceY(i32 tx, i32 ty, f32 worldX, Vec2& outNormal) const noexcept {
    const TileType tt = GetTile(tx, ty);
    const f32 tileMinX = m_origin.x + static_cast<f32>(tx) * m_tileSize;
    const f32 tileMinY = m_origin.y + static_cast<f32>(ty) * m_tileSize;
    const f32 u = std::clamp((worldX - tileMinX) * m_invTileSize, 0.0f, 1.0f);
    if (tt == TileType::SlopeUpRight) {
        outNormal = {-0.70710678f, 0.70710678f};
        return tileMinY + u * m_tileSize;
    }
    if (tt == TileType::SlopeUpLeft) {
        outNormal = {0.70710678f, 0.70710678f};
        return tileMinY + (1.0f - u) * m_tileSize;
    }
    outNormal = {0.0f, 1.0f};
    return tileMinY + m_tileSize;
}

u32 ContactListener2D::StepAndCollectTriggerEvents(
    const World& world,
    const SpatialIndex2D& spatial,
    std::span<TriggerContactEvent> outEvents) noexcept {
    u32 eventCount = 0;
    for (usize i = 0; i < m_pairCount; ++i) {
        m_pairs[i].presentCurrTick = false;
    }

    world.Each<TriggerVolume2D>([&](EntityId trigId, const TriggerVolume2D& trig) noexcept {
                const WorldTransform2D& wt = world.GetWorldByIndex(trigId.index);
        const Rect worldRect = trig.bounds.WorldRect({wt.x, wt.y});

        std::array<EntityId, 16> overlapping{};
        const u32 count = spatial.OverlapAabb(worldRect, 0xFFFFU, overlapping, trigId);
        for (u32 h = 0; h < count; ++h) {
            const EntityId actorId = overlapping[h];
            const u64 key = (static_cast<u64>(trigId.Packed()) << 32U) | static_cast<u64>(actorId.Packed());

            bool foundExisting = false;
            for (usize p = 0; p < m_pairCount; ++p) {
                if (m_pairs[p].pairKey == key) {
                    m_pairs[p].presentCurrTick = true;
                    foundExisting = true;
                    if (eventCount < outEvents.size()) {
                        outEvents[eventCount++] = TriggerContactEvent{
                            trigId, actorId, static_cast<u16>(trig.triggerTag.value), TriggerEventKind::OnStay, 0};
                    }
                    break;
                }
            }
            if (!foundExisting && m_pairCount < kMaxActivePairs) {
                m_pairs[m_pairCount++] = PairRecord{key, trigId, actorId, static_cast<u16>(trig.triggerTag.value), true, 0};
                if (eventCount < outEvents.size()) {
                    outEvents[eventCount++] = TriggerContactEvent{
                        trigId, actorId, static_cast<u16>(trig.triggerTag.value), TriggerEventKind::OnEnter, 0};
                }
            }
        }
    });

    usize writeIdx = 0;
    for (usize p = 0; p < m_pairCount; ++p) {
        if (m_pairs[p].presentCurrTick) {
            m_pairs[writeIdx++] = m_pairs[p];
        } else {
            if (eventCount < outEvents.size()) {
                outEvents[eventCount++] = TriggerContactEvent{
                    m_pairs[p].triggerEntity, m_pairs[p].actorEntity,
                    m_pairs[p].triggerTag, TriggerEventKind::OnExit, 0};
            }
        }
    }
    m_pairCount = writeIdx;
    return eventCount;
}

} // namespace en
