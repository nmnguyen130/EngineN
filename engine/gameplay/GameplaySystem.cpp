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
    auto wmap = m_arena.Alloc<WorldMap2D>(1U);
    if (cues.empty() || hits.empty() || wmap.empty() || !m_mutations.Init(m_arena, kMaxMutations)) {
        Shutdown();
        return Err(GameplayErrc::OutOfMemory);
    }

    m_cfg       = cfg;
    m_callbacks = callbacks;
    m_cues      = cues.data();
    m_hits      = hits.data();
    m_worldMap  = wmap.data();
    m_worldMap->Clear();
    return {};
}

void GameplaySystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_cues      = nullptr;
    m_hits      = nullptr;
    m_worldMap  = nullptr;
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
    const TickStep& step,
    EventBus* bus) noexcept {
    if (!m_cues || !step) return;
    SyncCharactersPostPhysics(world, physics, step);
    ResolveCombat(world, step);
    if (bus) {
        StepRoomPortals(world, physics, *bus);
        PublishCuesToBus(world, *bus);
    }
    StepCameras(world, step, bus);
}

void GameplaySystem::Step(
    World& world,
    PhysicsSystem& physics,
    const TickStep& step,
    const TickInput& input,
    ActionBuffer& actions,
    EventBus* bus) noexcept {
    if (!m_cues || !step) return;
    StepPrePhysics(world, step, input, actions);
    physics.Step(world, step);
    StepPostPhysics(world, physics, step, bus);
}

void GameplaySystem::StepRoomPortals(
    const World& world,
    const PhysicsSystem& physics,
    EventBus& bus) const noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << RoomPortal2D::kTypeId)) == 0ULL) return;
    const auto& portalPool = world.GetPool<RoomPortal2D>();
    if (portalPool.Size() == 0U) return;

    for (const ContactEvent2D& ev : physics.ContactEvents()) {
        if (ev.kind != ContactEventKind::TriggerEnter) continue;
        const EntityId portalId = world.Has<RoomPortal2D>(ev.entityA)
            ? ev.entityA
            : (world.Has<RoomPortal2D>(ev.entityB) ? ev.entityB : kNullEntity);
        if (!portalId.IsValid()) continue;

        const RoomPortal2D& portal = portalPool.DirectAtEntityIndex(portalId.index);
        if (!portal.targetRoom.IsValid()) continue;
        if (portal.requiredFlag < kMaxFlags && !TestWorldFlag(portal.requiredFlag)) {
            continue;
        }

        (void)bus.roomTransitions.Publish(RoomTransitionEvent{
            .targetRoom   = portal.targetRoom,
            .collisionMap = portal.targetCollisionMap,
            .prefab       = portal.targetPrefab,
            .spawnPos     = portal.spawnPos,
            .wipeTicks    = portal.wipeTicks,
            .wipeKind     = static_cast<u8>(portal.wipeKind & 0x03U),
            .clearWorld   = 1U,
        });
        break;
    }
}

void GameplaySystem::PublishCuesToBus(const World& world, EventBus& bus) noexcept {
    for (const GameplayCue& c : DrainCues()) {
        switch (c.kind) {
            case CueKind::Audio: {
                Vec2 pos = c.position;
                if (pos == Vec2{} && c.emitter.IsValid() && world.IsAlive(c.emitter)) {
                    const WorldTransform2D& wt = world.GetWorldByIndex(c.emitter.index);
                    pos = {wt.x, wt.y};
                }
                (void)bus.audio.Publish(AudioTrigger{
                    .cueId     = c.asset,
                    .position  = pos,
                    .emitter   = c.emitter.Packed(),
                    .gainScale = c.magnitude,
                });
                break;
            }
            case CueKind::Effect:
                (void)bus.vfxBursts.Publish(VfxBurstEvent{
                    .origin        = c.position,
                    .magnitude     = c.magnitude,
                    .durationTicks = c.durationTicks,
                    .count         = c.userTag,
                });
                break;
            case CueKind::DashGhost:
                (void)bus.dashGhosts.Publish(DashGhostEvent{
                    .position      = c.position,
                    .emitter       = c.emitter,
                    .durationTicks = c.durationTicks,
                    .flags         = c.userTag,
                });
                break;
            case CueKind::CameraShake:
                (void)bus.cameraTrauma.Publish(CameraTraumaEvent{
                    .origin = c.position,
                    .trauma = c.magnitude,
                });
                break;
            case CueKind::Hitstop:
                (void)bus.hitstops.Publish(HitstopEvent{
                    .durationTicks = c.durationTicks,
                    .channel       = c.channel,
                });
                break;
            case CueKind::Rumble:
                (void)bus.rumbles.Publish(RumbleEvent{
                    .intensity     = c.magnitude,
                    .durationTicks = c.durationTicks,
                });
                break;
            default:
                break;
        }
    }
}

u64 GameplaySystem::ComputeAuditHash(const World& world) const noexcept {
    u64 h = HashCombine(0xCBF29CE484222325ULL, m_tickCount);
    h     = HashCombine(h, m_totalHits);
    for (const u64 w : m_worldFlags) {
        if (w != 0ULL) h = HashCombine(h, w);
    }
    if (m_worldMap && m_worldMap->discoveredCount > 0U) {
        h = HashCombine(h, Crc32({reinterpret_cast<const u8*>(m_worldMap), sizeof(WorldMap2D)}));
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
