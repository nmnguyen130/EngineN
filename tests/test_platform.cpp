#include "platform/Platform.h"
#include "platform/Window.h"
#include "platform/EventRouting.h"
#include "platform/input/InputSystem.h"
#include "core/Log.h"

#include <SDL3/SDL.h>
#include <cassert>
#include <iostream>

using namespace EngineN;

void TestPlatformLifecycleAndTopology() {
    std::cout << "  [1/5] Testing Platform Lifecycle & Display Topology..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);
    assert(platform.IsInitialized());

    const auto& topology = platform.Displays();
    assert(!topology.Displays().empty());

    const DisplayInfo* primary = topology.Primary();
    assert(primary != nullptr);
    assert(primary->bounds.w > 0);
    assert(primary->bounds.h > 0);
    assert(primary->contentScale >= 1.0f);
    assert(!primary->name.Empty());

    platform.Shutdown();
    assert(!platform.IsInitialized());
    std::cout << "        -> PASS" << std::endl;
}

void TestWindowLifecycleAndModes() {
    std::cout << "  [2/5] Testing Window Creation, Modes & Dirty Tracking..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);

    WindowConfig config{
        .title = "EngineN Production Test Window",
        .size = {1280, 720},
        .mode = WindowMode::Windowed
    };

    auto winRes = Window::Create(config, platform.Displays());
    assert(winRes);

    Window window = std::move(*winRes);
    assert(window.IsValid());
    assert(window.Native() != nullptr);
    assert(window.State().metrics.logicalSize.w == 1280);
    assert(window.State().metrics.logicalSize.h == 720);
    assert(window.State().mode == WindowMode::Windowed);

    // Initial dirty changes drained
    auto initChanges = window.TakeChanges();
    assert(initChanges != WindowChange::None);

    // Toggle BorderlessFullscreen
    auto modeRes = window.SetMode(WindowMode::BorderlessFullscreen);
    assert(modeRes);
    assert(window.State().mode == WindowMode::BorderlessFullscreen);

    auto modeChanges = window.TakeChanges();
    assert(HasAny(modeChanges, WindowChange::Mode));

    // Toggle back to Windowed
    modeRes = window.SetMode(WindowMode::Windowed);
    assert(modeRes);
    assert(window.State().mode == WindowMode::Windowed);

    window.Destroy();
    assert(!window.IsValid());

    platform.Shutdown();
    std::cout << "        -> PASS" << std::endl;
}

void TestSingleSourcePlatformPump() {
    std::cout << "  [3/5] Testing Single-Source Event Drainage & Dispatch..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);

    WindowConfig config{
        .title = "Pump Test Window",
        .size = {640, 480}
    };
    Window window;
    auto winRes = window.Create(config);
    assert(winRes);

    InputSystem input;

    // Synthesize SDL_EVENT_QUIT
    SDL_Event quitEv{};
    quitEv.type = SDL_EVENT_QUIT;
    quitEv.quit.timestamp = SDL_GetTicksNS();
    (void)SDL_PushEvent(&quitEv);

    // Unified single-source pump
    const auto& events = platform.Pump(window, input);
    assert(platform.WasQuitRequested());
    assert(!events.Empty());
    assert(events[0].type == PlatformEventType::QuitRequested);

    window.Destroy();
    platform.Shutdown();
    std::cout << "        -> PASS" << std::endl;
}

