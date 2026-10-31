#include "physics/PhysicsSystem.h"

namespace en {
namespace {

using ActivePair = PhysicsSystem::ActivePair;
constexpr f32 kMinGroundNormalY = 0.65f;

[[nodiscard]] constexpr u64 MakePairKey(EntityId a, EntityId b) noexcept {
    if (b.index < a.index) std::swap(a, b);
    return (static_cast<u64>(a.Packed()) << 32U) | static_cast<u64>(b.Packed());
}

[[nodiscard]] constexpr EntityId UnpackPairA(u64 key) noexcept {
    return EntityId::FromPacked(static_cast<u32>(key >> 32U));
}

[[nodiscard]] constexpr EntityId UnpackPairB(u64 key) noexcept {
    return EntityId::FromPacked(static_cast<u32>(key & 0xFFFFFFFFULL));
}

[[nodiscard]] u16 SortAndUniquePairs(ActivePair* pairs, u16 count) noexcept {
    if (count <= 1U) return count;
    std::span<ActivePair> span{pairs, count};
    std::sort(span.begin(), span.end(),
              [](const ActivePair& a, const ActivePair& b) noexcept { return a.key < b.key; });
    const auto endIt = std::unique(
        span.begin(), span.end(),
        [](const ActivePair& a, const ActivePair& b) noexcept { return a.key == b.key; });
    return static_cast<u16>(std::distance(span.begin(), endIt));
}

} // namespace

void PhysicsSystem::StepDynamicBodies(World& world, const TickStep& step) noexcept {
    auto& bodyPool      = world.GetPool<PhysicsBody2D>();
    const auto entities = bodyPool.Entities();
    auto bodies         = bodyPool.Components();
    if (entities.empty()) return;

    const auto& colPool = world.GetPool<Collider2D>();
    bool anyMoved       = false;

    for (usize i = 0U; i < entities.size(); ++i) {
        PhysicsBody2D& body = bodies[i];
        if (!body.IsDynamic() || body.enabled == 0U) continue;

        const EntityId id = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) continue;
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) continue;

        const bool wasGrounded = body.IsGrounded();
        body.contacts &= ContactFlags::Crushed;

        body.velocity += m_cfg.gravity * (body.gravityScale * dt);
        if (body.linearDamping > 0.0f) {
            body.velocity *= 1.0f / (1.0f + body.linearDamping * dt);
        }

        if (!world.Has<Collider2D>(id)) {
            LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
            loc.x += body.velocity.x * dt;
            loc.y += body.velocity.y * dt;
            anyMoved = true;
            continue;
        }

        const Collider2D& col = colPool.DirectAtEntityIndex(id.index);
        if (col.isTrigger != 0U) {
            LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
            loc.x += body.velocity.x * dt;
            loc.y += body.velocity.y * dt;
            anyMoved = true;
            continue;
        }

        const WorldTransform2D& wt    = world.GetWorldByIndex(id.index);
        Vec2                    pos   = {wt.x, wt.y};
        Vec2                    rem   = body.velocity * dt;
        const bool              noOW  = body.ignoreOneWay != 0U;
        constexpr f32           kSkin = 1e-3f;

        const auto markGround = [&](const SweepResult2D& h) noexcept {
            body.contacts |= ContactFlags::Ground;
            if (h.oneWay != 0U) body.contacts |= ContactFlags::OneWay;
            if (h.normal.y < 0.999f) {
                body.contacts |= ContactFlags::Slope;
                const f32 vn = body.velocity.Dot(h.normal);
                if (vn < 0.0f) body.velocity -= h.normal * vn;
            } else if (body.velocity.y < 0.0f) {
                body.velocity.y = (body.restitution > 0.0f && -body.velocity.y > 1.0f)
                    ? (-body.velocity.y * body.restitution)
                    : 0.0f;
            }
            if (h.entity.IsValid()) body.carrier = h.entity;
        };

