#include "physics/PhysicsSystem.h"

namespace en {

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

    const Collider2D&       col   = *world.TryGet<Collider2D>(mover);
    const WorldTransform2D& wt    = world.GetWorldByIndex(mover.index);
    Vec2                    pos   = {wt.x, wt.y};
    Vec2                    rem   = delta;
    constexpr f32           kSkin = 1e-3f;

    SweepResult2D firstHit{};
    for (u32 iter = 0U; iter < 2U; ++iter) {
        if (std::abs(rem.x) <= 1e-6f && std::abs(rem.y) <= 1e-6f) break;

        const Rect          box = col.WorldBounds(pos);
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
        Rect       platBounds{};
        u8         platOneWay = 0U;
        if (hasPlatCol) {
            const WorldTransform2D& wt   = world.GetWorldByIndex(platId.index);
            const Collider2D&       pCol = colPool.DirectAtEntityIndex(platId.index);
            platBounds = pCol.WorldBounds({wt.x, wt.y});
            platOneWay = (pCol.userTag == 1U || pCol.isTrigger != 0U) ? 1U : 0U;
        }

        LocalTransform2D& platLoc = world.ModifyLocalTransformByIndex(platId.index);
        platLoc.x += delta.x;
        platLoc.y += delta.y;
        anyMoved = true;

        if (!hasPlatCol || platBounds.IsEmpty()) continue;
        const f32  platTop = platBounds.Bottom();
        const Rect nextPlatBounds{
            platBounds.x + delta.x,
            platBounds.y + delta.y,
            platBounds.w,
            platBounds.h,
        };

        for (usize j = 0U; j < entities.size(); ++j) {
            PhysicsBody2D& rider = bodies[j];
            if (!rider.IsDynamic() || rider.enabled == 0U) continue;
            const EntityId riderId = entities[j];
            if (!world.Has<Collider2D>(riderId)) continue;

            const WorldTransform2D& rWt  = world.GetWorldByIndex(riderId.index);
            const Collider2D&       rCol = colPool.DirectAtEntityIndex(riderId.index);
            const Rect              rBox = rCol.WorldBounds({rWt.x, rWt.y});

            const f32 gapTol = (rider.carrier == platId) ? 0.12f : 0.06f;
            const bool onPlatform =
                (rBox.Right() > platBounds.x + 1e-3f) &&
                (rBox.x < platBounds.Right() - 1e-3f) &&
                (std::abs(rBox.y - platTop) <= gapTol) &&
                (rider.velocity.y <= 0.05f);

            if (onPlatform) {
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
                continue;
            }

            if (rider.carrier == platId) rider.carrier = kNullEntity;

            if (platOneWay == 0U && rCol.isTrigger == 0U && nextPlatBounds.Overlaps(rBox)) {
                const SweepResult2D sweepHit = SweepBoxVsBox(platBounds, delta, rBox);
                Vec2 pushDelta{};
                if (sweepHit && sweepHit.startSolid == 0U) {
                    pushDelta = delta * (1.0f - sweepHit.time);
                } else {
                    const f32 pushL = nextPlatBounds.x - rBox.Right();
                    const f32 pushR = nextPlatBounds.Right() - rBox.x;
                    const f32 pushD = nextPlatBounds.y - rBox.Bottom();
                    const f32 pushU = nextPlatBounds.Bottom() - rBox.y;
                    if (std::min(std::abs(pushL), std::abs(pushR)) <
                        std::min(std::abs(pushD), std::abs(pushU))) {
                        pushDelta.x = (std::abs(pushL) < std::abs(pushR)) ? pushL : pushR;
                    } else {
                        pushDelta.y = (std::abs(pushD) < std::abs(pushU)) ? pushD : pushU;
                    }
                }

                if (std::abs(pushDelta.x) > 1e-6f || std::abs(pushDelta.y) > 1e-6f) {
                    const SweepResult2D pinHit = m_grid.SweepBox(
                        rBox, pushDelta, rCol.queryMask, riderId, rider.ignoreOneWay != 0U);
                    LocalTransform2D& rLoc = world.ModifyLocalTransformByIndex(riderId.index);
                    if (pinHit && pinHit.entity != platId && pinHit.time < 1.0f) {
                        const f32 safeT = std::max(0.0f, pinHit.time - 1e-3f);
                        rLoc.x += pushDelta.x * safeT;
                        rLoc.y += pushDelta.y * safeT;
                        if (pinHit.time < 0.95f && pushDelta.Dot(pinHit.normal) < -1e-4f) {
                            rider.contacts |= ContactFlags::Crushed;
                        }
                    } else {
                        rLoc.x += pushDelta.x;
                        rLoc.y += pushDelta.y;
                    }
                }
            }
        }
    }

    if (anyMoved) {
        world.PropagateTransforms();
        m_grid.Rebuild(world);
    }
}

u64 PhysicsSystem::ComputeAuditHash(const World& world) const noexcept {
    u64 h = HashCombine(0xCBF29CE484222325ULL, m_stepCount);
    h     = HashCombine(h, m_prevPairCount);
    if ((world.RegisteredPoolsMask() & (1ULL << PhysicsBody2D::kTypeId)) != 0ULL) {
        const auto& pool  = world.GetPool<PhysicsBody2D>();
        const auto  bytes = std::as_bytes(pool.Components());
        h = HashCombine(h, Crc32({reinterpret_cast<const u8*>(bytes.data()), bytes.size()}));
    }
    return h;
}

} // namespace en