void TestPrioritizedEventRoutingAndSuppression() {
    std::cout << "  [4/5] Testing 4-Stage Routing & Anti-Sticking Suppression..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);

    // Mock DevTools overlay backend capturing keyboard at Stage 0
    struct MockDevToolsBackend {
        DevToolsCapture capture{};
        u32 processedEvents{0};

        void ProcessEvent(const SDL_Event&) noexcept {
            processedEvents++;
        }
        [[nodiscard]] DevToolsCapture Capture() const noexcept {
            return capture;
        }
    } devToolsBackend;

    DevToolsSink<MockDevToolsBackend> devToolsSink(devToolsBackend);
    assert(devToolsSink.Attach(platform.Router()));

    // Mock App Sink registered at Stage 3
    struct MockAppSink {
        u32 keyDownCount{0};
        u32 keyUpCount{0};

        SinkVerdict OnEvent(const SDL_Event& e) noexcept {
            if (e.type == SDL_EVENT_KEY_DOWN) keyDownCount++;
            if (e.type == SDL_EVENT_KEY_UP) keyUpCount++;
            return SinkVerdict::Continue;
        }
    } appSink;

    assert(platform.Router().Add(
        SinkStage::App,
        EventSinkRef::Bind<&MockAppSink::OnEvent>(appSink)));

    // Case 1: Uncaptured -> Key Down & Up both pass to App
    devToolsBackend.capture.wantsKeyboard = false;

    SDL_Event keyDown{};
    keyDown.type = SDL_EVENT_KEY_DOWN;
    keyDown.key.scancode = SDL_SCANCODE_SPACE;
    (void)SDL_PushEvent(&keyDown);

    SDL_Event keyUp{};
    keyUp.type = SDL_EVENT_KEY_UP;
    keyUp.key.scancode = SDL_SCANCODE_SPACE;
    (void)SDL_PushEvent(&keyUp);

    (void)platform.Pump();
    assert(appSink.keyDownCount == 1);
    assert(appSink.keyUpCount == 1);

    // Case 2: Captured -> Key Down is SUPPRESSED, Key Up ALWAYS passes through
    devToolsBackend.capture.wantsKeyboard = true;
    appSink.keyDownCount = 0;
    appSink.keyUpCount = 0;

    (void)SDL_PushEvent(&keyDown);
    (void)SDL_PushEvent(&keyUp);

    (void)platform.Pump();
    assert(appSink.keyDownCount == 0); // Suppressed by DevTools Stage 0
    assert(appSink.keyUpCount == 1);   // Anti-sticking rule: release passes

    platform.Shutdown();
    std::cout << "        -> PASS" << std::endl;
}

void TestEnvironmentServicesAndSandboxing() {
    std::cout << "  [5/5] Testing Services (Clipboard RAII & Path Sandbox)..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);

    // Clipboard RAII zero-leak
    const char* sampleText = "EngineN_Platform_Zero_Leak_2026";
    auto clipSetRes = platform.ClipboardService().SetText(sampleText);
    assert(clipSetRes);
    assert(platform.ClipboardService().HasText());

    {
        ClipboardText text = platform.ClipboardService().GetText();
        assert(!text.Empty());
        assert(text.View() == sampleText);

        ClipboardText moved(std::move(text));
        assert(text.Empty());
        assert(moved.View() == sampleText);
    }

    // Path sandbox
    auto validUser = platform.Paths().ResolveUser("saves/slot1.sav");
    assert(validUser);
    assert(validUser->View().find("saves/slot1.sav") != std::string_view::npos);

    auto validAsset = platform.Paths().ResolveAsset("textures/player.png");
    assert(validAsset);
    assert(validAsset->View().find("textures/player.png") != std::string_view::npos);

    // Security constraints: reject ".." traversal and absolute paths
    auto traversal = platform.Paths().ResolveUser("../../../secret.dat");
    assert(!traversal);
    assert(traversal.Error().code == static_cast<i32>(PlatformErrc::PathRejected));

    auto driveLetter = platform.Paths().ResolveUser("C:/Windows/cmd.exe");
    assert(!driveLetter);
    assert(driveLetter.Error().code == static_cast<i32>(PlatformErrc::PathRejected));

    // Diagnostics queries
    assert(platform.Specs().logicalCores > 0);
    assert(platform.Specs().ramMiB > 0);

    platform.Shutdown();
    std::cout << "        -> PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " EngineN — Platform Subsystem Test Suite " << std::endl;
    std::cout << "========================================" << std::endl;

    TestPlatformLifecycleAndTopology();
    TestWindowLifecycleAndModes();
    TestSingleSourcePlatformPump();
    TestPrioritizedEventRoutingAndSuppression();
    TestEnvironmentServicesAndSandboxing();

    std::cout << "========================================" << std::endl;
    std::cout << " [ALL TESTS PASSED] Platform is 100% OK " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