        // Pre-elevate vertical displacement when walking uphill on a slope tile so the collider
        // rises along the slope surface before testing adjacent flat plateau tiles.
        if ((col.queryMask & 0x0001U) != 0U && m_grid.TileSize() > 0.0f && std::abs(rem.x) > 1e-6f) {
            const Rect preBox  = col.WorldBounds({pos.x + rem.x, pos.y});
            const f32  footX   = preBox.Center().x;
            const f32  invTile = 1.0f / m_grid.TileSize();
            const i32  tx      = static_cast<i32>(std::floor((footX - m_grid.Origin().x) * invTile));
            const i32  ty      = static_cast<i32>(std::floor((preBox.y + 0.05f - m_grid.Origin().y) * invTile));
            const TileFlag tf  = m_grid.SampleTile(tx, ty);
            if (IsSlopeTile(tf)) {
                const Rect tRect  = m_grid.TileWorldBounds(tx, ty);
                const f32  u      = (footX - tRect.x) / tRect.w;
                const f32  surfY  = tRect.y + SampleSlopeHeight(tf, u, tRect.h);
                if (preBox.y < surfY && surfY - preBox.y <= m_grid.TileSize()) {
                    pos.y += (surfY - preBox.y);
                }
            }
        }

        for (u32 iter = 0U; iter < 3U; ++iter) {
            if (std::abs(rem.x) <= 1e-6f && std::abs(rem.y) <= 1e-6f) break;

            const Rect box          = col.WorldBounds(pos);
            const SweepResult2D hit = m_grid.SweepBox(box, rem, col.queryMask, id, noOW);
            if (!hit) {
                pos += rem;
                break;
            }

            if (hit.startSolid != 0U) {
                pos += hit.normal * (hit.penetration + kSkin);
            } else {
                const f32 safeT = std::max(0.0f, hit.time - kSkin);
                pos += rem * safeT;
            }

            const f32 fracRem = 1.0f - hit.time;
            rem *= fracRem;

            // Upward jump ceiling corner correction when clipping the outer edge of an overhead block.
            if (hit.normal.y < -0.7f && rem.y > 1e-5f && m_cfg.cornerCorrectionDist > 1e-3f) {
                const Rect curBox = col.WorldBounds(pos);
                bool corrected    = false;
                for (const f32 sign : {-1.0f, 1.0f}) {
                    const f32 nudgeX = sign * m_cfg.cornerCorrectionDist;
                    if (m_grid.SweepBox(curBox, {nudgeX, 0.0f}, col.queryMask, id, true)) continue;
                    const Vec2 nudgedPos{pos.x + nudgeX, pos.y};
                    if (!m_grid.SweepBox(col.WorldBounds(nudgedPos), {0.0f, rem.y}, col.queryMask, id, true)) {
                        pos   = nudgedPos;
                        pos.y += rem.y;
                        rem.y = 0.0f;
                        corrected = true;
                        break;
                    }
                }
                if (corrected) continue;
            }

            // Ground step-up & airborne Ledge Hop corner correction:
            if (std::abs(hit.normal.x) > 0.9f && m_cfg.maxStepHeight > 1e-3f &&
                (wasGrounded || body.velocity.y <= m_cfg.ledgeHopMaxVy)) {
                const Rect curBox = col.WorldBounds(pos);
                if (!m_grid.SweepBox(curBox, {0.0f, m_cfg.maxStepHeight}, col.queryMask, id, true)) {
                    const Vec2 raisedPos{pos.x, pos.y + m_cfg.maxStepHeight};
                    const Rect raisedBox = col.WorldBounds(raisedPos);
                    const f32  probeX    = (std::abs(rem.x) > 0.04f)
                        ? rem.x
                        : ((hit.normal.x < 0.0f) ? 0.05f : -0.05f);
                    const SweepResult2D fwd = m_grid.SweepBox(raisedBox, {probeX, 0.0f}, col.queryMask, id, true);
                    if (!fwd || fwd.time > 0.25f) {
                        const f32  advX    = fwd ? (probeX * std::max(0.0f, fwd.time - kSkin)) : probeX;
                        const Vec2 stepPos = {raisedPos.x + advX, raisedPos.y};
                        const SweepResult2D down = m_grid.SweepBox(
                            col.WorldBounds(stepPos), {0.0f, -m_cfg.maxStepHeight}, col.queryMask, id, noOW);
                        if (down && down.normal.y >= kMinGroundNormalY && down.time < 0.98f) {
                            pos.x = stepPos.x;
                            pos.y = stepPos.y - m_cfg.maxStepHeight * std::max(0.0f, down.time - kSkin);
                            if (std::abs(rem.x) >= std::abs(advX)) rem.x -= advX;
                            else rem.x = 0.0f;
                            if (body.velocity.y <= 2.0f) {
                                rem.y = 0.0f;
                                markGround(down);
                            }
                            continue;
                        }
                    }
                }
            }

            if (hit.normal.y >= kMinGroundNormalY) {
                markGround(hit);
                if (body.friction > 0.0f) {
                    body.velocity.x *= std::max(0.0f, 1.0f - body.friction * dt);
                }
            } else {
                if (hit.normal.y < -0.7f) {
                    body.contacts |= ContactFlags::Ceiling;
                    if (body.velocity.y > 0.0f) {
                        body.velocity.y = (body.restitution > 0.0f)
                            ? (-body.velocity.y * body.restitution)
                            : 0.0f;
                    }
                }
                if (hit.normal.x > 0.7f) {
                    body.contacts |= ContactFlags::WallLeft;
                    if (body.velocity.x < 0.0f) {
                        body.velocity.x = (body.restitution > 0.0f)
                            ? (-body.velocity.x * body.restitution)
                            : 0.0f;
                    }
                } else if (hit.normal.x < -0.7f) {
                    body.contacts |= ContactFlags::WallRight;
                    if (body.velocity.x > 0.0f) {
                        body.velocity.x = (body.restitution > 0.0f)
                            ? (-body.velocity.x * body.restitution)
                            : 0.0f;
                    }
                }
            }

            const f32 dot = rem.Dot(hit.normal);
            if (dot < 0.0f) {
                rem -= hit.normal * (dot * (1.0f + body.restitution));
            }
        }

