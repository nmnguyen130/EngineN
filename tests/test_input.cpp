#include "platform/input/ActionBuffer.h"
#include "platform/input/InputBindings.h"
#include "platform/input/InputSystem.h"
#include "platform/input/InputTypes.h"

#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace EngineN;

namespace {

enum class Action : ActionId {
    Jump = 0,
    Attack = 1,
    Dash = 2,
    Cast = 3
};

enum class Context : ContextId {
    Gameplay = 0,
    Menu = 1
};

SDL_Event MakeKeyEvent(Uint32 type, SDL_Scancode scancode, u64 timestampNs, bool repeat = false) {
    SDL_Event e{};
    e.type = type;
    e.key.scancode = scancode;
    e.key.down = (type == SDL_EVENT_KEY_DOWN);
    e.key.repeat = repeat;
    e.key.timestamp = timestampNs;
    return e;
}

SDL_Event MakeMouseButtonEvent(Uint32 type, Uint8 button, u64 timestampNs) {
    SDL_Event e{};
    e.type = type;
    e.button.button = button;
    e.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    e.button.timestamp = timestampNs;
    return e;
}

SDL_Event MakeMouseMotionEvent(f32 x, f32 y, f32 xrel, f32 yrel, u64 timestampNs) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = xrel;
    e.motion.yrel = yrel;
    e.motion.timestamp = timestampNs;
    return e;
}

SDL_Event MakeMouseWheelEvent(f32 x, f32 y, u64 timestampNs) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.x = x;
    e.wheel.y = y;
    e.wheel.timestamp = timestampNs;
    return e;
}

SDL_Event MakeGamepadButtonEvent(Uint32 type, Uint8 button, Uint32 which, u64 timestampNs) {
    SDL_Event e{};
    e.type = type;
    e.gbutton.button = button;
    e.gbutton.which = which;
    e.gbutton.down = (type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    e.gbutton.timestamp = timestampNs;
    return e;
}

SDL_Event MakeGamepadAxisEvent(Uint8 axis, Sint16 value, Uint32 which, u64 timestampNs) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.axis = axis;
    e.gaxis.value = value;
    e.gaxis.which = which;
    e.gaxis.timestamp = timestampNs;
    return e;
}

SDL_Event MakeGamepadDeviceEvent(Uint32 type, Uint32 which, u64 timestampNs) {
    SDL_Event e{};
    e.type = type;
    e.gdevice.which = which;
    e.gdevice.timestamp = timestampNs;
    return e;
}

SDL_Event MakeFocusEvent(Uint32 type, u64 timestampNs) {
    SDL_Event e{};
    e.type = type;
    e.window.timestamp = timestampNs;
    return e;
}

void TestSourceValidationAndBindingLimits() {
    InputBindings bindings;

    // Out-of-bounds scancodes and mouse buttons
    auto res1 = bindings.BindAxis(0, Source{Device::Keyboard, 9999}, Source{Device::Keyboard, 3}, 0);
    assert(!res1.HasValue() && res1.Error() == InputError::InvalidSource);

    auto res2 = bindings.BindAction(0, Source{Device::Keyboard, 0}, Action::Jump);
    assert(!res2.HasValue() && res2.Error() == InputError::InvalidSource);

    auto res3 = bindings.BindAction(0, Source{Device::Mouse, 0}, Action::Jump);
    assert(!res3.HasValue() && res3.Error() == InputError::InvalidSource);

    // Opposing sources must not be identical
    auto res4 = bindings.BindAxis(0, Source{Device::Keyboard, SDL_SCANCODE_A}, Source{Device::Keyboard, SDL_SCANCODE_A}, 0);
    assert(!res4.HasValue() && res4.Error() == InputError::InvalidSource);

    // Duplicate action binding
    (void)bindings.BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);
    auto dupRes = bindings.BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);
    assert(!dupRes.HasValue() && dupRes.Error() == InputError::Duplicate);

    // Capacity limit
    bindings.Clear();
    for (u16 i = 1; i <= static_cast<u16>(InputBindings::kMaxActionBindings); ++i) {
        auto r = bindings.BindAction(0, Source{Device::Keyboard, i}, Action::Jump);
        assert(r.HasValue());
    }
    auto overflow = bindings.BindAction(0, Source{Device::Keyboard, 200}, Action::Jump);
    assert(!overflow.HasValue() && overflow.Error() == InputError::TableFull);

    // Direct ingestion of invalid events must not crash or write OOB
    InputSystem input;
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, static_cast<SDL_Scancode>(9999), 1000));
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_UNKNOWN, 2000));
}

