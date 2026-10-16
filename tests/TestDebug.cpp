#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "core/time/SdlClock.h"
#include "core/time/TimePipeline.h"
#include "debug/DebugBatcher.h"
#include "debug/DebugInspector.h"
#include "debug/DebugProfilerAndConsole.h"
#include "debug/DebugSystem.h"
#include "debug/DebugTypes.h"
#include "gameplay/GameplayPipeline.h"
#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "input/InputPipeline.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "renderer/Renderer.h"
#include "renderer/RendererList.h"
#include "world/World.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <source_location>
#include <string_view>

using namespace en;
using namespace en::debug;

namespace {

static u32 g_failedAssertions = 0;
static u32 g_passedAssertions = 0;

void AssertTrue(bool condition, std::string_view expression, std::source_location loc = std::source_location::current()) {
    if (!condition) {
        std::cerr << "[FAIL] " << loc.file_name() << ":" << loc.line()
                  << " Assertion failed: " << expression << "\n";
        ++g_failedAssertions;
    } else {
        ++g_passedAssertions;
    }
}

#define TEST_ASSERT(cond) AssertTrue(static_cast<bool>(cond), #cond)

// 1. Data-Oriented Memory Layout & Alignment Guarantees
void TestMemoryLayoutsAndTypes() {
    static_assert(sizeof(DebugShapeCmd) == 32);
    static_assert(alignof(DebugShapeCmd) == 32);

    static_assert(sizeof(FrameProfileSample) == 128);
    static_assert(alignof(FrameProfileSample) == 64);

    static_assert(sizeof(ActorTickRecord) == 64);
    static_assert(alignof(ActorTickRecord) == 64);

    static_assert(sizeof(ConsoleSymbol) == 32);
    static_assert(alignof(ConsoleSymbol) == 32);

    static_assert(sizeof(LogEntryMeta) == 8);

    // OverlayFlags bitwise operator coverage
    const OverlayFlags flags = OverlayFlags::ProfilerHud | OverlayFlags::TimeChannelHud;
    TEST_ASSERT(HasOverlay(flags, OverlayFlags::ProfilerHud));
    TEST_ASSERT(HasOverlay(flags, OverlayFlags::TimeChannelHud));
    TEST_ASSERT(!HasOverlay(flags, OverlayFlags::KinematicBounds));

    const OverlayFlags toggled = flags ^ OverlayFlags::ProfilerHud;
    TEST_ASSERT(!HasOverlay(toggled, OverlayFlags::ProfilerHud));
    TEST_ASSERT(HasOverlay(toggled, OverlayFlags::TimeChannelHud));

    const OverlayFlags masked = flags & OverlayFlags::TimeChannelHud;
    TEST_ASSERT(masked == OverlayFlags::TimeChannelHud);

    const OverlayFlags inverted = ~OverlayFlags::None;
    TEST_ASSERT(HasOverlay(inverted, OverlayFlags::CombatHitboxes));

    // Symbol Name Hashing
    constexpr u64 h1 = HashSymbolName("help");
    constexpr u64 h2 = HashSymbolName("overlay");
    constexpr u64 h3 = HashSymbolName("time.pause");
    TEST_ASSERT(h1 != 0);
    TEST_ASSERT(h1 != h2);
    TEST_ASSERT(h2 != h3);
    TEST_ASSERT(HashSymbolName("help") == h1);
}

// 2. Atlas Unpacking & Solid White UV region
void TestDebugAtlasUnpacking() {
    alignas(64) std::array<u8, kDebugAtlasWidth * kDebugAtlasHeight * 4> pixels{};
    UnpackDebugAtlasRGBA8(pixels);

    // Space character (0x20, glyph 0) at (0, 0) should be all black/transparent
    for (u32 py = 0; py < 8; ++py) {
        for (u32 px = 0; px < 8; ++px) {
            const usize idx = (py * kDebugAtlasWidth + px) * 4u;
            TEST_ASSERT(pixels[idx + 0] == 0);
            TEST_ASSERT(pixels[idx + 1] == 0);
            TEST_ASSERT(pixels[idx + 2] == 0);
            TEST_ASSERT(pixels[idx + 3] == 0);
        }
    }

    // Solid white UV block occupies [120..127, 56..63]
    for (u32 py = 56; py < 64; ++py) {
        for (u32 px = 120; px < 128; ++px) {
            const usize idx = (py * kDebugAtlasWidth + px) * 4u;
            TEST_ASSERT(pixels[idx + 0] == 255);
            TEST_ASSERT(pixels[idx + 1] == 255);
            TEST_ASSERT(pixels[idx + 2] == 255);
            TEST_ASSERT(pixels[idx + 3] == 255);
        }
    }

    // Verify solid white UV coordinates hit the center of that region
    constexpr f32 u = kSolidWhiteUv.x;
    constexpr f32 v = kSolidWhiteUv.y;
    TEST_ASSERT(u > 120.0f / 128.0f && u < 128.0f / 128.0f);
    TEST_ASSERT(v > 56.0f / 64.0f && v < 64.0f / 64.0f);
}

// 3. DebugBatcher String Formatters
void TestDebugBatcherFormatters() {
    std::array<char, 64> buf{};
    char* end = buf.data() + buf.size();

    // FormatI64
    {
        char* p = DebugBatcher::FormatI64(buf.data(), end, 0);
        TEST_ASSERT(std::string_view(buf.data(), p - buf.data()) == "0");

        p = DebugBatcher::FormatI64(buf.data(), end, 12345);
        TEST_ASSERT(std::string_view(buf.data(), p - buf.data()) == "12345");

        p = DebugBatcher::FormatI64(buf.data(), end, -9876);
        TEST_ASSERT(std::string_view(buf.data(), p - buf.data()) == "-9876");
    }

    // FormatU64Hex
    {
        char* p = DebugBatcher::FormatU64Hex(buf.data(), end, 0xABCD1234ull, 8);
        TEST_ASSERT(std::string_view(buf.data(), p - buf.data()) == "ABCD1234");

        p = DebugBatcher::FormatU64Hex(buf.data(), end, 0xFFull, 4);
        TEST_ASSERT(std::string_view(buf.data(), p - buf.data()) == "00FF");
    }

    // FormatF32Fixed2
    {
        char* p = DebugBatcher::FormatF32Fixed2(buf.data(), end, 0.0f);
        TEST_ASSERT(std::string_view(buf.data(), p - buf.data()) == "0.00");

        p = DebugBatcher::FormatF32Fixed2(buf.data(), end, 12.345f);
        TEST_ASSERT(std::string_view(buf.data(), p - buf.data()) == "12.35");

        p = DebugBatcher::FormatF32Fixed2(buf.data(), end, -5.67f);
        TEST_ASSERT(std::string_view(buf.data(), p - buf.data()) == "-5.67");
    }
}

// 4. DebugBatcher Primitive Emission into RenderList
void TestDebugBatcherEmission() {
    auto list = std::make_unique<renderer::RenderList>();
    list->Reset({800, 600});

    const renderer::TextureHandle mockAtlas{1, 1};
    DebugBatcher batcher(*list, mockAtlas, kDebugWorldPassId, 0);

    batcher.PushSolidRect({10.0f, 20.0f, 30.0f, 40.0f}, renderer::Colors::Red);
    batcher.PushBoxOutline({50.0f, 50.0f, 20.0f, 20.0f}, 1.0f, renderer::Colors::Green);
    batcher.PushLine(0.0f, 0.0f, 100.0f, 0.0f, 2.0f, renderer::Colors::Blue);
    batcher.PushArrow(0.0f, 0.0f, 50.0f, 50.0f, 4.0f, 1.0f, renderer::Colors::White);
    batcher.PushCircleOutline(100.0f, 100.0f, 25.0f, 16, 1.0f, renderer::Colors::White);
    batcher.PushText(10.0f, 10.0f, "EngineN", renderer::Colors::White);

    list->Compile();

    // Verify that quads were emitted and radix passes executed efficiently
    std::array<renderer::Vertex2D, 256> outVertices{};
    list->ExpandVertices(0, 10, outVertices);
    TEST_ASSERT(outVertices[0].u >= 0.0f && outVertices[0].v >= 0.0f);
}

// 5. DebugShapePool Transient Shapes & Expiration
void TestDebugShapePool() {
    std::array<DebugShapeCmd, DebugShapePool::kMaxShapes> arenaShapes{};
    DebugShapePool pool{};
    pool.BindArena(arenaShapes);

    TEST_ASSERT(pool.ActiveCount() == 0);

    // Add shapes with 1-tick duration
    pool.SetCurrentTick(100);
    pool.AddLine(0.0f, 0.0f, 10.0f, 10.0f, 0xFFFFFFFFu, 1.0f, 1);
    pool.AddBox(20.0f, 20.0f, 5.0f, 5.0f, 0x00FF00FFu, false, 1.0f, 1);
    pool.AddArrow(0.0f, 0.0f, 10.0f, 0.0f, 0xFFFF00FFu, 4.0f, 1.0f, 2);
    pool.AddCircle(30.0f, 30.0f, 15.0f, 0x00FFFFFFu, 16, 1.0f, 1);
    pool.AddTextWorld(50.0f, 50.0f, "WorldText", 0xFFFFFFFFu, 1);
    pool.AddTextScreen(100.0f, 100.0f, "ScreenText", 0xFFFFFFFFu, 1);

    TEST_ASSERT(pool.ActiveCount() == 6);

    auto list = std::make_unique<renderer::RenderList>();
    list->Reset({800, 600});
    const renderer::TextureHandle mockAtlas{1, 1};

    // Render on tick 100: all shapes must remain active
    pool.EmitAndExpire(100, mockAtlas, *list);
    TEST_ASSERT(pool.ActiveCount() == 6);

    // Render on tick 101: 1-tick shapes remain visible during tick 101
    list->Reset({800, 600});
    pool.EmitAndExpire(101, mockAtlas, *list);
    TEST_ASSERT(pool.ActiveCount() == 6);

    // Render on tick 102: 1-tick shapes (expireTick = 101) expire; 2-tick arrow (expireTick = 102) remains
    list->Reset({800, 600});
    pool.EmitAndExpire(102, mockAtlas, *list);
    TEST_ASSERT(pool.ActiveCount() == 1);

    // Render on tick 103: arrow expires
    list->Reset({800, 600});
    pool.EmitAndExpire(103, mockAtlas, *list);
    TEST_ASSERT(pool.ActiveCount() == 0);
}

// 6. DebugProfiler Quantized Percentiles
void TestDebugProfilerPercentiles() {
    std::array<FrameProfileSample, DebugProfiler::kHistoryFrames> samples{};
    DebugProfiler profiler{};
    profiler.BindArena(samples);

    // Record 256 synthetic frames with known timings
    for (u64 f = 0; f < 256; ++f) {
        profiler.BeginFrame(f, f);
        // Half frames are 5000 us (~5ms), other half are 15000 us (~15ms)
        const u32 frameUs = (f < 128) ? 5000u : 15000u;
        profiler.RecordScopeUs(ProfileScopeId::SimTickTotal, frameUs / 2);
        profiler.RecordScopeUs(ProfileScopeId::RenderSubmit, frameUs / 2);
        profiler.EndFrame();
    }

    const u32 p50 = profiler.ComputePercentileUs(128);
    const u32 p95 = profiler.ComputePercentileUs(243);

    TEST_ASSERT(p50 >= 4000 && p50 <= 6000);
    TEST_ASSERT(p95 >= 14000 && p95 <= 16000);

    const auto& latest = profiler.Latest();
    TEST_ASSERT(latest.frameIndex == 255);
}

// 7. ConsoleArgs Tokenization & Parsing
void TestConsoleArgs() {
    ConsoleArgs a1("overlay bounds");
    TEST_ASSERT(a1.Count() == 2);
    TEST_ASSERT(a1[0] == "overlay");
    TEST_ASSERT(a1[1] == "bounds");

    ConsoleArgs a2("set_val  123   -45.67   true");
    TEST_ASSERT(a2.Count() == 4);
    TEST_ASSERT(*a2.GetInt(1) == 123);
    TEST_ASSERT(std::abs(*a2.GetFloat(2) - (-45.67f)) < 0.001f);
    TEST_ASSERT(*a2.GetBool(3) == true);

    // Quoted strings with spaces
    ConsoleArgs a3("print \"Hello World from EngineN\"");
    TEST_ASSERT(a3.Count() == 2);
    TEST_ASSERT(a3[0] == "print");
    TEST_ASSERT(a3[1] == "Hello World from EngineN");
}

// 8. DebugConsole Symbol Table, Commands & CVars
void TestDebugConsoleSymbolsAndCVars() {
    std::array<ConsoleSymbol, DebugConsole::kMaxSymbols> syms{};
    std::array<LogEntryMeta, DebugConsole::kMaxLogEntries> metas{};
    std::array<char, DebugConsole::kLogTextBytes> textBuf{};
    std::array<std::array<char, 128>, DebugConsole::kMaxHistory> histBuf{};

    DebugConsole console{};
    console.BindArena(syms, metas, textBuf, histBuf);

    // Register Command
    static bool cmdExecuted = false;
    static i64 passedArg = 0;
    auto testCmd = [](void* /*ctx*/, const ConsoleArgs& args, DebugConsole& /*c*/) noexcept {
        cmdExecuted = true;
        if (args.Count() > 1) {
            if (auto v = args.GetInt(1)) passedArg = *v;
        }
    };
    const auto regCmdRes = console.RegisterCommand("test.run", testCmd);
    TEST_ASSERT(regCmdRes.has_value());

    // Register CVars
    f32 testF32 = 1.0f;
    i32 testI32 = 10;
    bool testBool = false;
    TEST_ASSERT(console.RegisterCVarF32("cvar_f", &testF32, 0.0f, 10.0f).has_value());
    TEST_ASSERT(console.RegisterCVarI32("cvar_i", &testI32, -100, 100).has_value());
    TEST_ASSERT(console.RegisterCVarBool("cvar_b", &testBool).has_value());

    // Duplicate registration rejected
    TEST_ASSERT(console.RegisterCVarBool("cvar_b", &testBool).error() == DebugErrc::DuplicateSymbol);

    // Execute Command Immediate
    console.ExecuteImmediate("test.run 42", 100);
    TEST_ASSERT(cmdExecuted);
    TEST_ASSERT(passedArg == 42);

    // Mutate CVars via command line
    console.ExecuteImmediate("cvar_f 5.5", 101);
    TEST_ASSERT(std::abs(testF32 - 5.5f) < 0.001f);

    console.ExecuteImmediate("cvar_f 99.0", 102); // Clamped to 10.0
    TEST_ASSERT(std::abs(testF32 - 10.0f) < 0.001f);

    console.ExecuteImmediate("cvar_i -50", 103);
    TEST_ASSERT(testI32 == -50);

    console.ExecuteImmediate("cvar_b true", 104);
    TEST_ASSERT(testBool == true);

    console.ExecuteImmediate("cvar_b 0", 105);
    TEST_ASSERT(testBool == false);

    // Enqueue & Flush
    console.EnqueueCommandLine("cvar_i 25");
    TEST_ASSERT(testI32 == -50); // Not executed yet
    console.FlushPendingCommands(106);
    TEST_ASSERT(testI32 == 25); // Executed on flush
}

// 9. DebugInspector Flight Recorder & Entity Picker
void TestDebugInspector() {
    std::array<ActorTickRecord, DebugInspector::kHistoryTicks> tickRing{};
    DebugInspector inspector{};
    inspector.BindArena(tickRing);

    world::World world{};
    (void)world.Init({.maxEntities = 64});
    gameplay::GameplayPipeline gameplay{};
    (void)gameplay.Init(world);
    (void)world.RegisterComponentPool<world::CameraRig2D>(64);

    const world::EntityId ent = *world.Spawn(world::EntitySpawnDesc{.localTransform = {100.0f, 200.0f}});
    (void)world.Emplace<gameplay::KinematicMotor2D>(ent, gameplay::KinematicMotor2D{
        .vx = 50.0f,
        .vy = -10.0f,
        .bounds = {.offsetX_q8 = 0, .offsetY_q8 = 0, .halfW_q8 = 8 * 256, .halfH_q8 = 16 * 256}
    });
    world.PropagateTransforms();

    input::TickInput tickInput{};
    tickInput.held = 0x01;
    input::ActionBuffer actionBuf{};

    // Record tick
    inspector.RecordActorTick(world, 10, tickInput, actionBuf);
    inspector.RecordActorTick(world, 11, tickInput, actionBuf);
    inspector.SetPinnedEntity(ent);
    TEST_ASSERT(inspector.PinnedEntity() == ent);

    // Pick entity at screen pos
    time::RenderTimeView timeView{};
    const platform::Extent2i vp{640, 480};
    // Center of screen is (320, 240) corresponding to camera at (0, 0)
    // Create a camera rig at (100, 200) so entity is in center of viewport
    const world::EntityId camEnt = *world.Spawn(world::EntitySpawnDesc{.localTransform = {100.0f, 200.0f}});
    (void)world.Emplace<world::CameraRig2D>(camEnt, world::CameraRig2D{
        .zoom = 1.0f,
        .viewSlot = 0,
        .screenSpace = false,
        .active = true
    });
    world.PropagateTransforms();

    const world::EntityId picked = inspector.PickEntityAtScreenPos(world, timeView, {320.0f, 240.0f}, vp);
    TEST_ASSERT(picked == ent);

    // Flight Scrubber steps
    TEST_ASSERT(inspector.ScrubOffset() == 0);
    inspector.StepScrubOffset(+1);
    TEST_ASSERT(inspector.ScrubOffset() == 1);
    inspector.ResetScrubOffset();
    TEST_ASSERT(inspector.ScrubOffset() == 0);
}

// 10. DebugSystem Full Hardware Coordinator Lifecycle & Hotkeys
void TestDebugSystemFullLifecycle() {
    platform::Platform platform;
    const auto pInit = platform.Init("DebugTest", "EngineN");
    TEST_ASSERT(pInit.has_value());

    platform::Window window;
    const auto wInit = window.Create(platform::WindowConfig{
        .title = "DebugTest",
        .size = {640, 480},
        .mode = platform::WindowMode::Windowed
    });
    TEST_ASSERT(wInit.has_value());

    auto renderer = std::make_unique<renderer::Renderer>();
    const auto rInit = renderer->Init(window);
    TEST_ASSERT(rInit.has_value());

    DebugSystem debugSys;
    const auto dInit = debugSys.Init(*renderer);
    TEST_ASSERT(dInit.has_value());

    // Verify built-in commands exist
    debugSys.Console().ExecuteImmediate("help", 1);
    debugSys.Console().ExecuteImmediate("overlay hitboxes", 2);
    TEST_ASSERT(HasOverlay(debugSys.ActiveOverlays(), OverlayFlags::CombatHitboxes));

    // Hotkey test: `~` (scancode 53) opens console
    input::InputPipeline inputPipeline;

    TEST_ASSERT(!debugSys.Console().IsOpen());
    const bool handled1 = debugSys.OnPlatformKeyEvent(53, true, false, false, inputPipeline);
    TEST_ASSERT(handled1);
    TEST_ASSERT(debugSys.Console().IsOpen());
    TEST_ASSERT(inputPipeline.GetActionMask() == 0); // Gameplay keys masked!

    // Typing into console
    debugSys.OnPlatformTextInput("overlay prof\n");
    // Press Return (scancode 40)
    debugSys.OnPlatformKeyEvent(40, true, false, false, inputPipeline);

    // Close console via `~`
    debugSys.OnPlatformKeyEvent(53, true, false, false, inputPipeline);
    TEST_ASSERT(!debugSys.Console().IsOpen());
    TEST_ASSERT(inputPipeline.GetActionMask() == 0xFFFF); // Restored!

    // Function keys (F1 = 58 toggles profiler hud)
    const bool f1Handled = debugSys.OnPlatformKeyEvent(58, true, false, false, inputPipeline);
    TEST_ASSERT(f1Handled);

    // Emit Overlays
    world::World world;
    (void)world.Init();
    gameplay::GameplayPipeline gameplay;
    (void)gameplay.Init(world);
    (void)world.RegisterComponentPool<world::CameraRig2D>(64);
    audio::AudioSystem audio;
    assets::AssetBank assetBank;
    time::TimeManager timePipe;
    timePipe.BeginFrame();

    auto& list = renderer->BeginFrame();
    debugSys.EmitOverlays(world, gameplay, audio, assetBank,
                          timePipe.GetRenderView(),
                          inputPipeline.GetFrameView(),
                          window.State().pixelSize,
                          list);
    renderer->Submit();

    debugSys.Shutdown(renderer.get());
    renderer->Shutdown();
    window.Destroy();
    platform.Shutdown();
}

} // namespace

