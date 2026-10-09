#include "platform/input/InputTypes.h"
#include "platform/input/InputSystem.h"

#include <SDL3/SDL.h>
#include <cassert>
#include <iostream>
#include <cmath>
#include <limits>

using namespace EngineN;

void TestDeadzoneMath() {
    std::cout << "  -> Testing Deadzone Math & Validation..." << std::endl;

    const f32 deadzone = 0.22f;

    // Inside radial deadzone must map strictly to zero vector
    DeadzoneResult inside = ApplyRadialDeadzone(0.1f, 0.1f, deadzone);
    assert(inside.x == 0.0f && inside.y == 0.0f);

    DeadzoneResult zero = ApplyRadialDeadzone(0.0f, 0.0f, deadzone);
    assert(zero.x == 0.0f && zero.y == 0.0f);

    // Full deflection at boundary
    DeadzoneResult edge = ApplyRadialDeadzone(1.0f, 0.0f, deadzone);
    assert(std::abs(edge.x - 1.0f) < 1e-5f);
    assert(std::abs(edge.y - 0.0f) < 1e-5f);

    // Intermediate value smoothly re-scaled between deadzone and 1.0
    DeadzoneResult intermediate = ApplyRadialDeadzone(0.5f, 0.0f, deadzone);
    f32 expectedScale = (0.5f - deadzone) / (1.0f - deadzone);
    assert(std::abs(intermediate.x - expectedScale) < 1e-5f);
    assert(intermediate.y == 0.0f);

    // Defensive check: NaN or invalid deadzone must safely evaluate to zero
    DeadzoneResult nanResult = ApplyRadialDeadzone(0.5f, 0.5f, std::numeric_limits<f32>::quiet_NaN());
    assert(nanResult.x == 0.0f && nanResult.y == 0.0f);

    // 1D Axial deadzone
    assert(ApplyAxialDeadzone(0.1f, deadzone) == 0.0f);
    assert(ApplyAxialDeadzone(-0.1f, deadzone) == 0.0f);
    assert(std::abs(ApplyAxialDeadzone(1.0f, deadzone) - 1.0f) < 1e-5f);
    assert(std::abs(ApplyAxialDeadzone(-1.0f, deadzone) - (-1.0f)) < 1e-5f);

    std::cout << "     [PASS] Deadzone Math & Validation" << std::endl;
}

void TestSameFrameEdgeDetection() {
    std::cout << "  -> Testing Same-Frame Press and Release Detection..." << std::endl;

    InputSystem input;
    input.BeginFrame();

    // Synthesize rapid press and release within the same render frame
    SDL_Event e1{};
    e1.type = SDL_EVENT_KEY_DOWN;
    e1.key.scancode = SDL_SCANCODE_SPACE;
    e1.key.timestamp = 1000ULL;
    input.ProcessEvent(e1);

    SDL_Event e2{};
    e2.type = SDL_EVENT_KEY_UP;
    e2.key.scancode = SDL_SCANCODE_SPACE;
    e2.key.timestamp = 2000ULL;
    input.ProcessEvent(e2);

    // Both transitions must be latched for this frame, while down state returns false
    assert(input.WasActionPressedThisFrame(InputAction::Jump));
    assert(input.WasActionReleasedThisFrame(InputAction::Jump));
    assert(!input.IsActionDown(InputAction::Jump));

    // Next frame clears transient edges
    input.BeginFrame();
    assert(!input.WasActionPressedThisFrame(InputAction::Jump));
    assert(!input.WasActionReleasedThisFrame(InputAction::Jump));
    assert(!input.IsActionDown(InputAction::Jump));

    std::cout << "     [PASS] Same-Frame Press and Release Detection" << std::endl;
}

void TestFixedTickEdgeLatching() {
    std::cout << "  -> Testing 120Hz Fixed-Tick Edge Latching..." << std::endl;

    InputSystem input;

    // Frame 1: Player presses Jump during render frame
    input.BeginFrame();
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 5000ULL;
    input.ProcessEvent(e);

    // First physics tick consumes the edge trigger
    input.BeginFixedTick();
    assert(input.WasActionPressedThisTick(InputAction::Jump));
    assert(input.IsActionDown(InputAction::Jump));

    // Catch-up tick in the same frame must NOT re-trigger the edge
    input.BeginFixedTick();
    assert(!input.WasActionPressedThisTick(InputAction::Jump));
    assert(input.IsActionDown(InputAction::Jump));

    // Frame 2: Player releases Jump
    input.BeginFrame();
    e.type = SDL_EVENT_KEY_UP;
    e.key.timestamp = 10000ULL;
    input.ProcessEvent(e);

    // Tick 3 observes release edge
    input.BeginFixedTick();
    assert(input.WasActionReleasedThisTick(InputAction::Jump));
    assert(!input.IsActionDown(InputAction::Jump));

    // Tick 4: Edge cleared
    input.BeginFixedTick();
    assert(!input.WasActionReleasedThisTick(InputAction::Jump));

    std::cout << "     [PASS] 120Hz Fixed-Tick Edge Latching" << std::endl;
}