void TestSubTickEdgePreservation() {
    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);

    // Tap Space within tick 0 slice [0, 8.33ms)
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 2'000'000));
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 5'000'000));

    // Both edges must be preserved within the single tick slice
    TickInput tick0 = input.CutTick(0, 8'333'333);
    assert(tick0.Pressed(Action::Jump));
    assert(tick0.Released(Action::Jump));
    assert(!tick0.Held(Action::Jump));

    // Tick 1 must be clean without residual edge bleed
    TickInput tick1 = input.CutTick(1, 16'666'666);
    assert(!tick1.Pressed(Action::Jump));
    assert(!tick1.Released(Action::Jump));
    assert(!tick1.Held(Action::Jump));
}

void TestMultiTickHoldAndCatchup() {
    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);

    // Press at 3ms, hold across multiple ticks, release at 40ms
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 3'000'000));
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 40'000'000));

    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Pressed(Action::Jump) && t0.Held(Action::Jump));
    assert(t0.heldTicks[static_cast<u8>(Action::Jump)] == 1);

    TickInput t1 = input.CutTick(1, 16'666'666);
    assert(!t1.Pressed(Action::Jump) && t1.Held(Action::Jump));
    assert(t1.heldTicks[static_cast<u8>(Action::Jump)] == 2);

    TickInput t2 = input.CutTick(2, 25'000'000);
    assert(t2.Held(Action::Jump));
    assert(t2.heldTicks[static_cast<u8>(Action::Jump)] == 3);

    TickInput t3 = input.CutTick(3, 33'333'333);
    assert(t3.Held(Action::Jump));
    assert(t3.heldTicks[static_cast<u8>(Action::Jump)] == 4);

    TickInput t4 = input.CutTick(4, 41'666'666);
    assert(!t4.Held(Action::Jump) && t4.Released(Action::Jump));
    assert(t4.heldTicks[static_cast<u8>(Action::Jump)] == 0);

    TickInput t5 = input.CutTick(5, 50'000'000);
    assert(!t5.Held(Action::Jump) && !t5.Released(Action::Jump));
}

void TestContextStackAndScoping() {
    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);
    (void)input.GetBindings().BindAction(1, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Attack);

    // Hold Space in gameplay
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 1'000'000));
    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Pressed(Action::Jump));

    // Push menu context with RAII scope
    {
        ScopedContext menuScope(input, Context::Menu, true);
        assert(menuScope.IsActive());

        // Lower context action receives synthetic release
        TickInput t1 = input.CutTick(1, 16'666'666);
        assert(t1.Released(Action::Jump));
        assert(!t1.Pressed(Action::Attack)); // Latched: no phantom press

        // Re-press in menu
        input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 20'000'000));
        (void)input.CutTick(2, 25'000'000);

        input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 28'000'000));
        TickInput t3 = input.CutTick(3, 33'333'333);
        assert(t3.Pressed(Action::Attack));
    }

    // Auto-popped on destruction
    assert(input.GetActiveContext() == 0);
}

void TestLatchingAndAntiGhosting() {
    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);

    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 1'000'000));
    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Pressed(Action::Jump) && t0.Held(Action::Jump));

    // Unbinding while held generates synthetic release
    (void)input.GetBindings().UnbindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);
    input.ResyncActionStates(10'000'000);

    TickInput t1 = input.CutTick(1, 16'666'666);
    assert(t1.Released(Action::Jump) && !t1.Held(Action::Jump));

    // Rebinding while physically held must not generate phantom press
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);
    input.ResyncActionStates(20'000'000);

    TickInput t2 = input.CutTick(2, 25'000'000);
    assert(!t2.Pressed(Action::Jump) && !t2.Held(Action::Jump));

    // Physical release then re-press fires clean press
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 30'000'000));
    (void)input.CutTick(3, 33'333'333);

    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 35'000'000));
    TickInput t4 = input.CutTick(4, 41'666'666);
    assert(t4.Pressed(Action::Jump) && t4.Held(Action::Jump));
}

