#include "platform/Platform.h"
#include "platform/Window.h"
#include "platform/EventPump.h"
#include "platform/input/InputSystem.h"
#include "core/Log.h"

#include <SDL3/SDL.h>
#include <cassert>
#include <iostream>
#include <vector>
#include <string_view>

using namespace EngineN;

void TestPlatformAndWindow() {
    std::cout << "  -> Testing Platform and Window Subsystem..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);
    assert(platform.IsInitialized());

    // Query displays
    i32 displayCount = platform.GetDisplayCount();
    assert(displayCount >= 1);

    DisplayInfo primary = platform.GetPrimaryDisplayInfo();
    assert(primary.width > 0);
    assert(primary.height > 0);
    assert(primary.refreshRate > 0.0f);
    assert(!primary.name.empty());

    // Create window
    Window window;
    WindowConfig config{
        .title = "Test Window",
        .width = 800,
        .height = 600,
        .mode = WindowMode::Windowed,
        .highDPI = true,
        .resizable = true
    };

    auto winRes = window.Create(config);
    assert(winRes);
    assert(window.IsValid());
    assert(window.GetWidth() == 800);
    assert(window.GetHeight() == 600);
    assert(window.GetPixelWidth() >= 800);
    assert(window.GetPixelHeight() >= 600);
    assert(window.GetDPIScale() >= 1.0f);
    assert(window.GetMode() == WindowMode::Windowed);

    // Toggle mode
    auto modeRes = window.SetMode(WindowMode::BorderlessFullscreen);
    assert(modeRes);
    assert(window.GetMode() == WindowMode::BorderlessFullscreen);

    modeRes = window.SetMode(WindowMode::Windowed);
    assert(modeRes);
    assert(window.GetMode() == WindowMode::Windowed);

    // Event pump quit test
    EventPump pump;
    InputSystem input;
    pump.BeginFrame();

    // Synthesize quit event
    SDL_Event quitEvent{};
    quitEvent.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quitEvent);

    bool quitResult = pump.PollAll(window, input);
    assert(quitResult == true);
    assert(pump.WasQuitRequested() == true);

    window.Destroy();
    assert(!window.IsValid());

    platform.Shutdown();
    assert(!platform.IsInitialized());

    std::cout << "     [PASS] Platform and Window Subsystem" << std::endl;
}

void TestClipboardRAII() {
    std::cout << "  -> Testing Clipboard RAII & Zero-Leak Safety..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);

    const char* testStr = "EngineN_Zero_Leak_RAII_Test_2026";
    bool setOk = platform.SetClipboardText(testStr);
    assert(setOk);
    assert(platform.HasClipboardText());

    // RAII query
    {
        ClipboardText clip = platform.GetClipboardText();
        assert(!clip.empty());
        assert(clip.view() == testStr);
        assert(clip.length() == std::string_view(testStr).length());
        assert(clip == testStr);
        assert(clip == std::string_view(testStr));
        assert(std::string_view(clip) == testStr);

        // Move construction
        ClipboardText moved(std::move(clip));
        assert(clip.empty());
        assert(clip.c_str()[0] == '\0');
        assert(moved.view() == testStr);

        // Move assignment
        ClipboardText moved2;
        moved2 = std::move(moved);
        assert(moved.empty());
        assert(moved2.view() == testStr);

        // Explicit reset
        moved2.Reset();
        assert(moved2.empty());
    } // clip, moved, moved2 go out of scope, guaranteeing 0 leaks

    // std::string convenience query
    std::string clipStr = platform.GetClipboardString();
    assert(clipStr == testStr);

    platform.Shutdown();
    std::cout << "     [PASS] Clipboard RAII & Zero-Leak Safety" << std::endl;
}

void TestDisplayQueriesAndDiagnostics() {
    std::cout << "  -> Testing Display Queries & Native Content Scaling..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);

    i32 count = platform.GetDisplayCount();
    assert(count >= 1);

    DisplayInfo primary = platform.GetPrimaryDisplayInfo();
    assert(primary.id != 0);
    assert(primary.width > 0);
    assert(primary.height > 0);
    assert(primary.refreshRate > 0.0f);
    assert(primary.contentScale >= 1.0f);
    assert(primary.hdpi >= 96.0f);
    assert(primary.vdpi >= 96.0f);
    // Verify memory safety: owned std::string name outlives any scope
    assert(!primary.name.empty());

    std::vector<DisplayInfo> allDisplays = platform.GetAllDisplays();
    assert(!allDisplays.empty());
    assert(allDisplays.size() == static_cast<size_t>(count));

    assert(platform.GetDisplayRefreshRate() > 0.0f);
    assert(platform.GetDisplayContentScale() >= 1.0f);

    // Diagnostics
    assert(platform.GetCPUCount() > 0);
    assert(platform.GetSystemRAM() > 0);
    assert(platform.GetCPUCacheLineSize() > 0);
    PowerInfo power = platform.GetPowerInfo();
    (void)power;
    assert(platform.GetPerformanceCounter() > 0);
    assert(platform.GetPerformanceFrequency() > 0);

    platform.Shutdown();
    std::cout << "     [PASS] Display Queries & Native Content Scaling" << std::endl;
}