void TestMetroidvaniaIntentBuffering() {
    std::cout << "  -> Testing Metroidvania Intent Buffering & Anti-Ghosting..." << std::endl;

    InputSystem input;

    // 1. Jump Buffering across frames before landing
    input.BeginFrame();
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 10'000'000ULL; // t = 10ms
    input.ProcessEvent(e);

    assert(input.WasActionPressedThisFrame(InputAction::Jump));
    assert(input.HasBufferedAction(InputAction::Jump, 10'000'000ULL, kDefaultBufferWindowNs));

    // Frame 2 at t = 20ms: Intent remains active even after edge expires
    input.BeginFrame();
    assert(!input.WasActionPressedThisFrame(InputAction::Jump));
    assert(input.HasBufferedAction(InputAction::Jump, 20'000'000ULL, kDefaultBufferWindowNs));

    // Landing at t = 60ms (50ms elapsed, within 100ms buffer window)
    u64 landTimeNs = 60'000'000ULL;
    assert(input.ConsumeAction(InputAction::Jump, landTimeNs, kDefaultBufferWindowNs));

    // Anti-ghosting: Intent cannot be consumed twice
    assert(!input.ConsumeAction(InputAction::Jump, landTimeNs, kDefaultBufferWindowNs));

    // 2. Buffer Window Expiration (Out of Window query)
    input.BeginFrame();
    e.key.scancode = SDL_SCANCODE_J; // Attack
    e.key.timestamp = 100'000'000ULL; // t = 100ms
    input.ProcessEvent(e);

    // Query 150ms later exceeds 100ms window
    u64 lateQueryNs = 250'000'000ULL;
    assert(!input.HasBufferedAction(InputAction::Attack, lateQueryNs, kDefaultBufferWindowNs));
    assert(!input.ConsumeAction(InputAction::Attack, lateQueryNs, kDefaultBufferWindowNs));

    // 3. Hitstun / Hazard cancellation
    input.BeginFrame();
    e.key.scancode = SDL_SCANCODE_LSHIFT; // Dash
    e.key.timestamp = 300'000'000ULL;
    input.ProcessEvent(e);

    assert(input.HasBufferedAction(InputAction::Dash, 310'000'000ULL, kDefaultBufferWindowNs));
    input.ClearActionBuffer();
    assert(!input.HasBufferedAction(InputAction::Dash, 310'000'000ULL, kDefaultBufferWindowNs));
    assert(!input.ConsumeAction(InputAction::Dash, 310'000'000ULL, kDefaultBufferWindowNs));

    // 4. Standalone ActionBuffer newest intent retention
    ActionBuffer buffer;
    for (u64 i = 0; i < 70; ++i) {
        buffer.RecordAction(InputAction::Attack, i * 1'000'000ULL);
    }
    // Newest press is at 69ms; consume at 70ms with 100ms window succeeds
    assert(buffer.ConsumeAction(InputAction::Attack, 70'000'000ULL, kDefaultBufferWindowNs));
    assert(!buffer.ConsumeAction(InputAction::Attack, 70'000'000ULL, kDefaultBufferWindowNs));

    std::cout << "     [PASS] Metroidvania Intent Buffering & Anti-Ghosting" << std::endl;
}

void TestMultiSourceKeyRedundancy() {
    std::cout << "  -> Testing Multi-Source Physical State Tracking..." << std::endl;

    InputSystem input;
    input.BindKey(SDL_SCANCODE_UP, InputAction::Jump);

    // Frame 1: Press Space
    input.BeginFrame();
    SDL_Event e1{};
    e1.type = SDL_EVENT_KEY_DOWN;
    e1.key.scancode = SDL_SCANCODE_SPACE;
    e1.key.timestamp = 100ULL;
    input.ProcessEvent(e1);
    assert(input.WasActionPressedThisFrame(InputAction::Jump));

    // Frame 2: While holding Space, press Up
    input.BeginFrame();
    SDL_Event e2{};
    e2.type = SDL_EVENT_KEY_DOWN;
    e2.key.scancode = SDL_SCANCODE_UP;
    e2.key.timestamp = 200ULL;
    input.ProcessEvent(e2);
    assert(input.IsActionDown(InputAction::Jump));
    assert(!input.WasActionPressedThisFrame(InputAction::Jump));

    // Frame 3: Release Space (Up remains held)
    input.BeginFrame();
    e1.type = SDL_EVENT_KEY_UP;
    e1.key.timestamp = 300ULL;
    input.ProcessEvent(e1);
    assert(input.IsActionDown(InputAction::Jump));
    assert(!input.WasActionReleasedThisFrame(InputAction::Jump));

    // Frame 4: Release Up (All physical sources released)
    input.BeginFrame();
    e2.type = SDL_EVENT_KEY_UP;
    e2.key.timestamp = 400ULL;
    input.ProcessEvent(e2);
    assert(!input.IsActionDown(InputAction::Jump));
    assert(input.WasActionReleasedThisFrame(InputAction::Jump));

    std::cout << "     [PASS] Multi-Source Physical State Tracking" << std::endl;
}