void TestDigitalSOCDAndAnalogDeadzone() {
    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAxis(0, Source{Device::Keyboard, SDL_SCANCODE_A}, Source{Device::Keyboard, SDL_SCANCODE_D}, 0);
    (void)input.GetBindings().BindGamepadAxis(0, GamepadAxis::LeftX, 0, 0.15f);

    // Digital negative
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, 1'000'000));
    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Axis(0) < -0.99f);

    // Opposing positive key pressed while negative held (SOCD last-input priority)
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_D, 10'000'000));
    TickInput t1 = input.CutTick(1, 16'666'666);
    assert(t1.Axis(0) > 0.99f);

    // Reverts to held negative
    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_D, 20'000'000));
    TickInput t2 = input.CutTick(2, 25'000'000);
    assert(t2.Axis(0) < -0.99f);

    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_A, 30'000'000));
    TickInput t3 = input.CutTick(3, 33'333'333);
    assert(std::abs(t3.Axis(0)) < 0.001f);

    // Gamepad analog within deadzone (0.10f < 0.15f)
    input.Ingest(MakeGamepadAxisEvent(SDL_GAMEPAD_AXIS_LEFTX, 3277, 1, 35'000'000));
    TickInput t4 = input.CutTick(4, 41'666'666);
    assert(std::abs(t4.Axis(0)) < 0.001f);

    // Gamepad analog outside deadzone (16384 / 32767 = 0.5f)
    input.Ingest(MakeGamepadAxisEvent(SDL_GAMEPAD_AXIS_LEFTX, 16384, 1, 45'000'000));
    TickInput t5 = input.CutTick(5, 50'000'000);
    assert(t5.Axis(0) > 0.35f && t5.Axis(0) < 0.45f);
}

void TestMouseMotionAndWheel() {
    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Mouse, SDL_BUTTON_LEFT}, Action::Attack);

    input.Ingest(MakeMouseMotionEvent(100.0f, 200.0f, 10.0f, -5.0f, 1'000'000));
    input.Ingest(MakeMouseMotionEvent(105.0f, 202.0f, 5.0f, 2.0f, 2'000'000));
    input.Ingest(MakeMouseWheelEvent(0.0f, 3.0f, 3'000'000));
    input.Ingest(MakeMouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, 4'000'000));

    const FrameInput& f0 = input.PublishFrameInput();
    assert(f0.Pressed(Action::Attack));
    assert(f0.mousePosition.x == 105.0f && f0.mousePosition.y == 202.0f);
    assert(f0.mouseDelta.x == 15.0f && f0.mouseDelta.y == -3.0f);
    assert(f0.mouseWheel.y == 3.0f);
    assert(f0.eventCount >= 1);

    // Next frame resets delta and wheel
    const FrameInput& f1 = input.PublishFrameInput();
    assert(f1.mouseDelta.x == 0.0f && f1.mouseDelta.y == 0.0f);
    assert(f1.mouseWheel.x == 0.0f && f1.mouseWheel.y == 0.0f);
}

void TestFocusLossAndDeviceDisconnect() {
    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);
    (void)input.GetBindings().BindAction(0, Source{Device::Gamepad, SDL_GAMEPAD_BUTTON_SOUTH, 1}, Action::Attack);

    input.Ingest(MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 1'000'000));
    input.Ingest(MakeGamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_SOUTH, 1, 2'000'000));
    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Held(Action::Jump) && t0.Held(Action::Attack));

    // Focus lost releases all active states
    input.Ingest(MakeFocusEvent(SDL_EVENT_WINDOW_FOCUS_LOST, 10'000'000));
    TickInput t1 = input.CutTick(1, 16'666'666);
    assert(!t1.Held(Action::Jump) && t1.Released(Action::Jump));
    assert(!t1.Held(Action::Attack) && t1.Released(Action::Attack));

    // Hotplug disconnect releases pad buttons
    input.Ingest(MakeGamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_SOUTH, 1, 20'000'000));
    (void)input.CutTick(2, 25'000'000);

    input.Ingest(MakeGamepadDeviceEvent(SDL_EVENT_GAMEPAD_REMOVED, 1, 30'000'000));
    TickInput t3 = input.CutTick(3, 33'333'333);
    assert(!t3.Held(Action::Attack) && t3.Released(Action::Attack));
}

