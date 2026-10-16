#include "framework/Application.h"
#include "framework/FrameworkTypes.h"
#include "framework/SceneDirector.h"
#include "assets/AssetSystem.h"
#include "world/WorldTypes.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace en;
using namespace en::framework;

u32 g_testsPassed = 0;
u32 g_testsFailed = 0;

void AssertTrue(bool condition, std::string_view expression, int line) {
    if (condition) {
        ++g_testsPassed;
    } else {
        ++g_testsFailed;
        std::cerr << "[FAIL] Line " << line << ": " << expression << "\n";
    }
}

#define TEST_ASSERT(cond) AssertTrue(static_cast<bool>(cond), #cond, __LINE__)

void TestFrameworkTypesAndLayout() {
    static_assert(sizeof(FrameworkError) == 4);
    static_assert(sizeof(SceneLoadContext) == 64);
    static_assert(sizeof(StateLayerDesc) == 48);
    static_assert(sizeof(CookedSceneManifestHeader) == 48);
    static_assert(sizeof(CookedSceneAssetEntry) == 16);
    static_assert(sizeof(SceneTransitionDesc) == 64);
    static_assert(sizeof(FrameAuditDigest) == 64);
    static_assert(sizeof(PendingStateCmd) == 64);

    const StateLayerFlags f = StateLayerFlags::BlockLowerSimTicks | StateLayerFlags::PauseSimClock;
    TEST_ASSERT(HasStateFlag(f, StateLayerFlags::BlockLowerSimTicks));
    TEST_ASSERT(HasStateFlag(f, StateLayerFlags::PauseSimClock));
    TEST_ASSERT(!HasStateFlag(f, StateLayerFlags::BlockLowerRender));

    FrameworkError err{FrameworkErrc::InvalidConfig, SubSystemStage::Assets, 42, 0};
    TEST_ASSERT(err.code == FrameworkErrc::InvalidConfig);
    TEST_ASSERT(err.stage == SubSystemStage::Assets);
    TEST_ASSERT(err.subSystemErrc == 42);
}

