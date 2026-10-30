#include "tests/subsystems/CtsHarness.h"

#include "debug/DebugSystem.h"
#include "gameplay/GameplaySystem.h"
#include "input/InputSystem.h"
#include "physics/PhysicsSystem.h"
#include "time/Clock.h"

namespace {

using namespace en;
using namespace en::cts;

bool CtsGizmoPersistenceAndOverlayEmission(CtsRunner& r) noexcept {
    DebugSystem dbg;
    CTS_CHECK(r, dbg.Init({.overlays = Overlay::Colliders | Overlay::Velocity | Overlay::Hitboxes | Overlay::Profiler}).has_value());
    CTS_CHECK(r, dbg.Ready());

    // Queue 1-tick transient shape and 5-tick persistent shape.
    dbg.EndTick(World{}, 10U);
    dbg.Shapes().Line(0.0f, 0.0f, 5.0f, 5.0f, {1.0f, 0.0f, 0.0f, 1.0f}, 0.05f, 1U);
    dbg.Shapes().Box({-1.0f, -1.0f, 2.0f, 2.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, 0.05f, false, 5U);
    CTS_CHECK(r, dbg.Shapes().Count() == 2U);

    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 64U, .defaultPoolCapacity = 64U}).has_value());
    PhysicsSystem phys;
    CTS_CHECK(r, phys.Init(world).has_value());
    GameplaySystem game;
    CTS_CHECK(r, game.Init(world).has_value());

    const EntityId hero = *world.Spawn({.local = {.x = 3.0f, .y = 4.0f}});
    (void)world.Emplace<Collider2D>(hero, Collider2D{.halfExtents = {0.5f, 0.5f}});
    (void)world.Emplace<PhysicsBody2D>(hero, PhysicsBody2D{.velocity = {5.0f, 2.0f}});
    (void)world.Emplace<CombatBox2D>(hero, CombatBox2D{.flags = CombatFlags::HurtboxActive | CombatFlags::HitboxActive});
    world.PropagateTransforms();

    RenderList list;
    list.Reset({1280, 720});
    FrameCtx ctx{.world = &world, .physics = &phys, .viewport = {1280, 720}, .tick = 11U};

    (void)dbg.BeginFrame(ctx);
    dbg.Profiler().Record(ProfileScope::Physics, 450U);
    dbg.Profiler().Record(ProfileScope::Gameplay, 350U);
    dbg.Emit(ctx, list);

    // At tick 11, the 1-tick line expired, leaving only the 5-tick box in ShapePool!
    CTS_CHECK(r, dbg.Shapes().Count() == 1U);
    CTS_CHECK(r, list.Stats().submitted > 5U);
    CTS_CHECK(r, dbg.Profiler().ValidFrames() == 1U);
    CTS_CHECK(r, dbg.Profiler().Latest().us[static_cast<usize>(ProfileScope::Frame)] == 800U);
    return true;
}

