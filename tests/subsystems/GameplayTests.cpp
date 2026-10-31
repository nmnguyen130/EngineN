#include "tests/subsystems/CtsHarness.h"

#include "gameplay/GameplaySystem.h"
#include "vfx/VfxSystem.h"
#include "time/TimeSystem.h"
#include <vector>

namespace {

using namespace en;
using namespace en::cts;

bool CtsCharacterLocomotionCoyoteWallJumpAndDash(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U}).has_value());

    PhysicsSystem phys;
    CTS_CHECK(r, phys.Init(world).has_value());

    GameplaySystem game;
    CTS_CHECK(r, game.Init(world).has_value());

    std::array<TileFlag, 12U * 12U> tiles{};
    tiles.fill(TileFlag::Empty);
    for (u32 x = 0U; x < 12U; ++x) tiles[x] = TileFlag::Solid;
    for (u32 y = 0U; y < 12U; ++y) tiles[y * 12U + 8U] = TileFlag::Solid;
    CTS_CHECK(r, phys.BuildTileMap(12U, 12U, 1.0f, {0.0f, 0.0f}, tiles).has_value());

    const EntityId hero = *world.Spawn({.local = {.x = 2.0f, .y = 1.5f}});
    (void)world.Emplace<Collider2D>(hero, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 1U});
    (void)world.Emplace<PhysicsBody2D>(hero, PhysicsBody2D{.type = BodyType::Dynamic});
    (void)world.Emplace<CharacterController2D>(hero, CharacterController2D{.maxRunSpeed = 8.0f, .jumpSpeed = 14.0f});
    (void)world.Emplace<SafeAnchor2D>(hero);
    (void)world.Emplace<SquashStretch2D>(hero);

    TimeSystem   time;
    TickInput    in{};
    ActionBuffer actions{};
    VfxSystem    vfx;
    EventBus     bus{};
    CTS_CHECK(r, vfx.Init().has_value());

    auto stepTicks = [&](u32 n) noexcept {
        for (u32 i = 0U; i < n; ++i) {
            time.BeginFrame(kFixedDtNs);
            const TickStep step = time.PollTick();
            world.BeginTick(step);
            game.Step(world, phys, step, in, actions, &bus);
            vfx.ConsumeEvents(bus, world);
            vfx.Step(step);
            world.EndTick(step);
        }
    };

    world.TryGet<CharacterController2D>(hero)->moveInputX = 1.0f;
    stepTicks(20U);
    CTS_CHECK(r, world.TryGet<PhysicsBody2D>(hero)->IsGrounded());
    CTS_CHECK(r, world.TryGet<CharacterController2D>(hero)->coyoteTicks == game.Config().coyoteWindowTicks);
    CTS_CHECK(r, world.TryGet<SafeAnchor2D>(hero)->safePos.x > 2.0f);

    world.TryGet<CharacterController2D>(hero)->RequestJump(true);
    stepTicks(1U);
    // Procedural jump stretch: scaleX < 1.0f and scaleY > 1.0f, plus jump dust particles emitted via EventBus!
    CTS_CHECK(r, world.GetLocalTransform(hero).scaleX < 0.95f && world.GetLocalTransform(hero).scaleY > 1.05f);
    CTS_CHECK(r, vfx.ActiveParticleCount() > 0U);
    stepTicks(1U);
    const f32 vyBeforeCut = world.TryGet<PhysicsBody2D>(hero)->velocity.y;
    CTS_CHECK(r, vyBeforeCut > 10.0f);

    // Verify Apex Hang Time: when airborne with JumpHeld and |vy| <= apexThreshold, gravityScale drops to 0.5f.
    world.TryGet<PhysicsBody2D>(hero)->velocity.y = 1.0f;
    stepTicks(1U);
    CTS_CHECK(r, Near(world.TryGet<PhysicsBody2D>(hero)->gravityScale, 0.5f, 1e-3f));
    world.TryGet<PhysicsBody2D>(hero)->velocity.y = vyBeforeCut;
    world.TryGet<CharacterController2D>(hero)->SetJumpHeld(false);
    stepTicks(1U);
    const f32 vyAfterCut = world.TryGet<PhysicsBody2D>(hero)->velocity.y;
    CTS_CHECK(r, vyAfterCut < vyBeforeCut * 0.6f);

    world.TryGet<CharacterController2D>(hero)->RequestDash();
    stepTicks(3U);
    // Verify Dash Ghost Trail (After-Image) emitted during dash via EventBus!
    CTS_CHECK(r, vfx.ActiveGhostCount() > 0U);
    stepTicks(12U);
    world.TryGet<CharacterController2D>(hero)->moveInputX = 1.0f;
    stepTicks(40U);
    CTS_CHECK(r, world.TryGet<CharacterController2D>(hero)->wallCoyoteTicks > 0U);
    CTS_CHECK(r, world.TryGet<CharacterController2D>(hero)->wallDir == 1);

    world.TryGet<CharacterController2D>(hero)->RequestJump(true);
    stepTicks(65U);
    CTS_CHECK(r, !world.TryGet<PhysicsBody2D>(hero)->IsGrounded());
    CTS_CHECK(r, world.TryGet<CharacterController2D>(hero)->IsWallSliding());
    CTS_CHECK(r, world.TryGet<PhysicsBody2D>(hero)->velocity.y >= -world.TryGet<CharacterController2D>(hero)->wallSlideSpeed - 1e-3f);

    world.TryGet<CharacterController2D>(hero)->moveInputX = 0.0f;
    world.TryGet<CharacterController2D>(hero)->RequestJump(true);
    stepTicks(1U);
    CTS_CHECK(r, world.TryGet<CharacterController2D>(hero)->facing == -1);
    CTS_CHECK(r, world.TryGet<PhysicsBody2D>(hero)->velocity.x < -5.0f);
    CTS_CHECK(r, world.TryGet<PhysicsBody2D>(hero)->velocity.y > 10.0f);
    return true;
}

