#include "gameplay/GameplayPipeline.h"

namespace en {

Result<void, WorldErrc> GameplayPipeline::Init(
    World& world, const GameplayPolicyTable& policies, const GameplayPipelineConfig& cfg) noexcept {
    Shutdown();
    if (!m_arena.Init(cfg.arenaBytes)) return Err(WorldErrc::OutOfMemory);

    m_hitScratch = m_arena.Carve<ResolvedHitEvent>(kMaxHitScratch);
    m_cueBuffer = m_arena.Carve<SimCue>(kMaxCues);
    if (m_hitScratch.empty() || m_cueBuffer.empty() ||
        !m_spatial.Init(m_arena, cfg.maxMapWidth, cfg.maxMapHeight) ||
        !m_particles.Init(m_arena, cfg.maxParticles)) {
        Shutdown();
        return Err(WorldErrc::OutOfMemory);
    }

    m_policies = policies;
    m_kinematicCfg = cfg.kinematics;
    m_cueCount = 0;
    ClearWorldFlags();

    (void)world.RegisterComponentPool<VerletRope2D>(128);
    (void)world.RegisterComponentPool<DistanceJoint2D>(128);
    (void)world.RegisterComponentPool<TriggerVolume2D>(256);
    (void)world.RegisterComponentPool<KinematicMotor2D>();
    (void)world.RegisterComponentPool<KinematicCarrier2D>(256);
    (void)world.RegisterComponentPool<MotionStack2D>();
    (void)world.RegisterComponentPool<StateMachine2D>();
    (void)world.RegisterComponentPool<HitboxRig2D>();
    (void)world.RegisterComponentPool<DamageReceiver2D>();
    (void)world.RegisterComponentPool<SafeGroundBreadcrumb2D>(256);
    (void)world.RegisterComponentPool<CameraController2D>(32);
    return {};
}

void GameplayPipeline::Shutdown() noexcept {
    m_arena.Shutdown();
    m_cueCount = 0;
    ClearWorldFlags();
}

Result<void, WorldErrc> GameplayPipeline::SetActiveRoomCollision(
    const AssetBank& bank, BlobHandle collisionMapBlob) noexcept {
    return m_spatial.BindCollisionMapBlob(bank.GetBlob(collisionMapBlob));
}

void GameplayPipeline::Tick(
    World& world, const AssetBank& assetBank, const SimStepView& step,
    const TickInput& tickInput, ActionBuffer& actionBuffer) noexcept {
    CombatAndStateSolver::DispatchAnimationFrameDirectives(world, assetBank, step, *this);

    m_spatial.RebuildDynamicGrid(world, SpatialRebuildMode::KinematicsOnly);
    KinematicSolver2D::StepCarriersAndRiders(world, m_spatial, step, m_kinematicCfg);

    CombatAndStateSolver::StepStateMachinesAndPolicies(
        world, assetBank, step, tickInput, actionBuffer, m_policies, *this);

    m_spatial.RebuildDynamicGrid(world, SpatialRebuildMode::KinematicsOnly);
    KinematicSolver2D::StepMotorsAndMotionStacks(world, m_spatial, step, m_kinematicCfg);
    KinematicSolver2D::StepConstraintsAndVerlet(world, m_spatial, step, m_kinematicCfg);

    (void)CombatAndStateSolver::GatherAndResolveCombatHits(
        world, step, m_hitScratch, m_policies, *this);

    CameraAndBreadcrumbSolver::StepSafeBreadcrumbs(world, m_spatial, step);
    if (!step.IsFrozen(TimeChannel::Vfx)) {
        m_particles.Step(step.Dt(TimeChannel::Vfx), m_kinematicCfg.gravity);
    }
    CameraAndBreadcrumbSolver::StepCameraControllers(world, step);
}

void GameplayPipeline::LoadRawWorldFlags(std::span<const u64, 64> words) noexcept {
    m_activeFlagWordsMask = 0ULL;
    for (u8 w = 0; w < 64U; ++w) {
        m_worldFlags[w] = words[w];
        if (words[w] != 0ULL) m_activeFlagWordsMask |= (1ULL << w);
    }
}

u64 GameplayPipeline::ComputeAuditHash(const World& world) const noexcept {
    u64 h = Mix64(m_activeFlagWordsMask ^ (static_cast<u64>(m_particles.ActiveCount()) << 32U));
    u64 m = m_activeFlagWordsMask;
    while (m != 0ULL) {
        const u8 w = static_cast<u8>(std::countr_zero(m));
        m &= (m - 1ULL);
        h = HashCombine64(h, m_worldFlags[w]);
    }
    world.Each<KinematicMotor2D>([&](EntityId id, const KinematicMotor2D& motor) noexcept {
        h = HashCombine64(h, static_cast<u64>(id.Packed()) |
                             (static_cast<u64>(motor.contacts) << 32U));
    });
    return h;
}

} // namespace en