bool CtsConsoleCommandsCVarsAutocompleteAndHistory(CtsRunner& r) noexcept {
    DebugSystem dbg;
    CTS_CHECK(r, dbg.Init().has_value());

    f32  gravityScale = 1.0f;
    i32  maxIterations = 6;
    bool godMode = false;

    CTS_CHECK(r, dbg.Console().RegisterF32("phys.grav_scale", &gravityScale, 0.0f, 4.0f).has_value());
    CTS_CHECK(r, dbg.Console().RegisterI32("phys.iters", &maxIterations, 1, 32).has_value());
    CTS_CHECK(r, dbg.Console().RegisterBool("game.god", &godMode).has_value());
    CTS_CHECK(r, dbg.Console().RegisterBool("game.god", &godMode).error() == DebugErrc::DuplicateSymbol);

    // Execute CVar mutations and verify clamping.
    dbg.Console().Exec("phys.grav_scale 2.5");
    dbg.Console().Exec("phys.iters 64");
    dbg.Console().Exec("game.god on");
    CTS_CHECK(r, Near(gravityScale, 2.5f) && maxIterations == 32 && godMode);

    // Interactive console typing, autocomplete, history, and input mask suppression.
    InputSystem input;
    CTS_CHECK(r, dbg.OnKey(53U, true, input)); // Toggle console open
    CTS_CHECK(r, dbg.Console().IsOpen());
    CTS_CHECK(r, dbg.OnText("phys.gr"));
    CTS_CHECK(r, dbg.OnKey(43U, true, input)); // Tab autocomplete
    CTS_CHECK(r, dbg.Console().Input() == "phys.grav_scale");
    CTS_CHECK(r, dbg.OnText(" 0.5"));
    CTS_CHECK(r, dbg.OnKey(40U, true, input)); // Enter submit

    const LoopDirectives d0 = dbg.BeginFrame({});
    (void)d0;
    CTS_CHECK(r, Near(gravityScale, 0.5f));

    // Built-in engine loop directives via console commands.
    dbg.Console().Exec("step 4");
    dbg.Console().Exec("timescale 0.25 1");
    dbg.Console().Enqueue("reset");
    const LoopDirectives dir = dbg.BeginFrame({});
    CTS_CHECK(r, dir.ForcePause() && dir.stepTicks == 4U);
    CTS_CHECK(r, Near(dir.timeScale, 0.25f) && dir.timeChannel == 1U);
    CTS_CHECK(r, dir.Reset());
    return true;
}

bool CtsInspectorPickingFlightRecorderAndScrubbing(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 64U, .defaultPoolCapacity = 64U}).has_value());
    PhysicsSystem phys;
    CTS_CHECK(r, phys.Init(world).has_value());
    GameplaySystem game;
    CTS_CHECK(r, game.Init(world).has_value());

    const EntityId actor = *world.Spawn({.local = {.x = 4.0f, .y = -2.0f}});
    (void)world.Emplace<Collider2D>(actor, Collider2D{.halfExtents = {0.5f, 0.5f}});
    (void)world.Emplace<PhysicsBody2D>(actor, PhysicsBody2D{.velocity = {3.0f, 0.0f}});
    (void)world.Emplace<Health2D>(actor, Health2D{.hp = 85});
    (void)world.Emplace<StateMachine2D>(actor, StateMachine2D{.state = 3U});
    world.PropagateTransforms();

    DebugSystem dbg;
    CTS_CHECK(r, dbg.Init().has_value());

    const EntityId picked = dbg.Inspector().Pick(world, {4.2f, -1.9f}, 0.5f);
    CTS_CHECK(r, picked == actor && dbg.Inspector().Pinned() == actor);

    for (u32 t = 1U; t <= 10U; ++t) {
        LocalTransform2D& loc = world.ModifyLocalTransformByIndex(actor.index);
        loc.x = static_cast<f32>(t);
        world.PropagateTransforms();
        dbg.EndTick(world, t);
    }

    CTS_CHECK(r, dbg.Inspector().Recorded() == 10U);
    CTS_CHECK(r, dbg.Inspector().Current() != nullptr && dbg.Inspector().Current()->tick == 10U);
    CTS_CHECK(r, Near(dbg.Inspector().Current()->x, 10.0f));

    // Scrub backward 4 ticks -> tick 6!
    dbg.Inspector().Scrub(4);
    CTS_CHECK(r, dbg.Inspector().Current()->tick == 6U && Near(dbg.Inspector().Current()->x, 6.0f));
    dbg.Inspector().ResetScrub();
    CTS_CHECK(r, dbg.Inspector().Current()->tick == 10U);
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "DebugTests (CTS+DST)"};
    runner.RunCase("Gizmo Persistence, Overlays & Profiler", CtsGizmoPersistenceAndOverlayEmission);
    runner.RunCase("Console Commands, CVars, Autocomplete & Loop", CtsConsoleCommandsCVarsAutocompleteAndHistory);
    runner.RunCase("Entity Picking, Flight Recorder & Scrubbing", CtsInspectorPickingFlightRecorderAndScrubbing);
    return runner.Finish();
}