void TestSceneDirectorStateStackOperations() {
    assets::AssetSystem assets;
    (void)assets.Init();

    audio::AudioSystem audio;
    audio::AudioSystemConfig audioCfg{};
    audioCfg.headlessMode = true;
    (void)audio.Init(audioCfg);

    SceneDirector director;
    auto initRes = director.Init(assets);
    TEST_ASSERT(initRes.has_value());
    TEST_ASSERT(director.StateDepth() == 0);

    // Mock state tracking counters
    struct TestStateData {
        int enters{0};
        int exits{0};
        int simTicks{0};
        int renderExtracts{0};
    };
    TestStateData dataA{};
    TestStateData dataB{};

    auto onEnterA = [](SceneLoadContext&, void* d) noexcept {
        static_cast<TestStateData*>(d)->enters++;
    };
    auto onExitA = [](SceneLoadContext&, void* d) noexcept {
        static_cast<TestStateData*>(d)->exits++;
    };
    auto onSimA = [](SimContext& ctx) noexcept {
        static_cast<TestStateData*>(ctx.userState)->simTicks++;
    };
    auto onRenderA = [](RenderContext& ctx) noexcept {
        static_cast<TestStateData*>(ctx.userState)->renderExtracts++;
    };

    StateLayerDesc layerA{
        .onSimTick = onSimA,
        .onRenderExtract = onRenderA,
        .onEnter = onEnterA,
        .onExit = onExitA,
        .stateData = &dataA,
        .id = 100,
        .inputActionMask = 0xFFFF,
        .flags = StateLayerFlags::None
    };

    StateLayerDesc layerB{
        .onSimTick = [](SimContext& ctx) noexcept { static_cast<TestStateData*>(ctx.userState)->simTicks++; },
        .onRenderExtract = [](RenderContext& ctx) noexcept { static_cast<TestStateData*>(ctx.userState)->renderExtracts++; },
        .onEnter = [](SceneLoadContext&, void* d) noexcept { static_cast<TestStateData*>(d)->enters++; },
        .onExit = [](SceneLoadContext&, void* d) noexcept { static_cast<TestStateData*>(d)->exits++; },
        .stateData = &dataB,
        .id = 200,
        .inputActionMask = 0x00FF,
        .flags = StateLayerFlags::None
    };

    // 1. Queue Push layerA
    auto pushRes = director.PushState(layerA);
    TEST_ASSERT(pushRes.has_value());

    // Before commit, stateDepth is still 0
    TEST_ASSERT(director.StateDepth() == 0);

    // Create dummy contexts for safe frame commit
    world::World world;
    (void)world.Init();
    gameplay::GameplayPipeline gameplay;
    input::InputPipeline input;
    time::TimeManager timeMgr;

    SceneLoadContext loadCtx{
        .world = world,
        .gameplay = gameplay,
        .assets = assets,
        .audio = audio
    };

    auto commitRes = director.CommitSafeFramePoint(loadCtx, input, timeMgr);
    TEST_ASSERT(commitRes.has_value());
    TEST_ASSERT(director.StateDepth() == 1);
    TEST_ASSERT(dataA.enters == 1);

    // 2. Queue Push layerB
    (void)director.PushState(layerB);
    (void)director.CommitSafeFramePoint(loadCtx, input, timeMgr);
    TEST_ASSERT(director.StateDepth() == 2);
    TEST_ASSERT(dataB.enters == 1);

    // 3. SwitchState on top
    TestStateData dataC{};
    StateLayerDesc layerC = layerB;
    layerC.stateData = &dataC;
    layerC.onEnter = [](SceneLoadContext&, void* d) noexcept { static_cast<TestStateData*>(d)->enters++; };
    layerC.onExit = [](SceneLoadContext&, void* d) noexcept { static_cast<TestStateData*>(d)->exits++; };

    (void)director.SwitchState(layerC);
    (void)director.CommitSafeFramePoint(loadCtx, input, timeMgr);
    TEST_ASSERT(director.StateDepth() == 2);
    TEST_ASSERT(dataB.exits == 1);
    TEST_ASSERT(dataC.enters == 1);

    // 4. PopState
    (void)director.PopState();
    (void)director.CommitSafeFramePoint(loadCtx, input, timeMgr);
    TEST_ASSERT(director.StateDepth() == 1);
    TEST_ASSERT(dataC.exits == 1);

    // 5. ClearAndPushState
    (void)director.ClearAndPushState(layerB);
    (void)director.CommitSafeFramePoint(loadCtx, input, timeMgr);
    TEST_ASSERT(director.StateDepth() == 1);
    TEST_ASSERT(dataA.exits == 1); // Layer A popped during clear
    TEST_ASSERT(dataB.enters == 2);

    director.Shutdown(assets, audio);
    audio.Shutdown();
    world.Shutdown();
    assets.Shutdown();
}

