#include "physics/PhysicsSystem.h"

namespace en {

namespace {

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

} // namespace

Result<void, PhysicsErrc> PhysicsSystem::Init(World& world, const PhysicsConfig& cfg) noexcept {
    Shutdown();
    if (cfg.maxProxies == 0U || cfg.maxContactPairs == 0U ||
        cfg.cellSize <= 0.05f || cfg.arenaBytes < 65536U) {
        return Err(PhysicsErrc::InvalidConfig);
    }

    if (!world.RegisterComponentPool<Collider2D>().has_value() ||
        !world.RegisterComponentPool<PhysicsBody2D>().has_value()) {
        return Err(PhysicsErrc::PoolRegistrationFailed);
    }

    if (!m_arena.Init(cfg.arenaBytes)) {
        return Err(PhysicsErrc::OutOfMemory);
    }

    auto events = m_arena.Alloc<ContactEvent2D>(cfg.maxContactPairs);
    auto pairs  = m_arena.Alloc<ActivePair>(static_cast<usize>(cfg.maxContactPairs) * 2U);
    if (events.empty() || pairs.empty() || !m_grid.Init(m_arena, cfg.maxProxies, cfg.cellSize)) {
        Shutdown();
        return Err(PhysicsErrc::OutOfMemory);
    }

    m_cfg         = cfg;
    m_events      = events.data();
    m_pairs       = pairs.data();
    m_initialized = 1U;
    return {};
}

void PhysicsSystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_grid          = {};
    m_events        = nullptr;
    m_pairs         = nullptr;
    m_stepCount     = 0ULL;
    m_eventCount    = 0U;
    m_prevPairCount = 0U;
    m_pairFlip      = 0U;
    m_initialized   = 0U;
}

void PhysicsSystem::Clear() noexcept {
    m_grid.Clear();
    m_eventCount    = 0U;
    m_prevPairCount = 0U;
    m_pairFlip      = 0U;
}

void PhysicsSystem::Step(World& world, const TickStep& step) noexcept {
    if (!m_initialized || !step) return;

    m_eventCount = 0U;
    world.PropagateTransforms();
    m_grid.Rebuild(world);

    StepKinematicPlatforms(world, step);
    StepDynamicBodies(world, step);

    m_grid.Rebuild(world);
    GatherContactEvents(world);
    ++m_stepCount;
}

SweepResult2D PhysicsSystem::SweepMove(
    World& world,
    EntityId mover,
    Vec2 delta,
    bool ignoreOneWay) noexcept {
    if (!m_initialized || !world.Has<Collider2D>(mover)) return {};

    world.PropagateTransforms();
    m_grid.Rebuild(world);

    const Collider2D&       col  = *world.TryGet<Collider2D>(mover);
    const WorldTransform2D& wt   = world.GetWorldByIndex(mover.index);
    Vec2                    pos  = {wt.x, wt.y};
    const Vec2              scl  = {wt.scaleX, wt.scaleY};
    Vec2                    rem  = delta;
    constexpr f32           kSkin = 1e-3f;

    SweepResult2D firstHit{};
    for (u32 iter = 0U; iter < 2U; ++iter) {
        if (std::abs(rem.x) <= 1e-6f && std::abs(rem.y) <= 1e-6f) break;

        const Rect          box = col.WorldBounds(pos, scl);
        const SweepResult2D hit = m_grid.SweepBox(box, rem, col.queryMask, mover, ignoreOneWay);
        if (!hit) {
            pos += rem;
            break;
        }
        if (!firstHit) firstHit = hit;

        if (hit.startSolid != 0U) {
            pos += hit.normal * (hit.penetration + kSkin);
        } else {
            const f32 safeT = std::max(0.0f, hit.time - kSkin);
            pos += rem * safeT;
        }

        rem *= (1.0f - hit.time);
        const f32 dot = rem.Dot(hit.normal);
        if (dot < 0.0f) {
            rem -= hit.normal * dot;
        }
    }

    LocalTransform2D& loc = world.ModifyLocalTransformByIndex(mover.index);
    loc.x += pos.x - wt.x;
    loc.y += pos.y - wt.y;
    world.PropagateTransforms();
    m_grid.Rebuild(world);
    return firstHit;
}