void TestWindowModeTogglesAndScaling() {
    std::cout << "  -> Testing Window Mode Toggles & Display Synchronization..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);

    Window window;
    WindowConfig config{
        .title = "Mode Toggle Window",
        .width = 640,
        .height = 480,
        .mode = WindowMode::Windowed,
        .highDPI = true,
        .resizable = true
    };
    auto winRes = window.Create(config);
    assert(winRes);
    assert(window.IsValid());
    assert(window.GetMode() == WindowMode::Windowed);
    assert(!window.IsFullscreen());
    assert(window.GetFullscreenMode() == nullptr);
    assert(window.GetDisplayScale() >= 1.0f);
    assert(window.GetDPIScale() >= 1.0f);
    assert(window.GetPixelDensity() >= 1.0f);
    assert(window.GetRefreshRate() > 0.0f);
    assert(window.GetDisplayID() != 0);

    // Toggle BorderlessFullscreen
    auto borderlessRes = window.SetMode(WindowMode::BorderlessFullscreen);
    assert(borderlessRes);
    assert(window.GetMode() == WindowMode::BorderlessFullscreen);
    assert(window.IsFullscreen());
    // In SDL3, borderless fullscreen desktop uses mode == nullptr
    assert(window.GetFullscreenMode() == nullptr);

    // Toggle ExclusiveFullscreen
    auto exclusiveRes = window.SetMode(WindowMode::ExclusiveFullscreen);
    assert(exclusiveRes);
    assert(window.GetMode() == WindowMode::ExclusiveFullscreen);
    assert(window.IsFullscreen());

    // Toggle back to Windowed
    auto windowedRes = window.SetMode(WindowMode::Windowed);
    assert(windowedRes);
    assert(window.GetMode() == WindowMode::Windowed);
    assert(!window.IsFullscreen());

    window.Destroy();
    assert(!window.IsValid());

    platform.Shutdown();
    std::cout << "     [PASS] Window Mode Toggles & Display Synchronization" << std::endl;
}

void TestDecoupledEventPumpAndRouting() {
    std::cout << "  -> Testing Decoupled Event Routing & Modular Filtering..." << std::endl;

    Platform platform;
    auto initRes = platform.Init();
    assert(initRes);

    struct MockAudioListener : public IEventListener {
        u32 audioEventCount = 0;
        void ProcessEvent(const SDL_Event& e) override {
            if (e.type == SDL_EVENT_AUDIO_DEVICE_ADDED) {
                audioEventCount++;
            }
        }
    };

    struct MockInputListener : public IEventListener {
        u32 inputEventCount = 0;
        void ProcessEvent(const SDL_Event& e) override {
            if (e.type == SDL_EVENT_KEY_DOWN) {
                inputEventCount++;
            }
        }
    };

    EventPump pump;
    MockAudioListener audioListener;
    MockInputListener inputListener;

    // Register with category masks
    pump.AddListener(&audioListener, EventCategory::Audio);
    pump.AddListener(&inputListener, EventCategory::Keyboard);

    assert(pump.HasListener(&audioListener));
    assert(pump.HasListener(&inputListener));

    // Register a lambda callback for system events
    bool quitCallbackInvoked = false;
    u32 cbId = pump.AddCallback([&](const SDL_Event& e) {
        if (e.type == SDL_EVENT_QUIT) {
            quitCallbackInvoked = true;
        }
    }, EventCategory::System);
    assert(cbId > 0);

    // 1. Synthesize and push an audio event
    pump.BeginFrame();
    SDL_Event audioEv{};
    audioEv.type = SDL_EVENT_AUDIO_DEVICE_ADDED;
    SDL_PushEvent(&audioEv);

    // 2. Synthesize and push a keyboard event
    SDL_Event keyEv{};
    keyEv.type = SDL_EVENT_KEY_DOWN;
    keyEv.key.scancode = SDL_SCANCODE_SPACE;
    SDL_PushEvent(&keyEv);

    // 3. Synthesize and push a quit event
    SDL_Event quitEv{};
    quitEv.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quitEv);

    // Drain all events via decoupled Poll()
    bool quitRequested = pump.Poll();
    assert(quitRequested == true);
    assert(pump.WasQuitRequested() == true);

    // Verify targeted routing according to category mask:
    assert(audioListener.audioEventCount == 1);
    assert(inputListener.inputEventCount == 1);
    assert(quitCallbackInvoked == true);

    // 4. Test category filtering isolation:
    // Reset listener counts
    audioListener.audioEventCount = 0;
    inputListener.inputEventCount = 0;

    // Push ONLY a keyboard event
    SDL_PushEvent(&keyEv);
    pump.Poll();

    // Audio listener must NOT have received keyboard event!
    assert(audioListener.audioEventCount == 0);
    assert(inputListener.inputEventCount == 1);

    // 5. Test unregistering listener and callback
    pump.RemoveListener(&audioListener);
    assert(!pump.HasListener(&audioListener));

    bool removeCbOk = pump.RemoveCallback(cbId);
    assert(removeCbOk == true);

    platform.Shutdown();
    std::cout << "     [PASS] Decoupled Event Routing & Modular Filtering" << std::endl;
}

int main() {
    std::cout << "[TestPlatform] Running platform subsystem tests..." << std::endl;

    TestPlatformAndWindow();
    TestClipboardRAII();
    TestDisplayQueriesAndDiagnostics();
    TestWindowModeTogglesAndScaling();
    TestDecoupledEventPumpAndRouting();

    std::cout << "[TestPlatform] All tests passed." << std::endl;
    return 0;
}
