#include "gameplay/GameplaySystem.h"

namespace en {

Result<void, GameplayErrc> GameplaySystem::Init(
    World& world,
    const GameplayConfig& cfg,
    const GameplayCallbacks& callbacks) noexcept {
    Shutdown();
    if (cfg.maxCuesPerTick == 0U || cfg.maxCombatHitsPerTick == 0U || cfg.arenaBytes < 8192U) {
        return Err(GameplayErrc::InvalidConfig);
    }

    if (!world.RegisterComponentPool<CharacterController2D>().has_value() ||
        !world.RegisterComponentPool<StateMachine2D>().has_value() ||
        !world.RegisterComponentPool<Health2D>().has_value() ||
        !world.RegisterComponentPool<CombatBox2D>().has_value() ||
        !world.RegisterComponentPool<SafeAnchor2D>(256U).has_value() ||
        !world.RegisterComponentPool<CameraFollow2D>(32U).has_value() ||
        !world.RegisterComponentPool<ActionTimeline2D>(256U).has_value() ||
        !world.RegisterComponentPool<WorldMutation2D>(512U).has_value() ||
        !world.RegisterComponentPool<RoomPortal2D>(64U).has_value()) {
        return Err(GameplayErrc::PoolRegistrationFailed);
    }

    if (!m_arena.Init(cfg.arenaBytes)) {
        return Err(GameplayErrc::OutOfMemory);
    }

    auto cues = m_arena.Alloc<GameplayCue>(cfg.maxCuesPerTick);
    auto hits = m_arena.Alloc<CombatHitEvent>(cfg.maxCombatHitsPerTick);
    if (cues.empty() || hits.empty() || !m_mutations.Init(m_arena, kMaxMutations)) {
        Shutdown();
        return Err(GameplayErrc::OutOfMemory);
    }

    m_cfg       = cfg;
    m_callbacks = callbacks;
    m_cues      = cues.data();
    m_hits      = hits.data();
    return {};
}

void GameplaySystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_cues      = nullptr;
    m_hits      = nullptr;
    m_mutations = {};
    m_tickCount = 0ULL;
    m_totalHits = 0U;
    ClearWorldFlags();
    m_cueCount  = 0U;
    m_hitCount  = 0U;
}

void GameplaySystem::Clear() noexcept {
    m_cueCount = 0U;
    m_hitCount = 0U;
}

void GameplaySystem::StepPrePhysics(
    World& world,
    const TickStep& step,
    const TickInput& input,
    ActionBuffer& actions) noexcept {
    if (!m_cues || !step) return;
    BeginTick(input, actions, step.IsFrozen(TimeChannel::Player));
    StepStatesAndHealth(world, step, input, actions);
    StepCharactersPrePhysics(world, step);
}

void GameplaySystem::StepPostPhysics(
    World& world,
    const PhysicsSystem& physics,
    const TickStep& step) noexcept {
    if (!m_cues || !step) return;
    SyncCharactersPostPhysics(world, physics, step);
    ResolveCombat(world, step);
    StepCameras(world, step);
}

void GameplaySystem::Step(
    World& world,
    PhysicsSystem& physics,
    const TickStep& step,
    const TickInput& input,
    ActionBuffer& actions) noexcept {
    if (!m_cues || !step) return;
    StepPrePhysics(world, step, input, actions);
    physics.Step(world, step);
    StepPostPhysics(world, physics, step);
}

u64 GameplaySystem::ComputeAuditHash(const World& world) const noexcept {
    u64 h = HashCombine(0xCBF29CE484222325ULL, m_tickCount);
    h     = HashCombine(h, m_totalHits);
    for (const u64 w : m_worldFlags) {
        if (w != 0ULL) h = HashCombine(h, w);
    }

    u64 mutXor = 0ULL;
    for (const auto& s : m_mutations.Slots()) {
        if (s.dist != 0U && s.key != 0ULL) {
            mutXor ^= HashCombine(s.key, s.value);
        }
    }
    if (mutXor != 0ULL) h = HashCombine(h, mutXor);

    const u64 reg = world.RegisteredPoolsMask();
    const auto mixPool = [&]<Component T>() noexcept {
        if ((reg & (1ULL << T::kTypeId)) != 0ULL) {
            const auto bytes = std::as_bytes(world.GetPool<T>().Components());
            h = HashCombine(h, Crc32({reinterpret_cast<const u8*>(bytes.data()), bytes.size()}));
        }
    };

    mixPool.template operator()<CharacterController2D>();
    mixPool.template operator()<StateMachine2D>();
    mixPool.template operator()<Health2D>();
    mixPool.template operator()<CombatBox2D>();
    mixPool.template operator()<SafeAnchor2D>();
    mixPool.template operator()<CameraFollow2D>();
    mixPool.template operator()<ActionTimeline2D>();
    mixPool.template operator()<WorldMutation2D>();
    mixPool.template operator()<RoomPortal2D>();
    return h;
}

} // namespace en