void PhysicsSystem::StepKinematicPlatforms(World& world, const TickStep& step) noexcept {
    auto& bodyPool      = world.GetPool<PhysicsBody2D>();
    const auto entities = bodyPool.Entities();
    auto bodies         = bodyPool.Components();
    if (entities.empty()) return;

    const auto& colPool = world.GetPool<Collider2D>();
    bool anyMoved       = false;

    for (usize i = 0U; i < entities.size(); ++i) {
        const PhysicsBody2D& platBody = bodies[i];
        if (!platBody.IsKinematic() || platBody.enabled == 0U) continue;

        const EntityId platId = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(platId.index));
        if (step.IsFrozen(ch)) continue;

        const f32  dt    = step.Dt(ch);
        const Vec2 delta = platBody.velocity * dt;
        if (std::abs(delta.x) <= 1e-7f && std::abs(delta.y) <= 1e-7f) continue;

        const bool hasPlatCol = world.Has<Collider2D>(platId);
        Rect platBounds{};
        if (hasPlatCol) {
            const WorldTransform2D& wt = world.GetWorldByIndex(platId.index);
            platBounds = colPool.DirectAtEntityIndex(platId.index).WorldBounds({wt.x, wt.y}, {wt.scaleX, wt.scaleY});
        }

        LocalTransform2D& platLoc = world.ModifyLocalTransformByIndex(platId.index);
        platLoc.x += delta.x;
        platLoc.y += delta.y;
        anyMoved = true;

        if (!hasPlatCol || platBounds.IsEmpty()) continue;
        const f32 platTop = platBounds.Bottom();

        for (usize j = 0U; j < entities.size(); ++j) {
            PhysicsBody2D& rider = bodies[j];
            if (!rider.IsDynamic() || rider.enabled == 0U) continue;
            const EntityId riderId = entities[j];
            if (!world.Has<Collider2D>(riderId)) continue;

            const WorldTransform2D& rWt  = world.GetWorldByIndex(riderId.index);
            const Collider2D&       rCol = colPool.DirectAtEntityIndex(riderId.index);
            const Rect              rBox = rCol.WorldBounds({rWt.x, rWt.y}, {rWt.scaleX, rWt.scaleY});

            const f32 gapTol = (rider.carrier == platId) ? 0.12f : 0.06f;
            const bool onPlatform =
                (rBox.Right() > platBounds.x + 1e-3f) &&
                (rBox.x < platBounds.Right() - 1e-3f) &&
                (std::abs(rBox.y - platTop) <= gapTol) &&
                (rider.velocity.y <= 0.05f);

            if (!onPlatform) {
                if (rider.carrier == platId) rider.carrier = kNullEntity;
                continue;
            }

            const SweepResult2D hit = m_grid.SweepBox(
                rBox, delta, rCol.queryMask, riderId, rider.ignoreOneWay != 0U);
            LocalTransform2D& rLoc = world.ModifyLocalTransformByIndex(riderId.index);
            if (hit && hit.entity != platId && hit.time < 1.0f) {
                const f32 safeT = std::max(0.0f, hit.time - 1e-3f);
                rLoc.x += delta.x * safeT;
                rLoc.y += delta.y * safeT;
                if (hit.time < 0.05f && delta.Dot(hit.normal) < -1e-3f) {
                    rider.contacts |= ContactFlags::Crushed;
                }
            } else {
                rLoc.x += delta.x;
                rLoc.y += delta.y;
            }
            rider.carrier   = platId;
            rider.contacts |= ContactFlags::Ground;
        }
    }

    if (anyMoved) {
        world.PropagateTransforms();
        m_grid.Rebuild(world);
    }
}

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

        const WorldTransform2D& wt   = world.GetWorldByIndex(id.index);
        Vec2                    pos  = {wt.x, wt.y};
        const Vec2              scl  = {wt.scaleX, wt.scaleY};
        Vec2                    rem  = body.velocity * dt;
        const bool              noOW = body.ignoreOneWay != 0U;
        constexpr f32           kSkin = 1e-3f;

        const auto markGround = [&](const SweepResult2D& h) noexcept {
            body.contacts |= ContactFlags::Ground;
            if (h.oneWay != 0U) body.contacts |= ContactFlags::OneWay;
            if (h.normal.y < 0.999f) body.contacts |= ContactFlags::Slope;
            if (h.entity.IsValid()) body.carrier = h.entity;
            if (body.velocity.y < 0.0f) {
                body.velocity.y = (body.restitution > 0.0f && -body.velocity.y > 1.0f)
                    ? (-body.velocity.y * body.restitution)
                    : 0.0f;
            }
        };

        for (u32 iter = 0U; iter < 3U; ++iter) {
            if (std::abs(rem.x) <= 1e-6f && std::abs(rem.y) <= 1e-6f) break;

            const Rect box         = col.WorldBounds(pos, scl);
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

            // Upward jump corner correction (Celeste / Hollow Knight style):
            // if moving upward and clipping the outer edge of a ceiling block, nudge horizontally into open air.
            if (hit.normal.y < -0.7f && rem.y > 1e-5f && m_cfg.cornerCorrectionDist > 1e-3f) {
                const Rect curBox = col.WorldBounds(pos, scl);
                bool corrected    = false;
                for (const f32 sign : {-1.0f, 1.0f}) {
                    const f32 nudgeX = sign * m_cfg.cornerCorrectionDist;
                    if (m_grid.SweepBox(curBox, {nudgeX, 0.0f}, col.queryMask, id, true)) continue;
                    const Vec2 nudgedPos{pos.x + nudgeX, pos.y};
                    if (!m_grid.SweepBox(col.WorldBounds(nudgedPos, scl), {0.0f, rem.y}, col.queryMask, id, true)) {
                        pos   = nudgedPos;
                        pos.y += rem.y;
                        rem.y = 0.0f;
                        corrected = true;
                        break;
                    }
                }
                if (corrected) continue;
            }

            // Step-up over small ledges when moving horizontally along the ground.
            if (std::abs(hit.normal.x) > 0.9f && m_cfg.maxStepHeight > 1e-3f &&
                (wasGrounded || body.velocity.y <= 0.0f)) {
                const Rect curBox = col.WorldBounds(pos, scl);
                if (!m_grid.SweepBox(curBox, {0.0f, m_cfg.maxStepHeight}, col.queryMask, id, true)) {
                    const Vec2 raisedPos{pos.x, pos.y + m_cfg.maxStepHeight};
                    const Rect raisedBox = col.WorldBounds(raisedPos, scl);
                    const SweepResult2D fwd = m_grid.SweepBox(raisedBox, {rem.x, 0.0f}, col.queryMask, id, true);
                    if (!fwd || fwd.time > 0.25f) {
                        const f32 advX     = fwd ? (rem.x * std::max(0.0f, fwd.time - kSkin)) : rem.x;
                        const Vec2 stepPos = {raisedPos.x + advX, raisedPos.y};
                        const SweepResult2D down = m_grid.SweepBox(
                            col.WorldBounds(stepPos, scl), {0.0f, -m_cfg.maxStepHeight}, col.queryMask, id, noOW);
                        if (down && down.normal.y >= kMinGroundNormalY) {
                            pos.x  = stepPos.x;
                            pos.y  = stepPos.y - m_cfg.maxStepHeight * std::max(0.0f, down.time - kSkin);
                            rem.x -= advX;
                            rem.y  = 0.0f;
                            markGround(down);
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

        // Downward ground snap when walking down slopes or small steps.
        if (!body.IsGrounded() && wasGrounded && body.velocity.y <= 0.0f && m_cfg.groundSnapDist > 0.0f) {
            const SweepResult2D snap = m_grid.SweepBox(
                col.WorldBounds(pos, scl), {0.0f, -m_cfg.groundSnapDist}, col.queryMask, id, noOW);
            if (snap && snap.normal.y >= kMinGroundNormalY && snap.time > 0.0f && snap.time < 1.0f) {
                pos.y -= m_cfg.groundSnapDist * std::max(0.0f, snap.time - kSkin);
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
    u16         nextCount = 0U;

    const auto& colPool = world.GetPool<Collider2D>();
    const auto  proxies = m_grid.Proxies();

    const auto recordPair = [&](const SpatialProxy2D& pA, const SpatialProxy2D& pB) noexcept {
        if (nextCount >= m_cfg.maxContactPairs || pA.entity == pB.entity) return;

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

        const u64 key = MakePairKey(pA.entity, pB.entity);
        for (u16 k = 0U; k < nextCount; ++k) {
            if (nextPairs[k].key == key) return;
        }

        const u8  isTrig = (cA.isTrigger != 0U || cB.isTrigger != 0U) ? 1U : 0U;
        const u16 tag    = cA.userTag != 0U ? cA.userTag : cB.userTag;
        nextPairs[nextCount++] = ActivePair{.key = key, .userTag = tag, .isTrigger = isTrig};

        const auto it = std::lower_bound(
            prevSpan.begin(), prevSpan.end(), key,
            [](const ActivePair& a, u64 k) noexcept { return a.key < k; });
        if (it == prevSpan.end() || it->key != key) {
            if (m_eventCount < m_cfg.maxContactPairs) {
                const Rect overlap = expandedA.Intersection(pB.bounds);
                const Vec2 delta   = pB.bounds.Center() - pA.bounds.Center();
                m_events[m_eventCount++] = ContactEvent2D{
                    .entityA   = UnpackPairA(key),
                    .entityB   = UnpackPairB(key),
                    .point     = overlap.Center(),
                    .normal    = delta.Normalized(),
                    .kind      = isTrig ? ContactEventKind::TriggerEnter : ContactEventKind::BeginContact,
                    .isTrigger = isTrig,
                    .userTag   = tag,
                };
            }
        }
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

    std::span<ActivePair> curSpan{nextPairs, nextCount};
    std::sort(curSpan.begin(), curSpan.end(),
              [](const ActivePair& a, const ActivePair& b) noexcept { return a.key < b.key; });

    for (const ActivePair& prev : prevSpan) {
        const auto it = std::lower_bound(
            curSpan.begin(), curSpan.end(), prev.key,
            [](const ActivePair& a, u64 k) noexcept { return a.key < k; });
        if (it == curSpan.end() || it->key != prev.key) {
            if (m_eventCount < m_cfg.maxContactPairs) {
                m_events[m_eventCount++] = ContactEvent2D{
                    .entityA   = UnpackPairA(prev.key),
                    .entityB   = UnpackPairB(prev.key),
                    .kind      = prev.isTrigger ? ContactEventKind::TriggerExit : ContactEventKind::EndContact,
                    .isTrigger = prev.isTrigger,
                    .userTag   = prev.userTag,
                };
            }
        }
    }

    m_pairFlip      = nextFlip;
    m_prevPairCount = nextCount;
}

u64 PhysicsSystem::ComputeAuditHash(const World& world) const noexcept {
    u64 h = HashCombine(0xCBF29CE484222325ULL, m_stepCount);
    h     = HashCombine(h, m_prevPairCount);
    if ((world.RegisteredPoolsMask() & (1ULL << PhysicsBody2D::kTypeId)) != 0ULL) {
        const auto& pool = world.GetPool<PhysicsBody2D>();
        const auto  bytes = std::as_bytes(pool.Components());
        h = HashCombine(h, Crc32({reinterpret_cast<const u8*>(bytes.data()), bytes.size()}));
    }
    return h;
}

} // namespace en