        // Analytical 2D Slope Heightmap Resolution & Downhill Slope Snapping:
        // Evaluates the tile column containing the actor's foot center X (current row and row below).
        if ((col.queryMask & 0x0001U) != 0U && m_grid.TileSize() > 0.0f) {
            const Rect curBox      = col.WorldBounds(pos);
            const f32  footX       = curBox.Center().x;
            const f32  invTile     = 1.0f / m_grid.TileSize();
            const i32  tx          = static_cast<i32>(std::floor((footX - m_grid.Origin().x) * invTile));
            const i32  baseTy      = static_cast<i32>(std::floor((curBox.y + 0.05f - m_grid.Origin().y) * invTile));
            const f32  horizTravel = std::abs(body.velocity.x) * dt;
            const f32  snapDist    = std::max(m_cfg.groundSnapDist, horizTravel * m_cfg.slopeSnapMultiplier);

            for (i32 dy = 0; dy >= -1; --dy) {
                const i32      ty = baseTy + dy;
                const TileFlag tf = m_grid.SampleTile(tx, ty);
                if (!IsSlopeTile(tf)) continue;

                const Rect tRect = m_grid.TileWorldBounds(tx, ty);
                const f32  u     = (footX - tRect.x) / tRect.w;
                const f32  surfY = tRect.y + SampleSlopeHeight(tf, u, tRect.h);
                const f32  diffY = surfY - curBox.y;

                const bool penetrating = (diffY >= -kSkin && diffY <= m_grid.TileSize() + 0.1f &&
                                          (wasGrounded || body.velocity.y <= 0.1f));
                const bool snappingDown = (!penetrating && wasGrounded && body.velocity.y <= 0.1f &&
                                           diffY < 0.0f && -diffY <= snapDist);

                if (penetrating || snappingDown) {
                    pos.y += diffY;
                    body.contacts |= (ContactFlags::Ground | ContactFlags::Slope);
                    const Vec2 n  = SlopeNormal(tf);
                    const f32  vn = body.velocity.Dot(n);
                    if (vn < 0.0f || snappingDown) {
                        const Vec2 vSlope = ProjectVelocityOnSlope(body.velocity, n);
                        body.velocity.y   = vSlope.y;
                    }
                    break;
                }
            }
        }

