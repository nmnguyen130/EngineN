#include "physics/PhysicsSystem.h"
#include "assets/AssetBank.h"

namespace en {

PhysicsSystem::PhysicsSystem(PhysicsSystem&& other) noexcept
    : m_spatial(other.m_spatial),
      m_cfg(other.m_cfg),
      m_manifolds(other.m_manifolds),
      m_events(other.m_events),
      m_arena(std::move(other.m_arena)),
      m_activeManifoldCount(other.m_activeManifoldCount),
      m_prevManifoldCount(other.m_prevManifoldCount),
      m_eventCount(other.m_eventCount),
      m_manifoldFlip(other.m_manifoldFlip),
      m_initialized(other.m_initialized),
      m_stepCount(other.m_stepCount),
      m_totalResolvedContacts(other.m_totalResolvedContacts),
      m_totalSpeculativeHits(other.m_totalSpeculativeHits) {
    other.m_manifolds = nullptr;
    other.m_events = nullptr;
    other.m_activeManifoldCount = 0U;
    other.m_prevManifoldCount = 0U;
    other.m_eventCount = 0U;
    other.m_initialized = 0U;
}

PhysicsSystem& PhysicsSystem::operator=(PhysicsSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_spatial = other.m_spatial;
        m_cfg = other.m_cfg;
        m_manifolds = other.m_manifolds;
        m_events = other.m_events;
        m_arena = std::move(other.m_arena);
        m_activeManifoldCount = other.m_activeManifoldCount;
        m_prevManifoldCount = other.m_prevManifoldCount;
        m_eventCount = other.m_eventCount;
        m_manifoldFlip = other.m_manifoldFlip;
        m_initialized = other.m_initialized;
        m_stepCount = other.m_stepCount;
        m_totalResolvedContacts = other.m_totalResolvedContacts;
        m_totalSpeculativeHits = other.m_totalSpeculativeHits;

        other.m_manifolds = nullptr;
        other.m_events = nullptr;
        other.m_activeManifoldCount = 0U;
        other.m_prevManifoldCount = 0U;
        other.m_eventCount = 0U;
        other.m_initialized = 0U;
    }
    return *this;
}

Result<void, PhysicsErrc> PhysicsSystem::Init(World& world, const PhysicsConfig& cfg) noexcept {
    Shutdown();
    if (cfg.maxManifolds == 0U || cfg.maxContactEvents == 0U ||
        cfg.maxSpatialProxies == 0U || cfg.arenaBytes < 65536U) {
        return Err(PhysicsErrc::InvalidConfig);
    }

    if (!world.RegisterComponentPool<Collider2D>().has_value() ||
        !world.RegisterComponentPool<RigidBody2D>().has_value() ||
        !world.RegisterComponentPool<CharacterMotor2D>().has_value() ||
        !world.RegisterComponentPool<DistanceJoint2D>(256U).has_value()) {
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
    m_activeManifoldCount = 0U;
    m_prevManifoldCount = 0U;
    m_eventCount = 0U;
    m_manifoldFlip = 0U;
    m_initialized = 1U;
    m_stepCount = 0ULL;
    m_totalResolvedContacts = 0ULL;
    m_totalSpeculativeHits = 0ULL;
    return {};
}

void PhysicsSystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_spatial = SpatialIndex2D{};
    m_manifolds = nullptr;
    m_events = nullptr;
    m_activeManifoldCount = 0U;
    m_prevManifoldCount = 0U;
    m_eventCount = 0U;
    m_manifoldFlip = 0U;
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
    if (!rec.IsValid()) {
        return Err(PhysicsErrc::CorruptedCollisionBlob);
    }
    return BindCollisionMapBytes(rec.Bytes());
}

void PhysicsSystem::Step(World& world, const SimStepView& step) noexcept {
    if (!m_initialized || !step.valid) return;

    m_eventCount = 0U;
    const f32 dt = step.Dt(TimeChannel::World);

    // Stage 1: Rebuild spatial grid and step kinematic bodies + latched riders
    m_spatial.RebuildDynamicGrid(world, dt);
    DynamicsSolver2D::StepKinematicBodiesAndRiders(world, m_spatial, step);

    // Stage 2: Refresh spatial grid with updated kinematic positions and step CharacterMotor2D (MoveAndSlide)
    m_spatial.RebuildDynamicGrid(world, dt);
    DynamicsSolver2D::StepCharacterMotors(world, m_spatial, step, m_cfg);

    // Stage 3: Gather narrowphase + speculative contact manifolds via SpatialIndex2D cell buckets
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

    // Stage 4: Step RigidBody2D and DistanceJoint2D Soft-Step TGS rotational & linear constraints
    DynamicsSolver2D::StepRigidBodiesAndJointsSoftTgs(
        world, step, m_cfg, nextSpan.subspan(0U, m_activeManifoldCount), eventSpan, m_eventCount);

    // Stage 5: Final spatial grid refresh so post-step queries see final positions
    m_spatial.RebuildDynamicGrid(world, 0.0f);
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

    if ((world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) != 0ULL) {
        const auto& pool = world.GetPool<RigidBody2D>();
        mixBytes(pool.Components().data(), pool.Components().size_bytes());
    }
    if ((world.RegisteredPoolsMask() & (1ULL << CharacterMotor2D::kTypeId)) != 0ULL) {
        const auto& pool = world.GetPool<CharacterMotor2D>();
        mixBytes(pool.Components().data(), pool.Components().size_bytes());
    }
    if ((world.RegisteredPoolsMask() & (1ULL << DistanceJoint2D::kTypeId)) != 0ULL) {
        const auto& pool = world.GetPool<DistanceJoint2D>();
        mixBytes(pool.Components().data(), pool.Components().size_bytes());
    }
    return h;
}

} // namespace en