void TestSceneDirectorBitmaskMasksAndOcclusion() {
    assets::AssetSystem assets;
    (void)assets.Init();
    audio::AudioSystem audio;
    audio::AudioSystemConfig audioCfg{.headlessMode = true};
    (void)audio.Init(audioCfg);

    SceneDirector director;
    (void)director.Init(assets);

    world::World world;
    (void)world.Init();
    gameplay::GameplayPipeline gameplay;
    platform::Platform platform;
    input::InputPipeline input;
    input::ActionBuffer actionBuffer;
    time::TimeManager timeMgr;
    auto renderList = std::make_unique<renderer::RenderList>();
    platform::WindowState winState{};

    SceneLoadContext loadCtx{
        .world = world,
        .gameplay = gameplay,
        .assets = assets,
        .audio = audio
    };

    struct LayerCounters {
        int sim{0};
        int render{0};
    };
    LayerCounters cnt0{};
    LayerCounters cnt1{};

    // Layer 0: base game
    StateLayerDesc baseLayer{
        .onSimTick = [](SimContext& ctx) noexcept { static_cast<LayerCounters*>(ctx.userState)->sim++; },
        .onRenderExtract = [](RenderContext& ctx) noexcept { static_cast<LayerCounters*>(ctx.userState)->render++; },
        .stateData = &cnt0,
        .id = 1,
        .inputActionMask = 0xFFFF,
        .flags = StateLayerFlags::None
    };

    // Layer 1: pause menu overlay blocking lower sim ticks, render, and pausing clock
    StateLayerDesc pauseOverlay{
        .onSimTick = [](SimContext& ctx) noexcept { static_cast<LayerCounters*>(ctx.userState)->sim++; },
        .onRenderExtract = [](RenderContext& ctx) noexcept { static_cast<LayerCounters*>(ctx.userState)->render++; },
        .stateData = &cnt1,
        .id = 2,
        .inputActionMask = 0x000F,
        .flags = StateLayerFlags::BlockLowerSimTicks | StateLayerFlags::BlockLowerRender | StateLayerFlags::PauseSimClock
    };

    (void)director.PushState(baseLayer);
    (void)director.CommitSafeFramePoint(loadCtx, input, timeMgr);
    TEST_ASSERT(!director.IsClockPausedByState());

    // Dispatch 1 tick: Layer 0 runs
    SimContext simCtx{
        .world = world,
        .gameplay = gameplay,
        .assetBank = assets.Bank(),
        .audio = audio,
        .platform = platform,
        .actionBuffer = actionBuffer
    };
    director.DispatchSimTick(simCtx);
    TEST_ASSERT(cnt0.sim == 1);

    RenderContext renderCtx{
        .world = world,
        .gameplay = gameplay,
        .assetBank = assets.Bank(),
        .renderList = *renderList,
        .frameInput = input.GetFrameView(),
        .windowState = winState
    };
    director.DispatchRenderExtract(renderCtx);
    TEST_ASSERT(cnt0.render == 1);

    // Push pause overlay
    (void)director.PushState(pauseOverlay);
    (void)director.CommitSafeFramePoint(loadCtx, input, timeMgr);

    // Verify clock is paused by state layer flag
    TEST_ASSERT(director.IsClockPausedByState());

    // Dispatch again: Layer 0 is occluded (blocked), only Layer 1 executes!
    director.DispatchSimTick(simCtx);
    director.DispatchRenderExtract(renderCtx);
    TEST_ASSERT(cnt0.sim == 1);    // Did not advance!
    TEST_ASSERT(cnt0.render == 1); // Did not advance!
    TEST_ASSERT(cnt1.sim == 1);
    TEST_ASSERT(cnt1.render == 1);

    // Pop pause overlay
    (void)director.PopState();
    (void)director.CommitSafeFramePoint(loadCtx, input, timeMgr);
    TEST_ASSERT(!director.IsClockPausedByState());

    // Layer 0 is unblocked again
    director.DispatchSimTick(simCtx);
    TEST_ASSERT(cnt0.sim == 2);

    director.Shutdown(assets, audio);
    audio.Shutdown();
    world.Shutdown();
    assets.Shutdown();
}

void TestSceneDirectorNonBlockingSceneStreaming() {
    assets::AssetSystem assets;
    (void)assets.Init();
    audio::AudioSystem audio;
    audio::AudioSystemConfig audioCfg{.headlessMode = true};
    (void)audio.Init(audioCfg);

    SceneDirector director;
    (void)director.Init(assets);

    world::World world;
    (void)world.Init();
    gameplay::GameplayPipeline gameplay;
    input::InputPipeline input;
    time::TimeManager timeMgr;

    SceneLoadContext loadCtx{
        .world = world,
        .gameplay = gameplay,
        .assets = assets,
        .audio = audio
    };

    SceneTransitionDesc desc{
        .sceneId = assets::AssetId{9901},
        .onActivated = [](SceneLoadContext& ctx, assets::AssetId) noexcept -> bool {
            return ctx.sceneScope != assets::kInvalidScope;
        },
        .mode = SceneTransitionMode::FullReset,
        .waitPolicy = SceneWaitPolicy::ImmediateBarrier
    };

    auto transRes = director.RequestSceneTransition(desc, assets);
    TEST_ASSERT(transRes.has_value());
    TEST_ASSERT(director.Phase() == TransitionPhase::ReadyToActivate);

    auto commitRes = director.CommitSafeFramePoint(loadCtx, input, timeMgr);
    TEST_ASSERT(commitRes.has_value());
    TEST_ASSERT(*commitRes == true); // Scene activated!
    TEST_ASSERT(director.ActiveSceneId() == desc.sceneId);
    TEST_ASSERT(director.ActiveSceneScope() != assets::kInvalidScope);
    TEST_ASSERT(director.Phase() == TransitionPhase::Idle);

    director.Shutdown(assets, audio);
    audio.Shutdown();
    world.Shutdown();
    assets.Shutdown();
}

