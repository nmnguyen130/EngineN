#include "core/Log.h"
#include "platform/input/ActionBuffer.h"
#include "platform/input/InputBindings.h"
#include "platform/input/InputSystem.h"
#include "platform/input/InputTypes.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <iostream>

using namespace EngineN;

namespace {

enum class GameAction : ActionId {
    Jump = 0,
    Attack = 1,
    Dash = 2,
    Cast = 3
};

enum class GameContext : ContextId {
    Gameplay = 0,
    Menu = 1
};

SDL_Event CreateKeyEvent(Uint32 type, SDL_Scancode scancode, u64 timestampNs, bool repeat = false) {
    SDL_Event e{};
    e.type = type;
    e.key.scancode = scancode;
    e.key.down = (type == SDL_EVENT_KEY_DOWN);
    e.key.repeat = repeat;
    e.key.timestamp = timestampNs;
    return e;
}

void TestOOBAndSourceValidation() {
    std::cout << "  -> Test 1: OOB & Source Validation..." << std::endl;

    InputBindings bindings;

    // Test rejecting invalid scancodes and mouse buttons
    auto res1 = bindings.BindAxis(0, Source{Device::Keyboard, 9999}, Source{Device::Keyboard, 3}, 0);
    assert(!res1.HasValue());
    assert(res1.Error() == InputError::InvalidSource);

    auto res2 = bindings.BindAction(0, Source{Device::Keyboard, 0}, GameAction::Jump);
    assert(!res2.HasValue());
    assert(res2.Error() == InputError::InvalidSource);

    auto res3 = bindings.BindAction(0, Source{Device::Mouse, 0}, GameAction::Jump);
    assert(!res3.HasValue());
    assert(res3.Error() == InputError::InvalidSource);

    auto res4 = bindings.BindAxis(0, Source{Device::Keyboard, SDL_SCANCODE_A}, Source{Device::Keyboard, SDL_SCANCODE_A}, 0);
    assert(!res4.HasValue());
    assert(res4.Error() == InputError::InvalidSource);

    // Ingest invalid scancodes into InputSystem without crash
    InputSystem input;
    SDL_Event badEvent = CreateKeyEvent(SDL_EVENT_KEY_DOWN, static_cast<SDL_Scancode>(9999), 1000);
    input.Ingest(badEvent);

    badEvent = CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_UNKNOWN, 2000);
    input.Ingest(badEvent);
}

void TestSyntheticPressPreventionAndReleaseOnLoss() {
    std::cout << "  -> Test 2: Synthetic Press Prevention & Release on Loss..." << std::endl;

    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);

    // Press Space at 1ms
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 1'000'000));
    TickInput tick0 = input.CutTick(0, 8'333'333);
    assert(tick0.Pressed(GameAction::Jump));
    assert(tick0.Held(GameAction::Jump));

    // While Space is still held down, unbind Jump
    (void)input.GetBindings().UnbindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);
    input.ResyncActionStates(10'000'000);

    // Next tick must observe a synthetic release
    TickInput tick1 = input.CutTick(1, 16'666'666);
    assert(tick1.Released(GameAction::Jump));
    assert(!tick1.Held(GameAction::Jump));

    // Rebind Jump while Space is still physically held
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);
    input.ResyncActionStates(20'000'000);

    // Must be latched: no synthetic press!
    TickInput tick2 = input.CutTick(2, 25'000'000);
    assert(!tick2.Pressed(GameAction::Jump));
    assert(!tick2.Held(GameAction::Jump));

    // Release Space
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 30'000'000));
    (void)input.CutTick(3, 33'333'333);

    // Press Space again: clean press must fire
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 35'000'000));
    TickInput tick4 = input.CutTick(4, 41'666'666);
    assert(tick4.Pressed(GameAction::Jump));
    assert(tick4.Held(GameAction::Jump));
}

void TestSubTickTapEdgePreservation() {
    std::cout << "  -> Test 3: Sub-Tick Tap Edge Preservation..." << std::endl;

    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);

    // Tap Space between 2ms and 5ms (both within tick 0: [0, 8.33ms))
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 2'000'000));
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 5'000'000));

    // Tick 0 consumes press
    TickInput tick0 = input.CutTick(0, 8'333'333);
    assert(tick0.Pressed(GameAction::Jump));
    assert(tick0.Held(GameAction::Jump));
    assert(!tick0.Released(GameAction::Jump));

    // Tick 1 consumes release
    TickInput tick1 = input.CutTick(1, 16'666'666);
    assert(!tick1.Pressed(GameAction::Jump));
    assert(!tick1.Held(GameAction::Jump));
    assert(tick1.Released(GameAction::Jump));
}

