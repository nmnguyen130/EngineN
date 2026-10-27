#include "physics/PhysicsSystem.h"

#include "assets/AssetBank.h"

namespace en {

Result<void, PhysicsErrc> PhysicsSystem::Init(World& world, const PhysicsConfig& cfg) noexcept {
    Shutdown();
    if (cfg.maxManifolds == 0U || cfg.maxContactEvents == 0U ||
        cfg.maxSpatialProxies == 0U || cfg.arenaBytes < 65536U) {
        return Err(PhysicsErrc::InvalidConfig);
    }

    if (!world.RegisterComponentPool<Collider2D>().has_value() ||
        !world.RegisterComponentPool<RigidBody2D>().has_value() ||
        !world.RegisterComponentPool<CharacterMotor2D>().has_value()) {
        return Err(PhysicsErrc::PoolRegistrationFailed);
    }

    if (!m_arena.Init(cfg.arenaBytes)) {
        return Err(PhysicsErrc::OutOfMemory);
    }

    auto manifoldsSpan = m_arena.Carve<ContactManifold2D>(static_cast<usize>(cfg.maxManifolds) * 2U);
    auto eventsSpan = m_arena.Carve<ContactEvent2D>(cfg.maxContactEvents);
    if (manifoldsSpan.empty() || eventsSpan.empty() ||
        !m_spatial.Init(m_arena, cfg.maxSpatialProxies)) {
        Shutdown();
        return Err(PhysicsErrc::OutOfMemory);
    }

    m_cfg = cfg;
    m_manifolds = manifoldsSpan.data();
    m_events = eventsSpan.data();
    m_initialized = 1U;
    return {};
}

void PhysicsSystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_spatial = SpatialIndex2D{};
    m_manifolds = nullptr;
    m_events = nullptr;
    Clear();
    m_initialized = 0U;
    m_stepCount = 0ULL;
    m_totalResolvedContacts = 0ULL;
    m_totalSpeculativeHits = 0ULL;
}

void PhysicsSystem::Clear() noexcept {
    m_spatial.Clear();
    m_activeManifoldCount = 0U;
    m_prevManifoldCount = 0U;
    m_eventCount = 0U;
    m_manifoldFlip = 0U;
}

Result<void, PhysicsErrc> PhysicsSystem::BindCollisionMapBlob(
    const AssetBank& bank,
    BlobHandle collisionBlob) noexcept {
    const BlobRecord& rec = bank.GetBlob(collisionBlob);
    return rec.IsValid() ? BindCollisionMapBytes(rec.Bytes()) : Err(PhysicsErrc::CorruptedCollisionBlob);
}

void PhysicsSystem::Step(World& world, const SimStepView& step) noexcept {
    if (!m_initialized || !step.valid) {
        return;
    }

    m_eventCount = 0U;
    const f32 dt = step.Dt(TimeChannel::World);

    if (m_spatial.ActiveBoxes().empty() &&
        (world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) != 0ULL &&
        world.GetPool<Collider2D>().Size() > 0U) {
        m_spatial.RebuildDynamicGrid(world, dt);
    }

    (void)DynamicsSolver2D::StepKinematicBodiesAndRiders(world, m_spatial, step);
    DynamicsSolver2D::StepCharacterMotors(world, m_spatial, step, m_cfg);

    m_spatial.RebuildDynamicGrid(world, dt);

    const u8 prevFlip = m_manifoldFlip;
    const u8 nextFlip = static_cast<u8>(1U - prevFlip);
    const std::span<const ContactManifold2D> prevSpan{
        m_manifolds + static_cast<usize>(prevFlip) * m_cfg.maxManifolds,
        m_prevManifoldCount};
    std::span<ContactManifold2D> nextSpan{
        m_manifolds + static_cast<usize>(nextFlip) * m_cfg.maxManifolds,
        m_cfg.maxManifolds};
    std::span<ContactEvent2D> eventSpan{m_events, m_cfg.maxContactEvents};

    m_activeManifoldCount = DynamicsSolver2D::GatherManifolds(
        world, m_spatial, step, m_cfg, prevSpan, nextSpan, eventSpan, m_eventCount, m_totalSpeculativeHits);
    m_manifoldFlip = nextFlip;
    m_prevManifoldCount = m_activeManifoldCount;
    m_totalResolvedContacts += m_activeManifoldCount;

    DynamicsSolver2D::StepRigidBodiesSoftTgs(
        world, step, m_cfg, nextSpan.subspan(0U, m_activeManifoldCount), eventSpan, m_eventCount);

    ++m_stepCount;
}

u64 PhysicsSystem::ComputeAuditHash(const World& world) const noexcept {
    u64 h = 14695981039346656037ULL;
    const auto mixBytes = [&](const void* data, usize len) noexcept {
        const auto* bytes = static_cast<const u8*>(data);
        for (usize i = 0U; i < len; ++i) {
            h ^= static_cast<u64>(bytes[i]);
            h *= 1099511628211ULL;
        }
    };

    mixBytes(&m_stepCount, sizeof(m_stepCount));
    mixBytes(&m_activeManifoldCount, sizeof(m_activeManifoldCount));
    mixBytes(&m_totalResolvedContacts, sizeof(m_totalResolvedContacts));
    mixBytes(&m_totalSpeculativeHits, sizeof(m_totalSpeculativeHits));

    const auto manifolds = ActiveManifolds();
    if (!manifolds.empty()) {
        mixBytes(manifolds.data(), manifolds.size_bytes());
    }

    const u64 regMask = world.RegisteredPoolsMask();
    const auto mixPool = [&]<typename T>() noexcept {
        if ((regMask & (1ULL << T::kTypeId)) != 0ULL) {
            const auto& pool = world.GetPool<T>();
            mixBytes(pool.Components().data(), pool.Components().size_bytes());
            for (const EntityId id : pool.Entities()) {
                const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
                mixBytes(&xf.x, sizeof(f32) * 2U);
                mixBytes(&xf.worldRotRad, sizeof(f32));
            }
        }
    };

    mixPool.template operator()<RigidBody2D>();
    mixPool.template operator()<CharacterMotor2D>();
    return h;
}

} // namespace en