void TestFixedRingQueueOverflow() {
    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);

    // Ingest 14 rapid transitions exceeding queue capacity (8)
    for (u64 i = 0; i < 14; ++i) {
        const Uint32 type = (i % 2 == 0) ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        input.Ingest(MakeKeyEvent(type, SDL_SCANCODE_SPACE, 1'000'000 + i * 50'000));
    }
    assert(input.GetDroppedTransitionsCount() > 0);
}

void TestActionBufferIntentAndFreeze() {
    ActionBuffer buf;
    TickInput in{};
    in.pressed = (1u << static_cast<u8>(Action::Jump));
    in.held = in.pressed;

    buf.Feed(in);
    assert(buf.Peek(Action::Jump, 3));

    // Age 2 ticks
    TickInput empty{};
    buf.Feed(empty);
    buf.Feed(empty);
    assert(buf.Peek(Action::Jump, 3));

    // Hitstop freeze keeps buffer age frozen
    for (int i = 0; i < 5; ++i) {
        buf.Feed(empty, true);
    }
    assert(buf.Peek(Action::Jump, 3));

    // Consume removes intent
    assert(buf.Consume(Action::Jump, 3));
    assert(!buf.Peek(Action::Jump, 3));

    // Expiration after window exceeds kMaxBufferTicks
    buf.Feed(in);
    for (int i = 0; i < 70; ++i) {
        buf.Feed(empty, false);
    }
    assert(!buf.Peek(Action::Jump, 255));
}

void TestDeterminism() {
    InputSystem inputA;
    InputSystem inputB;
    ActionBuffer bufferA;
    ActionBuffer bufferB;

    inputA.GetBindings().Clear();
    inputB.GetBindings().Clear();
    (void)inputA.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);
    (void)inputB.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, Action::Jump);

    u64 simTimeNs = 0;
    for (u64 tick = 0; tick < 1000; ++tick) {
        simTimeNs += 8'333'333;

        if (tick % 7 == 0) {
            auto e = MakeKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, simTimeNs - 4'000'000);
            inputA.Ingest(e);
            inputB.Ingest(e);
        } else if (tick % 7 == 3) {
            auto e = MakeKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, simTimeNs - 2'000'000);
            inputA.Ingest(e);
            inputB.Ingest(e);
        }

        TickInput inA = inputA.CutTick(tick, simTimeNs);
        TickInput inB = inputB.CutTick(tick, simTimeNs);

        assert(inA.held == inB.held);
        assert(inA.pressed == inB.pressed);
        assert(inA.released == inB.released);

        bufferA.Feed(inA);
        bufferB.Feed(inB);
        assert(bufferA.Peek(Action::Jump, 5) == bufferB.Peek(Action::Jump, 5));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << "\n";
        return -1;
    }

    std::cout << "[TestInput] Running test suite...\n";

    TestSourceValidationAndBindingLimits();
    std::cout << "  -> Source validation & binding limits: OK\n";

    TestSubTickEdgePreservation();
    std::cout << "  -> Sub-tick edge preservation: OK\n";

    TestMultiTickHoldAndCatchup();
    std::cout << "  -> Multi-tick hold & hitch catchup: OK\n";

    TestContextStackAndScoping();
    std::cout << "  -> Context stack & scoping: OK\n";

    TestLatchingAndAntiGhosting();
    std::cout << "  -> Latching & anti-ghosting: OK\n";

    TestDigitalSOCDAndAnalogDeadzone();
    std::cout << "  -> Digital SOCD & analog deadzone: OK\n";

    TestMouseMotionAndWheel();
    std::cout << "  -> Mouse motion & wheel: OK\n";

    TestFocusLossAndDeviceDisconnect();
    std::cout << "  -> Focus loss & device disconnect: OK\n";

    TestFixedRingQueueOverflow();
    std::cout << "  -> Fixed ring queue overflow: OK\n";

    TestActionBufferIntentAndFreeze();
    std::cout << "  -> ActionBuffer intent & freeze: OK\n";

    TestDeterminism();
    std::cout << "  -> 1000-tick determinism: OK\n";

    std::cout << "[TestInput] All tests passed!\n";

    SDL_Quit();
    return 0;
}
