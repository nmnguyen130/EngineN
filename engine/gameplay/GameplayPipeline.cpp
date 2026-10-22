#include "gameplay/GameplayPipeline.h"

namespace en {

GameplayPipeline::GameplayPipeline(GameplayPipeline&& other) noexcept
    : m_cfg(other.m_cfg),
      m_policies(other.m_policies),
      m_cues(other.m_cues),
      m_hitEvents(other.m_hitEvents),
      m_arena(std::move(other.m_arena)),
      m_activeFlagWordsMask(other.m_activeFlagWordsMask),
      m_tickCount(other.m_tickCount),
      m_totalCombatHits(other.m_totalCombatHits),
      m_cueCount(other.m_cueCount),
      m_hitEventCount(other.m_hitEventCount),
      m_worldFlags(other.m_worldFlags) {
    other.m_cues = nullptr;
    other.m_hitEvents = nullptr;
    other.m_activeFlagWordsMask = 0ULL;
    other.m_cueCount = 0U;
    other.m_hitEventCount = 0U;
}

GameplayPipeline& GameplayPipeline::operator=(GameplayPipeline&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_cfg = other.m_cfg;
        m_policies = other.m_policies;
        m_cues = other.m_cues;
        m_hitEvents = other.m_hitEvents;
        m_arena = std::move(other.m_arena);
        m_activeFlagWordsMask = other.m_activeFlagWordsMask;
        m_tickCount = other.m_tickCount;
        m_totalCombatHits = other.m_totalCombatHits;
        m_cueCount = other.m_cueCount;
        m_hitEventCount = other.m_hitEventCount;
        m_worldFlags = other.m_worldFlags;

        other.m_cues = nullptr;
        other.m_hitEvents = nullptr;
        other.m_activeFlagWordsMask = 0ULL;
        other.m_cueCount = 0U;
        other.m_hitEventCount = 0U;
    }
    return *this;
}

Result<void, GameplayErrc> GameplayPipeline::Init(
    World& world,
    const GameplayPolicyTable& policies,
    const GameplayConfig& cfg) noexcept {
    Shutdown();
    if (cfg.maxCuesPerTick == 0U || cfg.maxHitEventsPerTick == 0U || cfg.arenaBytes < 16384U) {
        return Err(GameplayErrc::InvalidConfig);
    }

    if (!world.RegisterComponentPool<MotionStack2D>().has_value() ||
        !world.RegisterComponentPool<StateMachine2D>().has_value() ||
        !world.RegisterComponentPool<HitboxRig2D>().has_value() ||
        !world.RegisterComponentPool<DamageReceiver2D>().has_value() ||
        !world.RegisterComponentPool<SafeAnchor2D>(256U).has_value() ||
        !world.RegisterComponentPool<CameraController2D>(32U).has_value()) {
        return Err(GameplayErrc::PoolRegistrationFailed);
    }

    if (!m_arena.Init(cfg.arenaBytes)) {
        return Err(GameplayErrc::OutOfMemory);
    }

    auto cuesSpan = m_arena.Carve<SimCue>(cfg.maxCuesPerTick);
    auto hitsSpan = m_arena.Carve<CombatHitEvent>(cfg.maxHitEventsPerTick);
    if (cuesSpan.empty() || hitsSpan.empty()) {
        Shutdown();
        return Err(GameplayErrc::OutOfMemory);
    }

    m_cfg = cfg;
    m_policies = policies;
    m_cues = cuesSpan.data();
    m_hitEvents = hitsSpan.data();
    m_activeFlagWordsMask = 0ULL;
    m_tickCount = 0ULL;
    m_totalCombatHits = 0U;
    m_cueCount = 0U;
    m_hitEventCount = 0U;
    m_worldFlags.fill(0ULL);
    return {};
}

void GameplayPipeline::Shutdown() noexcept {
    m_arena.Shutdown();
    m_cues = nullptr;
    m_hitEvents = nullptr;
    m_activeFlagWordsMask = 0ULL;
    m_tickCount = 0ULL;
    m_totalCombatHits = 0U;
    m_cueCount = 0U;
    m_hitEventCount = 0U;
    m_worldFlags.fill(0ULL);
}

