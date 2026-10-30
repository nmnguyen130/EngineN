#include "gameplay/GameplayPipeline.h"

namespace en {

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
    return {};
}

void GameplayPipeline::Shutdown() noexcept {
    m_arena.Shutdown();
    m_cues = nullptr;
    m_hitEvents = nullptr;
    Clear();
    m_tickCount = 0ULL;
    m_totalCombatHits = 0U;
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

    StateAndCombatSolver2D::Step(world, assetBank, step, input, actions, m_policies, *this);
    MotionSolver2D::Blend(world, step, m_cfg);
    physics.Step(world, step);
    MotionSolver2D::Separate(world, physics, step, m_cfg);
    MotionSolver2D::Sync(world, physics, step);

    std::span<CombatHitEvent> hitSpan{m_hitEvents, m_cfg.maxHitEventsPerTick};
    m_hitEventCount = StateAndCombatSolver2D::ResolveHits(world, step, m_policies, *this, hitSpan);
    m_totalCombatHits += m_hitEventCount;

    CameraDirector2D::Step(world, step, m_cfg, m_tickCount);
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
    const auto mixPool = [&]<typename T>() noexcept {
        if ((regMask & (1ULL << T::kTypeId)) != 0ULL) {
            const auto& p = world.GetPool<T>();
            mixBytes(p.Components().data(), p.Components().size_bytes());
        }
    };

    mixPool.template operator()<MotionStack2D>();
    mixPool.template operator()<StateMachine2D>();
    mixPool.template operator()<HitboxRig2D>();
    mixPool.template operator()<DamageReceiver2D>();
    mixPool.template operator()<SafeAnchor2D>();
    mixPool.template operator()<CameraController2D>();
    return h;
}

} // namespace en
