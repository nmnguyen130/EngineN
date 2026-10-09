#include "platform/Platform.h"
#include "platform/Window.h"
#include "platform/EventPump.h"
#include "platform/input/Gamepad.h"
#include "platform/input/InputSystem.h"
#include "core/Log.h"

#include <SDL3/SDL.h>
#include <cassert>
#include <iostream>
#include <cmath>
#include <vector>
#include <string_view>

using namespace EngineN;

void TestDeadzoneMath() {
    std::cout << "  -> Testing Deadzone Math..." << std::endl;

    f32 deadzone = 0.22f;

    // Inside deadzone -> must return exactly {0, 0}
    DeadzoneResult inside = ApplyRadialDeadzone(0.1f, 0.1f, deadzone);
    assert(inside.x == 0.0f && inside.y == 0.0f);

    DeadzoneResult zero = ApplyRadialDeadzone(0.0f, 0.0f, deadzone);
    assert(zero.x == 0.0f && zero.y == 0.0f);

    // Right on or outside deadzone
    DeadzoneResult edge = ApplyRadialDeadzone(1.0f, 0.0f, deadzone);
    assert(std::abs(edge.x - 1.0f) < 1e-5f);
    assert(std::abs(edge.y - 0.0f) < 1e-5f);

    // Scaled intermediate value
    DeadzoneResult intermediate = ApplyRadialDeadzone(0.5f, 0.0f, deadzone);
    f32 expectedScale = (0.5f - deadzone) / (1.0f - deadzone);
    assert(std::abs(intermediate.x - expectedScale) < 1e-5f);
    assert(intermediate.y == 0.0f);

    // Axial deadzone
    assert(ApplyAxialDeadzone(0.1f, deadzone) == 0.0f);
    assert(ApplyAxialDeadzone(-0.1f, deadzone) == 0.0f);
    assert(std::abs(ApplyAxialDeadzone(1.0f, deadzone) - 1.0f) < 1e-5f);
    assert(std::abs(ApplyAxialDeadzone(-1.0f, deadzone) - (-1.0f)) < 1e-5f);

    std::cout << "     [PASS] Deadzone Math" << std::endl;
}

void TestActionStateTransitions() {
    std::cout << "  -> Testing Action State Transitions..." << std::endl;

    InputSystem input;
    assert(input.GetActionState(PlayerAction::Jump) == ActionState::Idle);

    // Frame 1: Press Space
    input.BeginFrame();
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 1000ULL;
    e.key.repeat = 0;
    input.ProcessEvent(e);
    input.EndFrame();

    assert(input.IsActionJustPressed(PlayerAction::Jump));
    assert(input.IsActionHeld(PlayerAction::Jump));
    assert(!input.IsActionJustReleased(PlayerAction::Jump));

    // Frame 2: Still holding Space
    input.BeginFrame();
    input.EndFrame();

    assert(!input.IsActionJustPressed(PlayerAction::Jump));
    assert(input.IsActionHeld(PlayerAction::Jump));
    assert(input.GetActionState(PlayerAction::Jump) == ActionState::Held);

    // Frame 3: Release Space
    input.BeginFrame();
    e.type = SDL_EVENT_KEY_UP;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 2000ULL;
    input.ProcessEvent(e);
    input.EndFrame();

    assert(!input.IsActionJustPressed(PlayerAction::Jump));
    assert(!input.IsActionHeld(PlayerAction::Jump));
    assert(input.IsActionJustReleased(PlayerAction::Jump));

    // Frame 4: Return to Idle
    input.BeginFrame();
    input.EndFrame();

    assert(input.GetActionState(PlayerAction::Jump) == ActionState::Idle);

    std::cout << "     [PASS] Action State Transitions" << std::endl;
}

