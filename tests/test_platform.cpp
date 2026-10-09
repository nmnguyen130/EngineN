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

    std::cout << "==================================================" << std::endl;
    std::cout << "   ALL PLATFORM TESTS PASSED SUCCESSFULLY!        " << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