bool CtsCombatHitboxesIframesParryAndClash(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U}).has_value());

    PhysicsSystem phys;
    CTS_CHECK(r, phys.Init(world).has_value());

    GameplaySystem game;
    CTS_CHECK(r, game.Init(world).has_value());

    const EntityId hero  = *world.Spawn({.local = {.x = 0.0f, .y = 0.0f}});
    const EntityId enemy = *world.Spawn({.local = {.x = 0.8f, .y = 0.0f}});

    (void)world.Emplace<CharacterController2D>(hero, CharacterController2D{.facing = 1});
    (void)world.Emplace<StateMachine2D>(hero);
    (void)world.Emplace<Health2D>(hero, Health2D{.hp = 100, .maxHp = 100, .team = 1U});
    (void)world.Emplace<PhysicsBody2D>(hero, PhysicsBody2D{.type = BodyType::Dynamic});
    (void)world.Emplace<CombatBox2D>(hero, CombatBox2D{.flags = CombatFlags::HurtboxActive | CombatFlags::FlipWithFacing | CombatFlags::CanClash});

    (void)world.Emplace<CharacterController2D>(enemy, CharacterController2D{.facing = -1});
    (void)world.Emplace<StateMachine2D>(enemy);
    (void)world.Emplace<Health2D>(enemy, Health2D{.hp = 50, .maxHp = 50, .autoIframeTicks = 10U, .poise = 15U, .maxPoise = 15U, .team = 2U});
    (void)world.Emplace<CombatBox2D>(enemy, CombatBox2D{.flags = CombatFlags::HurtboxActive | CombatFlags::FlipWithFacing | CombatFlags::CanClash});
    (void)world.Emplace<HitFlash2D>(enemy);

    TimeSystem   time;
    TickInput    in{};
    ActionBuffer actions{};

    auto stepOne = [&]() noexcept {
        time.BeginFrame(kFixedDtNs);
        const TickStep step = time.PollTick();
        world.BeginTick(step);
        game.Step(world, phys, step, in, actions);
        world.EndTick(step);
    };

    world.TryGet<CombatBox2D>(hero)->BeginSwing({0.2f, -0.5f, 1.0f, 1.0f}, 20, {8.0f, 3.0f}, 4U, 20U);
    stepOne();
    CTS_CHECK(r, game.CombatHits().size() == 1U);
    CTS_CHECK(r, game.CombatHits()[0].outcome == HitOutcome::Damaged);
    CTS_CHECK(r, game.CombatHits()[0].damage == 20 && game.CombatHits()[0].poiseBroken == 1U);
    CTS_CHECK(r, world.TryGet<Health2D>(enemy)->hp == 30);
    CTS_CHECK(r, world.TryGet<Health2D>(enemy)->iframeTicks == 10U);
    CTS_CHECK(r, world.TryGet<HitFlash2D>(enemy)->remainingTicks == 4U && !game.Cues().empty());
    CTS_CHECK(r, world.TryGet<StateMachine2D>(hero)->hitstopTicks == 4U);

    stepOne();
    CTS_CHECK(r, game.CombatHits().empty());
    CTS_CHECK(r, world.TryGet<Health2D>(enemy)->hp == 30);
    world.TryGet<CombatBox2D>(hero)->EndSwing();

    for (u32 i = 0U; i < 15U; ++i) stepOne();
    world.TryGet<Health2D>(enemy)->flags |= HealthFlags::Parrying;
    world.TryGet<CombatBox2D>(hero)->BeginSwing({0.2f, -0.5f, 1.0f, 1.0f}, 20);
    stepOne();
    CTS_CHECK(r, game.CombatHits().size() == 1U && game.CombatHits()[0].outcome == HitOutcome::Parried);
    CTS_CHECK(r, world.TryGet<Health2D>(enemy)->hp == 30);
    world.TryGet<Health2D>(enemy)->flags &= ~HealthFlags::Parrying;
    world.TryGet<CombatBox2D>(hero)->EndSwing();

    for (u32 i = 0U; i < 6U; ++i) stepOne();
    world.TryGet<CombatBox2D>(hero)->BeginSwing({0.1f, -0.5f, 1.0f, 1.0f}, 20);
    world.TryGet<CombatBox2D>(enemy)->BeginSwing({0.1f, -0.5f, 1.0f, 1.0f}, 15);
    stepOne();
    CTS_CHECK(r, game.CombatHits().size() == 1U && game.CombatHits()[0].outcome == HitOutcome::Clashed);
    CTS_CHECK(r, !HasFlag(world.TryGet<CombatBox2D>(hero)->flags, CombatFlags::HitboxActive));
    CTS_CHECK(r, !HasFlag(world.TryGet<CombatBox2D>(enemy)->flags, CombatFlags::HitboxActive));

    for (u32 i = 0U; i < 6U; ++i) stepOne();
    world.TryGet<CharacterController2D>(hero)->airDashesLeft = 0U;
    world.TryGet<CombatBox2D>(hero)->flags |= CombatFlags::PogoStrike;
    world.TryGet<CombatBox2D>(hero)->hitstopTicks = 0U;
    (void)world.Emplace<ActionTimeline2D>(hero, ActionTimeline2D{
        .hitbox          = {0.1f, -0.5f, 1.0f, 1.0f},
        .damage          = 10,
        .startupTicks    = 2U,
        .activeTicks     = 2U,
        .recoveryTicks   = 2U,
        .cancelStartTick = 4U,
        .cancelMask      = 0x05U,
        .active          = 1U,
    });
    world.TryGet<StateMachine2D>(hero)->TransitionTo(5U, 6U, 0U, 0x00U);

    stepOne(); // tick 0 (startup)
    CTS_CHECK(r, !HasFlag(world.TryGet<CombatBox2D>(hero)->flags, CombatFlags::HitboxActive));
    stepOne(); // tick 1 (startup)
    CTS_CHECK(r, !HasFlag(world.TryGet<CombatBox2D>(hero)->flags, CombatFlags::HitboxActive));
    stepOne(); // tick 2 (active -> hits enemy, triggers PogoStrike!)
    CTS_CHECK(r, game.CombatHits().size() == 1U && world.TryGet<Health2D>(enemy)->hp == 20);
    stepOne(); // tick 3 (pre-physics applies PogoRequested -> refills airDashesLeft!)
    CTS_CHECK(r, world.TryGet<CharacterController2D>(hero)->airDashesLeft == 1U);
    stepOne(); // tick 4 (recovery + cancel window open)
    CTS_CHECK(r, !HasFlag(world.TryGet<CombatBox2D>(hero)->flags, CombatFlags::HitboxActive));
    CTS_CHECK(r, world.TryGet<StateMachine2D>(hero)->CanCancel(0x01U));
    return true;
}