void Test50msHitchReplay() {
    std::cout << "  -> Test 4: 50ms Hitch Replay (Section 4.2)..." << std::endl;

    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);

    // Jump pressed at 3ms, released at 40ms during a 50ms frame hitch
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 3'000'000));
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 40'000'000));

    // Tick 0 (boundary 8.33ms)
    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Pressed(GameAction::Jump));
    assert(t0.Held(GameAction::Jump));
    assert(t0.heldTicks[static_cast<u8>(GameAction::Jump)] == 1);

    // Tick 1 (boundary 16.67ms)
    TickInput t1 = input.CutTick(1, 16'666'666);
    assert(!t1.Pressed(GameAction::Jump));
    assert(t1.Held(GameAction::Jump));
    assert(t1.heldTicks[static_cast<u8>(GameAction::Jump)] == 2);

    // Tick 2 (boundary 25.00ms)
    TickInput t2 = input.CutTick(2, 25'000'000);
    assert(t2.Held(GameAction::Jump));
    assert(t2.heldTicks[static_cast<u8>(GameAction::Jump)] == 3);

    // Tick 3 (boundary 33.33ms)
    TickInput t3 = input.CutTick(3, 33'333'333);
    assert(t3.Held(GameAction::Jump));
    assert(t3.heldTicks[static_cast<u8>(GameAction::Jump)] == 4);

    // Tick 4 (boundary 41.67ms) -> release at 40ms lands here
    TickInput t4 = input.CutTick(4, 41'666'666);
    assert(!t4.Held(GameAction::Jump));
    assert(t4.Released(GameAction::Jump));
    assert(t4.heldTicks[static_cast<u8>(GameAction::Jump)] == 0);

    // Tick 5 (boundary 50.00ms) -> idle
    TickInput t5 = input.CutTick(5, 50'000'000);
    assert(!t5.Held(GameAction::Jump));
    assert(!t5.Released(GameAction::Jump));
}

void TestZeroTickFrames() {
    std::cout << "  -> Test 5: 0-Tick Frames (240Hz Render vs 120Hz Sim)..." << std::endl;

    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);

    // Frame 0 arrives with input, but no sim tick is due yet
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 2'000'000));
    const FrameInput& f0 = input.PublishFrameInput();
    assert(f0.Pressed(GameAction::Jump));
    assert(f0.Held(GameAction::Jump));

    // Next frame (sim tick now runs at boundary 8.33ms)
    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Pressed(GameAction::Jump));
    assert(t0.Held(GameAction::Jump));
}

void TestTakeNewerAxisResolution() {
    std::cout << "  -> Test 6: TakeNewer Digital Axis Resolution..." << std::endl;

    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAxis(0, Source{Device::Keyboard, SDL_SCANCODE_A}, Source{Device::Keyboard, SDL_SCANCODE_D}, 0);

    // Press A at 1ms -> Axis = -1.0
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, 1'000'000));
    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Axis(0) < -0.99f);

    // Press D at 10ms while A is still held -> TakeNewer: Axis = +1.0
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_D, 10'000'000));
    TickInput t1 = input.CutTick(1, 16'666'666);
    assert(t1.Axis(0) > 0.99f);

    // Release D at 20ms while A is still held -> reverts to -1.0
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_D, 20'000'000));
    TickInput t2 = input.CutTick(2, 25'000'000);
    assert(t2.Axis(0) < -0.99f);

    // Release A at 30ms -> Axis = 0.0
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_A, 30'000'000));
    TickInput t3 = input.CutTick(3, 33'333'333);
    assert(std::abs(t3.Axis(0)) < 0.001f);
}

void TestContextStackAndLatching() {
    std::cout << "  -> Test 7: Context Stack & Latching..." << std::endl;

    InputSystem input;
    input.GetBindings().Clear();
    // Gameplay (ctx 0): Space = Jump
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);
    // Menu (ctx 1): Space = Attack (Confirm)
    (void)input.GetBindings().BindAction(1, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Attack);

    // Hold Space in Gameplay
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 1'000'000));
    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(t0.Pressed(GameAction::Jump));

    // Push Menu context (consumes = true) while Space is held
    auto pushRes = input.PushContext(GameContext::Menu, true);
    assert(pushRes.HasValue());

    // Gameplay Jump must receive synthetic release
    TickInput t1 = input.CutTick(1, 16'666'666);
    assert(t1.Released(GameAction::Jump));
    // Menu Attack must NOT receive a phantom press (it is latched)
    assert(!t1.Pressed(GameAction::Attack));

    // Release Space in Menu
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 20'000'000));
    (void)input.CutTick(2, 25'000'000);

    // Re-press Space in Menu -> Menu action Attack fires cleanly
    input.Ingest(CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 28'000'000));
    TickInput t3 = input.CutTick(3, 33'333'333);
    assert(t3.Pressed(GameAction::Attack));

    // Pop Menu context
    auto popRes = input.PopContext();
    assert(popRes.HasValue());
}