void TestTimestampedActionBuffer() {
    std::cout << "  -> Testing Timestamped Action Buffer..." << std::endl;

    InputSystem input;
    input.BeginFrame();

    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 12345ULL;
    e.key.repeat = 0;
    input.ProcessEvent(e);

    e.key.scancode = SDL_SCANCODE_J; // Attack
    e.key.timestamp = 23456ULL;
    input.ProcessEvent(e);

    assert(input.GetTimestampedActionCount() == 2);
    assert(input.GetTimestampedAction(0).action == PlayerAction::Jump);
    assert(input.GetTimestampedAction(0).timestamp == 12345ULL);
    assert(input.GetTimestampedAction(0).pressed == true);

    assert(input.GetTimestampedAction(1).action == PlayerAction::Attack);
    assert(input.GetTimestampedAction(1).timestamp == 23456ULL);
    assert(input.GetTimestampedAction(1).pressed == true);

    // Next frame resets buffer
    input.BeginFrame();
    assert(input.GetTimestampedActionCount() == 0);

    std::cout << "     [PASS] Timestamped Action Buffer" << std::endl;
}

void TestMultipleKeysSameAction() {
    std::cout << "  -> Testing Multiple Keys Bound to Same Action..." << std::endl;

    InputSystem input;
    // Both SPACE and UP are bound to Jump by default

    // Frame 1: Press SPACE
    input.BeginFrame();
    SDL_Event e1{};
    e1.type = SDL_EVENT_KEY_DOWN;
    e1.key.scancode = SDL_SCANCODE_SPACE;
    e1.key.timestamp = 100ULL;
    input.ProcessEvent(e1);
    assert(input.IsActionJustPressed(PlayerAction::Jump));

    // Frame 2: While holding SPACE, press UP
    input.BeginFrame();
    SDL_Event e2{};
    e2.type = SDL_EVENT_KEY_DOWN;
    e2.key.scancode = SDL_SCANCODE_UP;
    e2.key.timestamp = 200ULL;
    input.ProcessEvent(e2);
    assert(input.IsActionHeld(PlayerAction::Jump));
    // Since Jump was already active, it remains Held (not second JustPressed)
    assert(!input.IsActionJustPressed(PlayerAction::Jump));

    // Frame 3: Release SPACE (UP is still held)
    input.BeginFrame();
    e1.type = SDL_EVENT_KEY_UP;
    e1.key.scancode = SDL_SCANCODE_SPACE;
    e1.key.timestamp = 300ULL;
    input.ProcessEvent(e1);
    // Should NOT be released because UP is still holding it!
    assert(input.IsActionHeld(PlayerAction::Jump));
    assert(!input.IsActionJustReleased(PlayerAction::Jump));

    // Frame 4: Release UP
    input.BeginFrame();
    e2.type = SDL_EVENT_KEY_UP;
    e2.key.scancode = SDL_SCANCODE_UP;
    e2.key.timestamp = 400ULL;
    input.ProcessEvent(e2);
    // Now all physical inputs are released -> JustReleased!
    assert(input.IsActionJustReleased(PlayerAction::Jump));

    std::cout << "     [PASS] Multiple Keys Bound to Same Action" << std::endl;
}

void TestGamepadOpenClose() {
    std::cout << "  -> Testing Gamepad Lifecycle & Move Semantics..." << std::endl;

    Gamepad pad;
    assert(!pad.IsConnected());
    assert(pad.GetRawHandle() == nullptr);

    // Safe to close an un-opened gamepad
    pad.Close();
    assert(!pad.IsConnected());

    // Move construction
    Gamepad moved(std::move(pad));
    assert(!moved.IsConnected());

    std::cout << "     [PASS] Gamepad Lifecycle & Move Semantics" << std::endl;
}

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