void TestCompositeVirtualAxes() {
    std::cout << "  -> Testing Composite Virtual Axis Evaluation..." << std::endl;

    InputSystem input;
    input.BeginFrame();

    // Press D -> MoveX = +1.0
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_D;
    input.ProcessEvent(e);
    assert(std::abs(input.GetAxis(AxisId::MoveX) - 1.0f) < 1e-5f);

    // Press A while holding D -> Neutralized to 0.0
    e.key.scancode = SDL_SCANCODE_A;
    input.ProcessEvent(e);
    assert(std::abs(input.GetAxis(AxisId::MoveX)) < 1e-5f);

    // Release A while D remains held -> Returns to +1.0
    e.type = SDL_EVENT_KEY_UP;
    input.ProcessEvent(e);
    assert(std::abs(input.GetAxis(AxisId::MoveX) - 1.0f) < 1e-5f);

    std::cout << "     [PASS] Composite Virtual Axis Evaluation" << std::endl;
}

void TestContextIsolationAndPurge() {
    std::cout << "  -> Testing InputContext Isolation & State Purge..." << std::endl;

    InputSystem input;
    assert(input.GetActiveContext() == InputContext::Gameplay);

    // In Gameplay context, Escape maps to MenuPause
    input.BeginFrame();
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_ESCAPE;
    e.key.timestamp = 100ULL;
    input.ProcessEvent(e);
    assert(input.IsActionDown(InputAction::MenuPause));

    // Switching to UI context purges active Gameplay actions
    input.SetActiveContext(InputContext::UI);
    assert(input.GetActiveContext() == InputContext::UI);
    assert(!input.IsActionDown(InputAction::MenuPause));

    // In UI Context, Space maps to MenuConfirm (not Jump)
    input.BeginFrame();
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 200ULL;
    input.ProcessEvent(e);
    assert(input.IsActionDown(InputAction::MenuConfirm));
    assert(!input.IsActionDown(InputAction::Jump));

    std::cout << "     [PASS] InputContext Isolation & State Purge" << std::endl;
}

void TestSubFrameTimestampLogging() {
    std::cout << "  -> Testing Sub-Frame Timestamp Logging..." << std::endl;

    InputSystem input;
    input.BeginFrame();

    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 12345ULL;
    input.ProcessEvent(e);

    e.key.scancode = SDL_SCANCODE_J; // Attack
    e.key.timestamp = 23456ULL;
    input.ProcessEvent(e);

    assert(input.GetTimestampedActionCount() == 2);
    assert(input.GetTimestampedAction(0).action == InputAction::Jump);
    assert(input.GetTimestampedAction(0).timestampNs == 12345ULL);
    assert(input.GetTimestampedAction(0).pressed);

    assert(input.GetTimestampedAction(1).action == InputAction::Attack);
    assert(input.GetTimestampedAction(1).timestampNs == 23456ULL);
    assert(input.GetTimestampedAction(1).pressed);

    // Frame boundary clears event log
    input.BeginFrame();
    assert(input.GetTimestampedActionCount() == 0);

    std::cout << "     [PASS] Sub-Frame Timestamp Logging" << std::endl;
}

void TestFocusLossSafety() {
    std::cout << "  -> Testing Window Focus Loss Release..." << std::endl;

    InputSystem input;
    input.BeginFrame();

    // Holding keys before focus lost
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_D;
    e.key.timestamp = 100ULL;
    input.ProcessEvent(e);

    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.timestamp = 200ULL;
    input.ProcessEvent(e);

    assert(input.IsKeyDown(SDL_SCANCODE_D));
    assert(input.IsActionDown(InputAction::Jump));

    // Focus lost event (e.g. Alt-Tab)
    SDL_Event focusLost{};
    focusLost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    focusLost.window.timestamp = 300ULL;
    input.ProcessEvent(focusLost);

    assert(!input.IsKeyDown(SDL_SCANCODE_D));
    assert(!input.IsActionDown(InputAction::Jump));
    assert(input.WasActionReleasedThisFrame(InputAction::Jump));

    std::cout << "     [PASS] Window Focus Loss Release" << std::endl;
}

int main() {
    std::cout << "EngineN Input Subsystem Test Suite" << std::endl;

    TestDeadzoneMath();
    TestSameFrameEdgeDetection();
    TestFixedTickEdgeLatching();
    TestMetroidvaniaIntentBuffering();
    TestMultiSourceKeyRedundancy();
    TestCompositeVirtualAxes();
    TestContextIsolationAndPurge();
    TestSubFrameTimestampLogging();
    TestFocusLossSafety();

    std::cout << "All input subsystem tests passed successfully." << std::endl;
    return 0;
}
