#include "debug/DebugSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "input/InputPipeline.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace en::tests {

struct TestSuite {
    u32 passed{0U};
    u32 failed{0U};

    void Expect(bool cond, const char* expr, int line) noexcept {
        if (cond) {
            ++passed;
        } else {
            ++failed;
            std::printf("[FAIL] Line %d: %s\n", line, expr);
        }
    }
};

#define EXPECT_TRUE(suite, expr) (suite).Expect(static_cast<bool>(expr), #expr, __LINE__)
#define EXPECT_EQ(suite, a, b) (suite).Expect((a) == (b), #a " == " #b, __LINE__)
#define EXPECT_NEAR(suite, a, b, eps) (suite).Expect(std::abs((a) - (b)) <= (eps), #a " ~= " #b, __LINE__)

// Compile-time struct size & cache-line invariants (0B hidden compiler padding)
static_assert(sizeof(DebugLoopDirectives) == 8);
static_assert(sizeof(DebugShapeCmd) == 32);
static_assert(sizeof(FrameProfileSample) == 64);
static_assert(sizeof(ActorTickRecord) == 64);
static_assert(sizeof(SubsystemTelemetryDesc) == 32);
static_assert(sizeof(WatchPlotTrack) == 288);
static_assert(sizeof(DebugFrameContext) == 64);
static_assert(sizeof(DebugSystemConfig) == 16);
static_assert(sizeof(DebugBatcher) == 16);
static_assert(sizeof(DebugShapePool) == 288);
static_assert(sizeof(DebugInspector) == 16);
static_assert(sizeof(ConsoleSymbol) == 32);
static_assert(sizeof(LogEntryMeta) == 8);
static_assert(sizeof(DebugProfiler) == 160);
static_assert(sizeof(DebugConsole) == 2752);
static_assert(sizeof(DebugSystem) == 64);

#if EN_ENABLE_DEBUG_TOOLS

struct MockClock {
    static inline u64 s_nowNs{1'000'000ULL};
    [[nodiscard]] static u64 NowNs() noexcept {
        s_nowNs += 1'250'000ULL; // +1250 us per call
        return s_nowNs;
    }
};

void TestShapePoolBitmaskExpirationAndBatcherFormatting(TestSuite& suite) noexcept {
    DebugSystem dbg;
    EXPECT_TRUE(suite, dbg.Init().has_value());

    char buf[64]{};
    char* p = DebugBatcher::FormatI64(buf, buf + sizeof(buf), -4200);
    EXPECT_EQ(suite, (std::string_view{buf, static_cast<usize>(p - buf)}), "-4200");

    p = DebugBatcher::FormatU64Hex(buf, buf + sizeof(buf), 0xDEADBEEFULL, 8U);
    EXPECT_EQ(suite, (std::string_view{buf, static_cast<usize>(p - buf)}), "0xDEADBEEF");

    p = DebugBatcher::FormatF32Fixed2(buf, buf + sizeof(buf), 16.67f);
    EXPECT_EQ(suite, (std::string_view{buf, static_cast<usize>(p - buf)}), "16.67");

    dbg.Shapes().SetCurrentTick(10U);
    dbg.Shapes().AddLine(0.0f, 0.0f, 5.0f, 5.0f, Color{1.0f, 0.0f, 0.0f, 1.0f}, 0.05f, 0U);  // expires at tick >= 10
    dbg.Shapes().AddCircle(2.0f, 3.0f, 1.5f, Color{0.0f, 1.0f, 0.0f, 1.0f}, 16U, 0.05f, 3U); // expires at tick >= 13
    dbg.Shapes().AddTextWorld(1.0f, 2.0f, "HIT!", Color{1.0f, 1.0f, 0.0f, 1.0f}, 0.1f, 2U);  // expires at tick >= 12
    EXPECT_EQ(suite, dbg.Shapes().ActiveCount(), 3U);
    EXPECT_EQ(suite, dbg.Shapes().TotalSubmitted(), 3ULL);

    auto renderList = std::make_unique<RenderList>();
    renderList->Reset({1280, 720});

    DebugFrameContext ctx{};
    ctx.currentTickIndex = 10U;
    dbg.EmitOverlays(ctx, *renderList);
    EXPECT_EQ(suite, dbg.Shapes().ActiveCount(), 2U);
    EXPECT_TRUE(suite, renderList->Stats().submittedQuads > 10U);

    ctx.currentTickIndex = 12U;
    dbg.EmitOverlays(ctx, *renderList);
    EXPECT_EQ(suite, dbg.Shapes().ActiveCount(), 1U);

    ctx.currentTickIndex = 13U;
    dbg.EmitOverlays(ctx, *renderList);
    EXPECT_EQ(suite, dbg.Shapes().ActiveCount(), 0U);

    dbg.Shutdown();
}