bool CtsCameraFollowRoomBoundsWorldMapAndFlags(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 64U, .defaultPoolCapacity = 64U}).has_value());

    PhysicsSystem phys;
    CTS_CHECK(r, phys.Init(world).has_value());

    GameplaySystem game;
    CTS_CHECK(r, game.Init(world).has_value());

    game.SetWorldFlag(0U, true);
    game.SetWorldFlag(4095U, true);
    CTS_CHECK(r, game.TestWorldFlag(0U) && game.TestWorldFlag(4095U) && !game.TestWorldFlag(2048U));
    game.SetWorldFlag(0U, false);
    CTS_CHECK(r, !game.TestWorldFlag(0U));

    // WorldMap2D Fog-of-War & Markers + WorldMutation2D SpatialGuid persistence save/load round-trip.
    game.WorldMap().RevealRadius(5, 5, 1, true);
    game.WorldMap().SetCellFlags(6, 5, MapCellFlags::LockedDoor | MapCellFlags::SaveRoom);
    CTS_CHECK(r, game.WorldMap().PlaceMarker(5, 5, 3U, 1U, 99U));
    CTS_CHECK(r, game.WorldMap().IsVisited(5, 5) && game.WorldMap().IsDiscovered(6, 5));
    CTS_CHECK(r, game.WorldMap().discoveredCount == 5U);

    const u64 bossGuid = WorldMutation2D::ComputeSpatialGuid(3U, 12, -4, 99U);
    const EntityId boss = *world.Spawn({.local = {.x = 12.0f, .y = -4.0f}});
    (void)world.Emplace<WorldMutation2D>(boss, WorldMutation2D{
        .guid          = bossGuid,
        .flagId        = 42U,
        .flags         = MutationFlags::DestroyIfMutated,
        .recordOnDeath = 1U,
    });
    CTS_CHECK(r, game.RecordMutation(bossGuid, 1U));
    game.SetWorldFlag(42U, true);

    std::vector<u8> saveBlob(game.RequiredSaveBytes());
    const auto written = game.SerializePersistence(saveBlob);
    CTS_CHECK(r, written.has_value() && *written == saveBlob.size());

    game.ClearWorldFlags();
    game.ClearWorldMap();
    game.ClearMutations();
    CTS_CHECK(r, !game.HasMutation(bossGuid) && !game.TestWorldFlag(42U) && game.WorldMap().discoveredCount == 0U);
    CTS_CHECK(r, game.DeserializePersistence(saveBlob).has_value());
    CTS_CHECK(r, game.HasMutation(bossGuid) && game.TestWorldFlag(42U));
    CTS_CHECK(r, game.WorldMap().discoveredCount == 5U && game.WorldMap().IsVisited(5, 5));
    CTS_CHECK(r, HasFlag(game.WorldMap().GetCell(6, 5), MapCellFlags::LockedDoor | MapCellFlags::SaveRoom));
    CTS_CHECK(r, game.WorldMap().Markers().size() == 1U && game.WorldMap().Markers()[0].userTag == 99U);
    CTS_CHECK(r, game.ApplyRoomMutations(world) == 1U && !world.IsAlive(boss));

    const EntityId hero = *world.Spawn({.local = {.x = 2.0f, .y = 5.0f}});
    (void)world.Emplace<CharacterController2D>(hero, CharacterController2D{.facing = 1});

    const EntityId camEnt = *world.Spawn({.local = {.x = 0.0f, .y = 0.0f}});
    (void)world.Emplace<CameraRig2D>(camEnt, CameraRig2D{.zoom = 1.0f, .pixelsPerMeter = 64.0f});
    (void)world.Emplace<CameraFollow2D>(camEnt, CameraFollow2D{
        .roomBounds = {0.0f, 0.0f, 40.0f, 20.0f},
        .trauma     = 0.5f,
        .target     = hero,
    });

    TimeSystem   time;
    TickInput    in{};
    ActionBuffer actions{};
    time.BeginFrame(kFixedDtNs);
    const TickStep step = time.PollTick();
    world.BeginTick(step);
    game.Step(world, phys, step, in, actions);
    world.EndTick(step);

    const WorldTransform2D camWt = world.GetWorldTransform(camEnt);
    CTS_CHECK(r, Near(camWt.x, 10.0f, 1e-3f) && Near(camWt.y, 5.625f, 1e-3f));
    CTS_CHECK(r, world.TryGet<CameraRig2D>(camEnt)->trauma > 0.0f);
    return true;
}