        // High-speed Flat/Entity Ground Adhesion & Downhill Snapping:
        if (!body.IsGrounded() && wasGrounded && body.velocity.y <= 0.0f && m_cfg.groundSnapDist > 0.0f) {
            const f32 horizTravel = std::abs(body.velocity.x) * dt;
            const f32 snapDist    = std::max(m_cfg.groundSnapDist, horizTravel * m_cfg.slopeSnapMultiplier);
            const SweepResult2D snap = m_grid.SweepBox(
                col.WorldBounds(pos), {0.0f, -snapDist}, col.queryMask, id, noOW);
            if (snap && snap.normal.y >= kMinGroundNormalY && snap.time > 0.0f && snap.time < 1.0f) {
                pos.y -= snapDist * std::max(0.0f, snap.time - kSkin);
                markGround(snap);
            }
        }

        if (!body.IsGrounded()) body.carrier = kNullEntity;

        LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
        loc.x += pos.x - wt.x;
        loc.y += pos.y - wt.y;
        anyMoved = true;
    }

    if (anyMoved) world.PropagateTransforms();
}

void PhysicsSystem::GatherContactEvents(const World& world) noexcept {
    if (!m_pairs || !m_events) return;

    const u8   prevFlip = m_pairFlip;
    const u8   nextFlip = static_cast<u8>(1U - prevFlip);
    const auto prevSpan = std::span<const ActivePair>{
        m_pairs + static_cast<usize>(prevFlip) * m_cfg.maxContactPairs,
        m_prevPairCount,
    };
    ActivePair* nextPairs = m_pairs + static_cast<usize>(nextFlip) * m_cfg.maxContactPairs;
    u16         rawCount  = 0U;

    const auto& colPool = world.GetPool<Collider2D>();
    const auto  proxies = m_grid.Proxies();

    // Phase 1: O(1) candidate append per overlapping grid pair (no inner O(N) duplicate scan).
    const auto recordPair = [&](const SpatialProxy2D& pA, const SpatialProxy2D& pB) noexcept {
        if (pA.entity == pB.entity) return;

        const Collider2D& cA = colPool.DirectAtEntityIndex(pA.entity.index);
        const Collider2D& cB = colPool.DirectAtEntityIndex(pB.entity.index);
        if ((cA.queryMask & cB.layerMask) == 0U || (cB.queryMask & cA.layerMask) == 0U) return;
        if (cA.isStatic != 0U && cB.isStatic != 0U) return;

        Rect expandedA = pA.bounds;
        expandedA.x -= 2e-3f;
        expandedA.y -= 2e-3f;
        expandedA.w += 4e-3f;
        expandedA.h += 4e-3f;
        if (!expandedA.Overlaps(pB.bounds)) return;

        if (rawCount >= m_cfg.maxContactPairs) {
            rawCount = SortAndUniquePairs(nextPairs, rawCount);
            if (rawCount >= m_cfg.maxContactPairs) return;
        }

        const u8  isTrig = (cA.isTrigger != 0U || cB.isTrigger != 0U) ? 1U : 0U;
        const u16 tag    = cA.userTag != 0U ? cA.userTag : cB.userTag;
        nextPairs[rawCount++] = ActivePair{
            .key       = MakePairKey(pA.entity, pB.entity),
            .userTag   = tag,
            .isTrigger = isTrig,
        };
    };

    const auto offsets = m_grid.CellOffsets();
    const auto refs    = m_grid.CellRefs();
    for (u32 c = 0U; c < SpatialGrid2D::kGridCells; ++c) {
        const u32 begin = offsets[c];
        const u32 end   = offsets[c + 1U];
        for (u32 i = begin; i < end; ++i) {
            for (u32 j = i + 1U; j < end; ++j) {
                recordPair(proxies[refs[i]], proxies[refs[j]]);
            }
        }
    }

    for (const u16 overIdx : m_grid.OversizedRefs()) {
        for (u16 i = 0U; i < proxies.size(); ++i) {
            if (i != overIdx) recordPair(proxies[overIdx], proxies[i]);
        }
    }

    // Phase 2: Single batch Sort-and-Unique pass over all recorded pairs.
    const u16 nextCount = SortAndUniquePairs(nextPairs, rawCount);
    const std::span<const ActivePair> curSpan{nextPairs, nextCount};

    // Phase 3: O(N + M) two-pointer merge scan between prevSpan and curSpan to emit Begin/End events.
    usize iPrev = 0U;
    usize iCur  = 0U;
    while (iPrev < prevSpan.size() || iCur < curSpan.size()) {
        if (iCur < curSpan.size() &&
            (iPrev >= prevSpan.size() || curSpan[iCur].key < prevSpan[iPrev].key)) {
            const ActivePair& cur = curSpan[iCur++];
            if (m_eventCount < m_cfg.maxContactPairs) {
                const EntityId eA = UnpackPairA(cur.key);
                const EntityId eB = UnpackPairB(cur.key);
                Vec2 point{};
                Vec2 normal{1.0f, 0.0f};
                if (world.Has<Collider2D>(eA) && world.Has<Collider2D>(eB)) {
                    const WorldTransform2D& wtA = world.GetWorldByIndex(eA.index);
                    const WorldTransform2D& wtB = world.GetWorldByIndex(eB.index);
                    Rect bA = colPool.DirectAtEntityIndex(eA.index).WorldBounds({wtA.x, wtA.y});
                    bA.x -= 2e-3f;
                    bA.y -= 2e-3f;
                    bA.w += 4e-3f;
                    bA.h += 4e-3f;
                    const Rect bB = colPool.DirectAtEntityIndex(eB.index).WorldBounds({wtB.x, wtB.y});
                    point  = bA.Intersection(bB).Center();
                    normal = (bB.Center() - bA.Center()).Normalized();
                }
                m_events[m_eventCount++] = ContactEvent2D{
                    .entityA   = eA,
                    .entityB   = eB,
                    .point     = point,
                    .normal    = normal,
                    .kind      = cur.isTrigger ? ContactEventKind::TriggerEnter : ContactEventKind::BeginContact,
                    .isTrigger = cur.isTrigger,
                    .userTag   = cur.userTag,
                };
            }
        } else if (iPrev < prevSpan.size() &&
                   (iCur >= curSpan.size() || prevSpan[iPrev].key < curSpan[iCur].key)) {
            const ActivePair& prev = prevSpan[iPrev++];
            if (m_eventCount < m_cfg.maxContactPairs) {
                m_events[m_eventCount++] = ContactEvent2D{
                    .entityA   = UnpackPairA(prev.key),
                    .entityB   = UnpackPairB(prev.key),
                    .kind      = prev.isTrigger ? ContactEventKind::TriggerExit : ContactEventKind::EndContact,
                    .isTrigger = prev.isTrigger,
                    .userTag   = prev.userTag,
                };
            }
        } else {
            ++iPrev;
            ++iCur;
        }
    }

    m_pairFlip      = nextFlip;
    m_prevPairCount = nextCount;
}

} // namespace en