void GameplayPipeline::Clear() noexcept {
    ClearWorldFlags();
    m_cueCount = 0U;
    m_hitEventCount = 0U;
}

void GameplayPipeline::Tick(
    World& world,
    PhysicsSystem& physics,
    const AssetBank& assetBank,
    const SimStepView& step,
    const TickInput& input,
    ActionBuffer& actions) noexcept {
    if (!m_cues || !step.valid) return;

    m_hitEventCount = 0U;

    // Stage 1: Step state machines, timeline frame directives, and client policies
    StateAndCombatSolver2D::StepStateMachinesAndDirectives(
        world, assetBank, step, input, actions, m_policies, *this);

    // Stage 2: Blend MotionStack2D velocities/root-motion/impulses into CharacterMotor2D
    MotionSolver2D::BlendMotionStacksIntoMotors(world, step, m_cfg);

    // Stage 3: Step 2D PhysicsSystem (kinematic platforms, MoveAndSlide characters, Soft-Step TGS rigid bodies & joints)
    physics.Step(world, step);

    // Stage 4: Resolve soft character pushbox separation & refresh post-physics coyote timers and 3-ray safe anchors
    MotionSolver2D::ResolveSoftCharacterPushboxes(world, physics, step, m_cfg);
    MotionSolver2D::SyncPostPhysicsContactsAndTimers(world, physics, step);

    // Stage 5: Resolve Bloom-deduplicated hitbox/hurtbox combat overlaps and clash/parry arbitration
    std::span<CombatHitEvent> hitSpan{m_hitEvents, m_cfg.maxHitEventsPerTick};
    m_hitEventCount = StateAndCombatSolver2D::ResolveCombatOverlaps(
        world, step, m_policies, *this, hitSpan);
    m_totalCombatHits += m_hitEventCount;

    // Stage 6: Step 2nd-order critically damped spring cameras, room confiners, and trauma shake
    CameraDirector2D::StepCameras(world, step, m_cfg, m_tickCount);
    ++m_tickCount;
}

u64 GameplayPipeline::ComputeAuditHash(const World& world) const noexcept {
    u64 h = 14695981039346656037ULL;
    const auto mixBytes = [&](const void* data, usize len) noexcept {
        const auto* bytes = static_cast<const u8*>(data);
        for (usize i = 0U; i < len; ++i) {
            h ^= static_cast<u64>(bytes[i]);
            h *= 1099511628211ULL;
        }
    };

    mixBytes(&m_tickCount, sizeof(m_tickCount));
    mixBytes(&m_totalCombatHits, sizeof(m_totalCombatHits));
    mixBytes(&m_activeFlagWordsMask, sizeof(m_activeFlagWordsMask));

    u64 flagMask = m_activeFlagWordsMask;
    while (flagMask != 0ULL) {
        const u32 w = static_cast<u32>(std::countr_zero(flagMask));
        flagMask &= (flagMask - 1ULL);
        mixBytes(&w, sizeof(w));
        mixBytes(&m_worldFlags[w], sizeof(u64));
    }

    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
        const auto& p = world.GetPool<MotionStack2D>();
        mixBytes(p.Components().data(), p.Components().size_bytes());
    }
    if ((regMask & (1ULL << StateMachine2D::kTypeId)) != 0ULL) {
        const auto& p = world.GetPool<StateMachine2D>();
        mixBytes(p.Components().data(), p.Components().size_bytes());
    }
    if ((regMask & (1ULL << HitboxRig2D::kTypeId)) != 0ULL) {
        const auto& p = world.GetPool<HitboxRig2D>();
        mixBytes(p.Components().data(), p.Components().size_bytes());
    }
    if ((regMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL) {
        const auto& p = world.GetPool<DamageReceiver2D>();
        mixBytes(p.Components().data(), p.Components().size_bytes());
    }
    if ((regMask & (1ULL << SafeAnchor2D::kTypeId)) != 0ULL) {
        const auto& p = world.GetPool<SafeAnchor2D>();
        mixBytes(p.Components().data(), p.Components().size_bytes());
    }
    if ((regMask & (1ULL << CameraController2D::kTypeId)) != 0ULL) {
        const auto& p = world.GetPool<CameraController2D>();
        mixBytes(p.Components().data(), p.Components().size_bytes());
    }
    return h;
}

} // namespace en