void TestApplicationHeadlessInitAndLifecycle() {
    auto app = std::make_unique<Application>();
    ApplicationConfig cfg{};
    cfg.headless = true;
    cfg.appName = "EngineN_Test";

    static bool s_bootCalled = false;
    static bool s_preShutdownCalled = false;
    static bool s_componentsRegistered = false;
    s_bootCalled = false;
    s_preShutdownCalled = false;
    s_componentsRegistered = false;

    AppHooks hooks{};
    hooks.OnRegisterComponents = [](world::World&, void*) noexcept -> Result<void, u8> {
        s_componentsRegistered = true;
        return {};
    };
    hooks.OnBoot = [](SceneLoadContext&, SceneDirector&) noexcept -> Result<void, u8> {
        s_bootCalled = true;
        return {};
    };
    hooks.OnPreShutdown = [](SceneLoadContext&) noexcept {
        s_preShutdownCalled = true;
    };

    auto initRes = app->Init(cfg, hooks);
    TEST_ASSERT(initRes.has_value());
    TEST_ASSERT(s_componentsRegistered);
    TEST_ASSERT(s_bootCalled);

    // Accessors
    TEST_ASSERT(!app->World().IsAlive(world::kNullEntity));
    TEST_ASSERT(app->Director().StateDepth() == 0);
    TEST_ASSERT(!app->Time().IsPaused());

    app->Shutdown();
    TEST_ASSERT(s_preShutdownCalled);
}

void TestApplicationHeadlessStepAndAuditHash() {
    auto app = std::make_unique<Application>();
    ApplicationConfig cfg{};
    cfg.headless = true;

    AppHooks hooks{};
    hooks.OnBoot = [](SceneLoadContext&, SceneDirector& director) noexcept -> Result<void, u8> {
        // Push a basic idle state layer
        StateLayerDesc idleLayer{
            .onSimTick = [](SimContext&) noexcept {},
            .onRenderExtract = [](RenderContext&) noexcept {},
            .id = 1
        };
        (void)director.PushState(idleLayer);
        return {};
    };

    auto initRes = app->Init(cfg, hooks);
    TEST_ASSERT(initRes.has_value());

    // Step 10 headless ticks
    auto stepRes = app->StepHeadless(10, 2);
    TEST_ASSERT(stepRes.has_value());

    const FrameAuditDigest digest1 = *stepRes;
    TEST_ASSERT(digest1.tickIndex == 10);
    TEST_ASSERT(digest1.combinedHash != 0);

    // Compute direct digest
    const FrameAuditDigest directDigest = app->ComputeFrameAuditDigest();
    TEST_ASSERT(directDigest.combinedHash == digest1.combinedHash);

    app->Shutdown();
}

void TestApplicationCameraListenerSyncAndAudioDucking() {
    auto app = std::make_unique<Application>();
    ApplicationConfig cfg{};
    cfg.headless = true;

    AppHooks hooks{};
    hooks.OnBoot = [](SceneLoadContext& ctx, SceneDirector&) noexcept -> Result<void, u8> {
        // Spawn active camera entity
        auto spawnRes = ctx.world.Spawn({.hasSpatial = true});
        if (!spawnRes) return Err(1u);
        const world::EntityId camEntity = *spawnRes;

        world::LocalTransform2D lt{};
        lt.x = 24.0f;
        lt.y = 12.0f;
        ctx.world.SetLocalTransform(camEntity, lt);

        world::CameraRig2D rig{};
        rig.active = true;
        rig.viewSlot = 0;
        rig.screenSpace = false;
        (void)ctx.world.Emplace<world::CameraRig2D>(camEntity, rig);
        ctx.world.PropagateTransforms();
        return {};
    };

    (void)app->Init(cfg, hooks);

    // Step a frame: camera listener sync should track camera entity at (24, 12)
    auto stepRes = app->StepHeadless(1, 1);
    TEST_ASSERT(stepRes.has_value());

    app->Shutdown();
}