void TestInspector1HopPickingJointsBroadphaseInputViewerAndFlightRecorder(TestSuite& suite) noexcept {
    World world;
    EXPECT_TRUE(suite, world.Init().has_value());

    PhysicsSystem physics;
    EXPECT_TRUE(suite, physics.Init(world).has_value());
    TileMaterialFlags tiles[4]{
        TileMaterialFlags::Solid, TileMaterialFlags::OneWay,
        TileMaterialFlags::Empty, TileMaterialFlags::Solid};
    EXPECT_TRUE(suite, physics.BuildTileMap(2U, 2U, 1.0f, 0.0f, 0.0f, tiles).has_value());

    GameplayPipeline gameplay;
    EXPECT_TRUE(suite, gameplay.Init(world).has_value());

    EntitySpawnDesc spawnDesc{};
    spawnDesc.local = LocalTransform2D{4.0f, 2.0f};
    const EntityId hero = world.Spawn(spawnDesc).value_or(kNullEntity);
    const EntityId anchor = world.Spawn(spawnDesc).value_or(kNullEntity);
    EXPECT_TRUE(suite, hero.IsValid() && anchor.IsValid());

    (void)world.Emplace<Collider2D>(hero, Collider2D{
        .localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.5f, 1.0f),
        .shape = ShapeKind2D::CapsuleY,
    });
    (void)world.Emplace<CharacterMotor2D>(hero, CharacterMotor2D{.vx = 6.5f, .vy = -1.2f});
    (void)world.Emplace<MotionStack2D>(hero, MotionStack2D{});
    (void)world.Emplace<StateMachine2D>(hero, StateMachine2D{.stateTicks = 7U, .stateId = 3U});
    (void)world.Emplace<HitboxRig2D>(hero, HitboxRig2D{
        .hurtbox = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.9f),
    });
    (void)world.Emplace<DamageReceiver2D>(hero, DamageReceiver2D{.hp = 85, .maxHp = 100, .iframeTicks = 12U});

    DistanceJoint2D joint{};
    joint.entityA = hero;
    joint.entityB = anchor;
    joint.SetSoftSpring(2.0f, 4.0f, 0.7f);
    (void)world.Emplace<DistanceJoint2D>(hero, joint);
    world.PropagateTransforms();

    DebugSystem dbg;
    EXPECT_TRUE(suite, dbg.Init().has_value());
    dbg.SetActiveOverlays(OverlayFlags::AllWorld | OverlayFlags::AllHud);

    const EntityId picked = dbg.Inspector().PickEntityAtWorldPos(world, {4.1f, 2.2f}, 1.0f);
    EXPECT_EQ(suite, picked, hero);
    EXPECT_EQ(suite, dbg.Inspector().PinnedEntity(), hero);

    TickInput tickIn{};
    tickIn.held = 0x0003U;
    tickIn.pressed = 0x0001U;
    tickIn.axis[0] = 0.75f;
    tickIn.axis[1] = -0.5f;
    ActionBuffer actBuf{};

    for (u32 t = 1U; t <= 5U; ++t) {
        if (auto* sm = world.TryGet<StateMachine2D>(hero)) {
            sm->stateId = static_cast<u8>(t);
        }
        dbg.EndSimTick(world, t, &tickIn, &actBuf);
    }
    EXPECT_EQ(suite, dbg.Inspector().RecordedTickCount(), 5U);
    EXPECT_TRUE(suite, dbg.Inspector().InspectedRecord() != nullptr);
    EXPECT_EQ(suite, dbg.Inspector().InspectedRecord()->tickIndex, 5U);
    EXPECT_EQ(suite, dbg.Inspector().InspectedRecord()->stateId, 5U);
    EXPECT_EQ(suite, dbg.Inspector().InspectedRecord()->hp, 85);

    dbg.Inspector().StepScrubOffset(3);
    EXPECT_EQ(suite, dbg.Inspector().ScrubOffset(), 3U);
    EXPECT_EQ(suite, dbg.Inspector().InspectedRecord()->tickIndex, 2U);
    EXPECT_EQ(suite, dbg.Inspector().InspectedRecord()->stateId, 2U);

    // Plot real-time variable sparkline samples
    dbg.Profiler().PlotValue("hero.vy", -1.2f);
    dbg.Profiler().PlotValue("hero.vy",  3.4f);
    dbg.Profiler().PlotValue("hero.vy",  1.1f);
    const WatchPlotTrack* plot = dbg.Profiler().FindPlotTrack("hero.vy");
    EXPECT_TRUE(suite, plot != nullptr);
    EXPECT_EQ(suite, plot->validCount, 3U);
    EXPECT_NEAR(suite, plot->minVal, -1.2f, 1e-5f);
    EXPECT_NEAR(suite, plot->maxVal,  3.4f, 1e-5f);

    auto renderList = std::make_unique<RenderList>();
    renderList->Reset({1280, 720});
    AudioEmitterView emitters[1]{{4.0f, 2.0f, 12.0f, 0.0f}};
    DebugFrameContext ctx{};
    ctx.world = &world;
    ctx.physics = &physics;
    ctx.gameplay = &gameplay;
    ctx.audioEmitters = emitters;
    ctx.currentTickIndex = 5U;
    dbg.EmitOverlays(ctx, *renderList);
    EXPECT_TRUE(suite, renderList->Stats().submittedQuads > 30U);

    dbg.Shutdown();
    gameplay.Shutdown();
    physics.Shutdown();
    world.Shutdown();
}