void TestMetroidvaniaActionBuffering() {
    std::cout << "  -> Testing Metroidvania Action Buffering & Forgiveness..." << std::endl;

    InputSystem input;

    // 1. Jump Buffering across multiple frames before landing
    // Frame 1 at t = 10ms (10,000,000 ns): Player presses Jump in mid-air
    input.BeginFrame();
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 10'000'000ULL;
    e.key.repeat = 0;
    input.ProcessEvent(e);
    input.EndFrame();

    assert(input.IsActionJustPressed(PlayerAction::Jump));
    assert(input.HasBufferedAction(PlayerAction::Jump, 10'000'000ULL, kJumpBufferWindowNs));

    // Frame 2 at t = 20ms: Mid-air tick
    input.BeginFrame();
    input.EndFrame();
    // In frame 2, JustPressed transitioned to Held
    assert(!input.IsActionJustPressed(PlayerAction::Jump));
    // BUT the buffered intent is still active!
    assert(input.HasBufferedAction(PlayerAction::Jump, 20'000'000ULL, kJumpBufferWindowNs));

    // Frame 5 at t = 60ms (50ms after press): Character touches ground!
    input.BeginFrame();
    u64 landTimeNs = 60'000'000ULL;
    // Ground detection consumes buffered jump intent within 100ms window
    bool consumed = input.ConsumeAction(PlayerAction::Jump, landTimeNs, kJumpBufferWindowNs);
    assert(consumed == true);

    // Anti-ghosting: Cannot consume the same jump intent twice
    bool secondConsume = input.ConsumeAction(PlayerAction::Jump, landTimeNs, kJumpBufferWindowNs);
    assert(secondConsume == false);
    input.EndFrame();

    // 2. Buffer Window Expiration (Out of Window)
    // Player presses Attack at t = 100ms
    input.BeginFrame();
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_J;
    e.key.timestamp = 100'000'000ULL;
    e.key.repeat = 0;
    input.ProcessEvent(e);
    input.EndFrame();

    // Query 150ms later (t = 250ms), exceeding the 100ms attack buffer window
    u64 lateQueryNs = 250'000'000ULL;
    assert(!input.HasBufferedAction(PlayerAction::Attack, lateQueryNs, kAttackBufferWindowNs));
    assert(!input.ConsumeAction(PlayerAction::Attack, lateQueryNs, kAttackBufferWindowNs));

    // 3. Forgiveness Cancellation (Hitstun / Hurt state clears buffer)
    input.BeginFrame();
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_LSHIFT; // Dash
    e.key.timestamp = 300'000'000ULL;
    e.key.repeat = 0;
    input.ProcessEvent(e);
    input.EndFrame();

    assert(input.HasBufferedAction(PlayerAction::Dash, 310'000'000ULL, kDashBufferWindowNs));
    // Player gets hit by enemy hazard -> Clear action buffer!
    input.ClearActionBuffer();
    assert(!input.HasBufferedAction(PlayerAction::Dash, 310'000'000ULL, kDashBufferWindowNs));
    assert(!input.ConsumeAction(PlayerAction::Dash, 310'000'000ULL, kDashBufferWindowNs));

    // 4. Standalone ActionBuffer Ring Buffer Wrap-Around & Anti-Ghosting
    ActionBuffer buffer;
    assert(buffer.GetCount() == 0);

    // Record 70 actions (exceeding kMaxBufferedActions = 64)
    for (u64 i = 0; i < 70; ++i) {
        buffer.RecordAction(PlayerAction::Attack, i * 1'000'000ULL);
    }
    assert(buffer.GetCount() == kMaxBufferedActions);

    // Newest is at 69ms; consume with 100ms window at 70ms
    assert(buffer.ConsumeAction(PlayerAction::Attack, 70'000'000ULL, kAttackBufferWindowNs));
    // Anti-ghosting has invalidated all older Attack entries
    assert(!buffer.ConsumeAction(PlayerAction::Attack, 70'000'000ULL, kAttackBufferWindowNs));

    std::cout << "     [PASS] Metroidvania Action Buffering & Forgiveness" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "   ENGINEN PLATFORM SUBSYSTEM TEST SUITE          " << std::endl;
    std::cout << "   Modern C++20 | SDL3 Platform Abstraction       " << std::endl;
    std::cout << "==================================================" << std::endl;

    TestDeadzoneMath();
    TestActionStateTransitions();
    TestTimestampedActionBuffer();
    TestMultipleKeysSameAction();
    TestGamepadOpenClose();
    TestPlatformAndWindow();
    TestClipboardRAII();
    TestDisplayQueriesAndDiagnostics();
    TestWindowModeTogglesAndScaling();
    TestDecoupledEventPumpAndRouting();
    TestMetroidvaniaActionBuffering();

    std::cout << "==================================================" << std::endl;
    std::cout << "   ALL PLATFORM TESTS PASSED SUCCESSFULLY!        " << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