void TestApplicationSimCueRouting() {
    auto app = std::make_unique<Application>();
    ApplicationConfig cfg{};
    cfg.headless = true;

    static int s_cuesReceived = 0;
    s_cuesReceived = 0;

    AppHooks hooks{};
    hooks.OnConsumeSimCue = [](const gameplay::SimCue&, world::World&,
                               const assets::AssetBank&, void*) noexcept {
        s_cuesReceived++;
    };
    hooks.OnBoot = [](SceneLoadContext&, SceneDirector& director) noexcept -> Result<void, u8> {
        StateLayerDesc emitterLayer{
            .onSimTick = [](SimContext& ctx) noexcept {
                // Emit an audio one-shot cue
                ctx.gameplay.EmitCue(gameplay::SimCue{
                    .assetId = assets::AssetId{1234},
                    .worldX = 5.0f,
                    .worldY = 10.0f,
                    .kind = gameplay::SimCueKind::AudioOneShot
                });
            },
            .id = 55
        };
        (void)director.PushState(emitterLayer);
        return {};
    };

    (void)app->Init(cfg, hooks);

    // Run 2 ticks
    auto stepRes = app->StepHeadless(2, 1);
    TEST_ASSERT(stepRes.has_value());
    TEST_ASSERT(s_cuesReceived == 2);

    app->Shutdown();
}

void TestSceneDirectorAuditHashCombiner() {
    const u64 h1 = SceneDirector::CombineAuditHashes(1, 100, 200, 300, 400, 500, 600);
    const u64 h2 = SceneDirector::CombineAuditHashes(1, 100, 200, 300, 400, 500, 600);
    const u64 h3 = SceneDirector::CombineAuditHashes(2, 100, 200, 300, 400, 500, 600);

    TEST_ASSERT(h1 != 0);
    TEST_ASSERT(h1 == h2); // Deterministic
    TEST_ASSERT(h1 != h3); // Perturbation detection
}

} // namespace

int main() {
    std::cout << "Running EngineN Framework Subsystem Tests...\n" << std::flush;

    std::cout << "  [1/8] TestFrameworkTypesAndLayout...\n" << std::flush;
    TestFrameworkTypesAndLayout();

    std::cout << "  [2/8] TestSceneDirectorStateStackOperations...\n" << std::flush;
    TestSceneDirectorStateStackOperations();

    std::cout << "  [3/8] TestSceneDirectorBitmaskMasksAndOcclusion...\n" << std::flush;
    TestSceneDirectorBitmaskMasksAndOcclusion();

    std::cout << "  [4/8] TestSceneDirectorNonBlockingSceneStreaming...\n" << std::flush;
    TestSceneDirectorNonBlockingSceneStreaming();

    std::cout << "  [5/8] TestApplicationHeadlessInitAndLifecycle...\n" << std::flush;
    TestApplicationHeadlessInitAndLifecycle();

    std::cout << "  [6/8] TestApplicationHeadlessStepAndAuditHash...\n" << std::flush;
    TestApplicationHeadlessStepAndAuditHash();

    std::cout << "  [7/8] TestApplicationCameraListenerSyncAndAudioDucking...\n" << std::flush;
    TestApplicationCameraListenerSyncAndAudioDucking();

    std::cout << "  [8/8] TestApplicationSimCueRouting and Audit Combiner...\n" << std::flush;
    TestApplicationSimCueRouting();
    TestSceneDirectorAuditHashCombiner();

    std::cout << "Test Summary: " << g_testsPassed << " passed, " << g_testsFailed << " failed.\n" << std::flush;
    return g_testsFailed == 0 ? 0 : 1;
}