bool DstGameplayStateMachineAndLockstepStress(CtsRunner& r) noexcept {
    auto runScenario = [](u64& outHash) noexcept -> bool {
        World          world;
        PhysicsSystem  phys;
        GameplaySystem game;
        if (!world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U}).has_value()) return false;
        if (!phys.Init(world).has_value()) return false;

        GameplayCallbacks cb{};
        cb.onStateStep = [](EntityId id, StateMachine2D& fsm, World& w, const TickInput&, ActionBuffer&, GameplaySystem& g) noexcept {
            if (fsm.state == 0U && fsm.stateTicks == 10U) {
                fsm.TransitionTo(1U, 8U, 0U);
                if (auto* box = w.TryGet<CombatBox2D>(id)) {
                    box->BeginSwing({0.2f, -0.4f, 0.9f, 0.8f}, 12, {5.0f, 2.0f}, 2U);
                }
                g.EmitCue({.emitter = id, .kind = CueKind::Audio});
            }
        };
        cb.onStateTransition = [](EntityId id, u8 from, u8 to, World& w, GameplaySystem&) noexcept {
            if (from == 1U && to == 0U) {
                if (auto* box = w.TryGet<CombatBox2D>(id)) box->EndSwing();
            }
        };
        if (!game.Init(world, {}, cb).has_value()) return false;

        std::array<TileFlag, 24U * 8U> tiles{};
        tiles.fill(TileFlag::Empty);
        for (u32 x = 0U; x < 24U; ++x) tiles[x] = TileFlag::Solid;
        (void)phys.BuildTileMap(24U, 8U, 1.0f, {0.0f, 0.0f}, tiles);

        for (u32 i = 0U; i < 8U; ++i) {
            const EntityId e = *world.Spawn({.local = {.x = 2.0f + static_cast<f32>(i) * 1.1f, .y = 1.5f}});
            (void)world.Emplace<Collider2D>(e, Collider2D{.halfExtents = {0.35f, 0.5f}, .layerMask = 2U, .queryMask = 1U});
            (void)world.Emplace<PhysicsBody2D>(e, PhysicsBody2D{.type = BodyType::Dynamic});
            (void)world.Emplace<CharacterController2D>(e, CharacterController2D{
                .moveInputX = (i & 1U) ? -0.5f : 0.5f,
                .facing     = (i & 1U) ? static_cast<i8>(-1) : static_cast<i8>(1),
            });
            (void)world.Emplace<StateMachine2D>(e);
            (void)world.Emplace<Health2D>(e, Health2D{.hp = 200, .maxHp = 200, .team = static_cast<u8>((i & 1U) + 1U)});
            (void)world.Emplace<CombatBox2D>(e);
        }

        TimeSystem   time;
        TickInput    in{};
        ActionBuffer actions{};
        for (u32 t = 0U; t < 240U; ++t) {
            time.BeginFrame(kFixedDtNs);
            const TickStep step = time.PollTick();
            world.BeginTick(step);
            game.Step(world, phys, step, in, actions);
            (void)game.DrainCues();
            world.EndTick(step);
        }
        outHash = HashCombine(game.ComputeAuditHash(world), phys.ComputeAuditHash(world));
        return game.TotalCombatHits() > 0U;
    };

    u64 hashA = 0ULL;
    u64 hashB = 0ULL;
    CTS_CHECK(r, runScenario(hashA) && runScenario(hashB));
    CTS_CHECK(r, hashA == hashB && hashA != 0ULL);
    r.RecordFuzzOps(240U * 8U);
    r.MixDigest(hashA);
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "GameplayTests (CTS+DST)"};
    runner.RunCase("Locomotion, Coyote, Variable Jump, Wall & Dash", CtsCharacterLocomotionCoyoteWallJumpAndDash);
    runner.RunCase("Combat Hitboxes, Timelines, Pogo, Parry & Clash", CtsCombatHitboxesIframesParryAndClash);
    runner.RunCase("Camera Confinement, WorldMap2D & Mutation Save", CtsCameraFollowRoomBoundsWorldMapAndFlags);
    runner.RunCase("DST: 240-Tick FSM & Combat Lockstep Stress", DstGameplayStateMachineAndLockstepStress);
    return runner.Finish();
}