void TestProfilerPercentilesConsoleCVarsHistoryClearAndLoopDirectives(TestSuite& suite) noexcept {
    DebugSystem dbg;
    EXPECT_TRUE(suite, dbg.Init().has_value());

    f32 gravityScale = 1.0f;
    i32 maxJumpCount = 2;
    bool godMode = false;
    EXPECT_TRUE(suite, dbg.Console().RegisterCVarF32("phys.gravity_scale", &gravityScale, 0.0f, 10.0f).has_value());
    EXPECT_TRUE(suite, dbg.Console().RegisterCVarI32("plr.max_jumps", &maxJumpCount, 1, 5).has_value());
    EXPECT_TRUE(suite, dbg.Console().RegisterCVarBool("cheat.god_mode", &godMode).has_value());

    InputPipeline inputPipe{};
    inputPipe.Reset();
    EXPECT_EQ(suite, inputPipe.GetActionMask(), 0xFFFFU);

    // Open console with grave key (scancode 53) -> saves and zeroes InputPipeline action mask
    EXPECT_TRUE(suite, dbg.OnPlatformKeyEvent(53U, true, false, false, inputPipe));
    EXPECT_TRUE(suite, dbg.Console().IsOpen());
    EXPECT_EQ(suite, inputPipe.GetActionMask(), 0x0000U);

    // Type prefix "phys.grav" and press Tab (scancode 43) -> autocompletes to "phys.gravity_scale "
    EXPECT_TRUE(suite, dbg.OnPlatformTextInput("phys.grav"));
    EXPECT_TRUE(suite, dbg.OnPlatformKeyEvent(43U, true, false, false, inputPipe));
    EXPECT_EQ(suite, dbg.Console().InputView(), "phys.gravity_scale ");

    // Append "2.5" and press Enter (scancode 40)
    EXPECT_TRUE(suite, dbg.OnPlatformTextInput("2.5"));
    EXPECT_TRUE(suite, dbg.OnPlatformKeyEvent(40U, true, false, false, inputPipe));

    // Verify Up (scancode 82) recalls history, and Down (scancode 81) clears back to empty prompt
    EXPECT_TRUE(suite, dbg.OnPlatformKeyEvent(82U, true, false, false, inputPipe));
    EXPECT_EQ(suite, dbg.Console().InputView(), "phys.gravity_scale 2.5");
    EXPECT_TRUE(suite, dbg.OnPlatformKeyEvent(81U, true, false, false, inputPipe));
    EXPECT_EQ(suite, dbg.Console().InputView(), "");

    // Enqueue built-in commands: "cheat.god_mode toggle", "step 4", "timescale 0.25 1"
    dbg.Console().EnqueueCommandLine("cheat.god_mode toggle");
    dbg.Console().EnqueueCommandLine("step 4");
    dbg.Console().EnqueueCommandLine("timescale 0.25 1");

    // Close console -> restores InputPipeline action mask
    EXPECT_TRUE(suite, dbg.OnPlatformKeyEvent(53U, true, false, false, inputPipe));
    EXPECT_TRUE(suite, !dbg.Console().IsOpen());
    EXPECT_EQ(suite, inputPipe.GetActionMask(), 0xFFFFU);

    DebugFrameContext ctx{};
    ctx.currentTickIndex = 100U;
    const DebugLoopDirectives dir = dbg.BeginFrame(ctx);
    EXPECT_NEAR(suite, gravityScale, 2.5f, 1e-5f);
    EXPECT_TRUE(suite, godMode);
    EXPECT_EQ(suite, dir.stepTicks, 4U);
    EXPECT_TRUE(suite, dir.ForcePause());
    EXPECT_NEAR(suite, dir.channelTimeScale, 0.25f, 1e-5f);
    EXPECT_EQ(suite, dir.targetTimeChannel, 1U);

    // Verify "clear" command actually resets LogCount to 0, and "reset" sets ResetRequested()
    EXPECT_TRUE(suite, dbg.Console().LogCount() > 0U);
    dbg.Console().ExecuteImmediate("clear", 101U);
    EXPECT_EQ(suite, dbg.Console().LogCount(), 0U);
    dbg.Console().ExecuteImmediate("cvarlist", 101U);
    EXPECT_EQ(suite, dbg.Console().LogCount(), 4U); // command echo + 3 CVars
    dbg.Console().ExecuteImmediate("reset", 101U);
    const DebugLoopDirectives dir2 = dbg.BeginFrame(ctx);
    EXPECT_TRUE(suite, dir2.ResetRequested());

    // Record profiler scopes and verify O(1) bucket percentile calculation
    {
        ScopedProfileTimer<MockClock> timer(dbg.Profiler(), ProfileScopeId::PhysicsStep);
    }
    SubsystemTelemetryDesc tele{};
    tele.aliveEntities = 42U;
    tele.submittedQuads = 512U;
    tele.drawBatches = 4U;
    tele.activeContacts = 7U;
    dbg.Profiler().RecordSubsystemTelemetry(tele);
    dbg.Profiler().EndFrame();

    EXPECT_EQ(suite, dbg.Profiler().ValidFrames(), 1U);
    EXPECT_EQ(suite, dbg.Profiler().Latest().aliveEntities, 42U);
    EXPECT_TRUE(suite, dbg.Profiler().ComputePercentileUs(250U) >= 1000U);

    const u64 h1 = dbg.ComputeAuditHash();
    EXPECT_TRUE(suite, h1 != 0ULL);

    dbg.Shutdown();
}

#endif // EN_ENABLE_DEBUG_TOOLS

void TestZeroOverheadReleaseStubMode(TestSuite& suite) noexcept {
    DebugSystem dbg;
    EXPECT_TRUE(suite, dbg.Init().has_value());
#if !EN_ENABLE_DEBUG_TOOLS
    EXPECT_TRUE(suite, !dbg.IsInitialized());
    EXPECT_EQ(suite, dbg.ComputeAuditHash(), 0ULL);
#endif
    dbg.Shutdown();
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestSuite suite{};
#if EN_ENABLE_DEBUG_TOOLS
    TestShapePoolBitmaskExpirationAndBatcherFormatting(suite);
    TestInspector1HopPickingJointsBroadphaseInputViewerAndFlightRecorder(suite);
    TestProfilerPercentilesConsoleCVarsHistoryClearAndLoopDirectives(suite);
#endif
    TestZeroOverheadReleaseStubMode(suite);

    std::printf("Phase 11 Debug v2 Verification: %u passed, %u failed\n", suite.passed, suite.failed);
    return (suite.failed == 0U) ? 0 : 1;
}