void TestQueueOverflow() {
    std::cout << "  -> Test 8: Queue Overflow Protection..." << std::endl;

    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);

    // Inject 12 rapid transitions in a single tick
    for (u64 i = 0; i < 12; ++i) {
        const Uint32 type = (i % 2 == 0) ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        input.Ingest(CreateKeyEvent(type, SDL_SCANCODE_SPACE, 1'000'000 + i * 100'000));
    }

    assert(input.GetDroppedTransitionsCount() > 0);
}

void TestDeterminism() {
    std::cout << "  -> Test 9: Determinism Verification..." << std::endl;

    InputSystem inputA;
    InputSystem inputB;
    ActionBuffer bufferA;
    ActionBuffer bufferB;

    inputA.GetBindings().Clear();
    inputB.GetBindings().Clear();
    (void)inputA.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);
    (void)inputB.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);

    u64 simTimeNs = 0;
    for (u64 tick = 0; tick < 1000; ++tick) {
        simTimeNs += 8'333'333;

        // Deterministic synthetic inputs every few ticks
        if (tick % 7 == 0) {
            auto e = CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, simTimeNs - 4'000'000);
            inputA.Ingest(e);
            inputB.Ingest(e);
        } else if (tick % 7 == 3) {
            auto e = CreateKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, simTimeNs - 2'000'000);
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

        assert(bufferA.Peek(GameAction::Jump, 5) == bufferB.Peek(GameAction::Jump, 5));
    }
}

void TestImGuiGate() {
    std::cout << "  -> Test 10: ImGui DevTools Press-Gating..." << std::endl;

    InputSystem input;
    input.GetBindings().Clear();
    (void)input.GetBindings().BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, GameAction::Jump);

    // Simulate ImGui capturing keyboard: suppress DOWN, pass UP
    bool imguiWantsKeyboard = true;
    SDL_Event downEv = CreateKeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, 1'000'000);
    if (!imguiWantsKeyboard) {
        input.Ingest(downEv);
    }

    // Release ALWAYS passes through
    SDL_Event upEv = CreateKeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, 5'000'000);
    input.Ingest(upEv);

    TickInput t0 = input.CutTick(0, 8'333'333);
    assert(!t0.Held(GameAction::Jump));
    assert(!t0.Pressed(GameAction::Jump));
}

void TestIntentBufferForgiveness() {
    std::cout << "  -> Test 11: IntentBuffer Forgiveness & Hitstop Freeze..." << std::endl;

    ActionBuffer buf;
    TickInput in{};
    in.pressed = (1u << static_cast<u8>(GameAction::Jump));
    in.held = in.pressed;

    // Feed press
    buf.Feed(in);
    assert(buf.Peek(GameAction::Jump, 3));

    // Age 2 ticks without press
    TickInput emptyIn{};
    buf.Feed(emptyIn);
    buf.Feed(emptyIn);
    assert(buf.Peek(GameAction::Jump, 3));

    // Hitstop freeze for 5 ticks: age must NOT increment!
    for (int i = 0; i < 5; ++i) {
        buf.Feed(emptyIn, /*frozen=*/true);
    }
    assert(buf.Peek(GameAction::Jump, 3));

    // Consume action
    assert(buf.Consume(GameAction::Jump, 3));
    assert(!buf.Peek(GameAction::Jump, 3)); // Consumed once, cannot be re-consumed
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "========================================\n";
    std::cout << "Running EngineN Input Subsystem Test Suite\n";
    std::cout << "========================================\n";

    TestOOBAndSourceValidation();
    TestSyntheticPressPreventionAndReleaseOnLoss();
    TestSubTickTapEdgePreservation();
    Test50msHitchReplay();
    TestZeroTickFrames();
    TestTakeNewerAxisResolution();
    TestContextStackAndLatching();
    TestQueueOverflow();
    TestDeterminism();
    TestImGuiGate();
    TestIntentBufferForgiveness();

    std::cout << "All Input Subsystem Tests Passed Successfully!\n";
    return 0;
}