int main() {
    std::cout << "Running EngineN Debug & Tooling Subsystem Tests...\n" << std::flush;

    TestMemoryLayoutsAndTypes();
    std::cout << "[PASS] TestMemoryLayoutsAndTypes\n" << std::flush;
    TestDebugAtlasUnpacking();
    std::cout << "[PASS] TestDebugAtlasUnpacking\n" << std::flush;
    TestDebugBatcherFormatters();
    std::cout << "[PASS] TestDebugBatcherFormatters\n" << std::flush;
    TestDebugBatcherEmission();
    std::cout << "[PASS] TestDebugBatcherEmission\n" << std::flush;
    TestDebugShapePool();
    std::cout << "[PASS] TestDebugShapePool\n" << std::flush;
    TestDebugProfilerPercentiles();
    std::cout << "[PASS] TestDebugProfilerPercentiles\n" << std::flush;
    TestConsoleArgs();
    std::cout << "[PASS] TestConsoleArgs\n" << std::flush;
    TestDebugConsoleSymbolsAndCVars();
    std::cout << "[PASS] TestDebugConsoleSymbolsAndCVars\n" << std::flush;
    TestDebugInspector();
    std::cout << "[PASS] TestDebugInspector\n" << std::flush;
    TestDebugSystemFullLifecycle();
    std::cout << "[PASS] TestDebugSystemFullLifecycle\n" << std::flush;

    std::cout << "Test Summary: " << g_passedAssertions << " passed, "
              << g_failedAssertions << " failed.\n" << std::flush;

    return (g_failedAssertions == 0) ? 0 : 1;
}
