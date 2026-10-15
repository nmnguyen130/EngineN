#include "input/ActionBuffer.h"
#include "input/InputPipeline.h"
#include "input/InputTypes.h"
#include "input/SdlInput.h"

#include <SDL3/SDL_events.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

using namespace en;
using namespace en::input;

static u32 g_failedAssertions = 0;
static u32 g_passedAssertions = 0;

namespace {

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

enum class GameAction : u8 {
    Jump = 0,
    Attack,
    Dash,
    Interact,
    Menu
};

enum class GameAxis : u8 {
    MoveX = 0,
    MoveY
};

void TestCompileTimeTypesAndLayout() {
    static_assert(sizeof(Source) == 4);
    static_assert(sizeof(TickInput) == 64);
    static_assert(sizeof(FrameInput) == 64);
    static_assert(alignof(TickInput) == 64);
    static_assert(alignof(FrameInput) == 64);

    constexpr u16 mask = MakeActionMask(GameAction::Jump, GameAction::Attack, GameAction::Dash);
    TEST_ASSERT(mask == ((1u << 0) | (1u << 1) | (1u << 2)));
}

void TestDirectBindingAndPullEvaluation() {
    InputPipeline pipeline;

    constexpr u16 kScancodeSpace = 44;
    constexpr u16 kScancodeZ     = 29;

    pipeline.Map().Bind(GameAction::Jump, {Source::Key(kScancodeSpace)});
    pipeline.Map().Bind(GameAction::Attack, {Source::Key(kScancodeZ)});

    // Initially nothing held
    pipeline.BeginFrame();
    (void)pipeline.EndFrame();
    TickInput tick0 = pipeline.PollTick(0);
    TEST_ASSERT(!tick0.Held(GameAction::Jump));
    TEST_ASSERT(!tick0.Pressed(GameAction::Jump));
    TEST_ASSERT(!tick0.Released(GameAction::Jump));

    // Press Space key
    pipeline.BeginFrame();
    pipeline.FeedButton(Source::Key(kScancodeSpace), true);
    (void)pipeline.EndFrame();

    TickInput tick1 = pipeline.PollTick(1);
    TEST_ASSERT(tick1.Held(GameAction::Jump));
    TEST_ASSERT(tick1.Pressed(GameAction::Jump));
    TEST_ASSERT(!tick1.Released(GameAction::Jump));
    TEST_ASSERT(tick1.HeldTicks(GameAction::Jump) == 1);

    // Keep holding Space on tick 2
    pipeline.BeginFrame();
    (void)pipeline.EndFrame();

    TickInput tick2 = pipeline.PollTick(2);
    TEST_ASSERT(tick2.Held(GameAction::Jump));
    TEST_ASSERT(!tick2.Pressed(GameAction::Jump));
    TEST_ASSERT(!tick2.Released(GameAction::Jump));
    TEST_ASSERT(tick2.HeldTicks(GameAction::Jump) == 2);

    // Release Space key on tick 3
    pipeline.BeginFrame();
    pipeline.FeedButton(Source::Key(kScancodeSpace), false);
    (void)pipeline.EndFrame();

    TickInput tick3 = pipeline.PollTick(3);
    TEST_ASSERT(!tick3.Held(GameAction::Jump));
    TEST_ASSERT(!tick3.Pressed(GameAction::Jump));
    TEST_ASSERT(tick3.Released(GameAction::Jump));
    TEST_ASSERT(tick3.HeldTicks(GameAction::Jump) == 0);
}

void TestSubFrameTapPreservation() {
    InputPipeline pipeline;
    constexpr u16 kScancodeSpace = 44;
    pipeline.Map().Bind(GameAction::Jump, {Source::Key(kScancodeSpace)});

    // Fast press & release within the same render frame before PollTick runs
    pipeline.BeginFrame();
    pipeline.FeedButton(Source::Key(kScancodeSpace), true);
    pipeline.FeedButton(Source::Key(kScancodeSpace), false);
    (void)pipeline.EndFrame();

    // Tick N must see clean Pressed and Held, but NOT Released ((pressed & released) == 0 invariant)
    TickInput tick1 = pipeline.PollTick(1);
    TEST_ASSERT(tick1.Pressed(GameAction::Jump));
    TEST_ASSERT(tick1.Held(GameAction::Jump));
    TEST_ASSERT(!tick1.Released(GameAction::Jump));
    TEST_ASSERT((tick1.pressed & tick1.released) == 0);

    // Tick N+1 must see Released
    pipeline.BeginFrame();
    (void)pipeline.EndFrame();
    TickInput tick2 = pipeline.PollTick(2);
    TEST_ASSERT(!tick2.Pressed(GameAction::Jump));
    TEST_ASSERT(!tick2.Held(GameAction::Jump));
    TEST_ASSERT(tick2.Released(GameAction::Jump));
}

void TestActionMaskingAndStuckKeyImmunity() {
    InputPipeline pipeline;
    constexpr u16 kScancodeSpace = 44;
    pipeline.Map().Bind(GameAction::Jump, {Source::Key(kScancodeSpace)});

    // Press and hold Space
    pipeline.BeginFrame();
    pipeline.FeedButton(Source::Key(kScancodeSpace), true);
    (void)pipeline.EndFrame();
    TickInput tick1 = pipeline.PollTick(1);
    TEST_ASSERT(tick1.Held(GameAction::Jump));

    // Mask out Jump action (e.g. during cinematic or pause menu)
    pipeline.SetActionMask(static_cast<u16>(~MakeActionMask(GameAction::Jump)));

    // Next tick must evaluate Jump as NOT held, preventing stuck input bugs
    pipeline.BeginFrame();
    (void)pipeline.EndFrame();
    TickInput tick2 = pipeline.PollTick(2);
    TEST_ASSERT(!tick2.Held(GameAction::Jump));
    TEST_ASSERT(tick2.Released(GameAction::Jump));

    // Unmask Jump while key is still physically held
    pipeline.SetActionMask(InputPipeline::kAllActionsMask);
    pipeline.BeginFrame();
    (void)pipeline.EndFrame();
    TickInput tick3 = pipeline.PollTick(3);
    TEST_ASSERT(tick3.Held(GameAction::Jump));
}

void TestDeadzoneCalculations() {
    // Axial Deadzone
    TEST_ASSERT(ApplyAxialDeadzone(0.1f, 0.15f) == 0.0f);
    TEST_ASSERT(ApplyAxialDeadzone(-0.1f, 0.15f) == 0.0f);
    TEST_ASSERT(ApplyAxialDeadzone(1.0f, 0.15f) == 1.0f);
    TEST_ASSERT(ApplyAxialDeadzone(-1.0f, 0.15f) == -1.0f);

    const f32 midVal = ApplyAxialDeadzone(0.575f, 0.15f);
    // (0.575 - 0.15) / (1.0 - 0.15) = 0.425 / 0.85 = 0.5f
    TEST_ASSERT(std::abs(midVal - 0.5f) < 1e-4f);

    // Radial Deadzone
    const Vec2 stickInside{0.1f, 0.1f};
    const Vec2 resInside = ApplyRadialDeadzone(stickInside, 0.2f);
    TEST_ASSERT(resInside.x == 0.0f && resInside.y == 0.0f);

    const Vec2 stickOutside{0.6f, 0.8f}; // length == 1.0
    const Vec2 resOutside = ApplyRadialDeadzone(stickOutside, 0.2f);
    TEST_ASSERT(std::abs(resOutside.x - 0.6f) < 1e-4f);
    TEST_ASSERT(std::abs(resOutside.y - 0.8f) < 1e-4f);
}

void TestSocdPolicies() {
    InputPipeline pipeline;
    constexpr u16 kKeyA = 4;
    constexpr u16 kKeyD = 7;

    // LastInputWins policy
    pipeline.Map().BindAxis(GameAxis::MoveX, AxisBinding{
        .negative = Source::Key(kKeyA),
        .positive = Source::Key(kKeyD),
        .socd     = SocdPolicy::LastInputWins,
        .deadzone = 0.0f
    });

    pipeline.BeginFrame();
    pipeline.FeedButton(Source::Key(kKeyA), true);
    (void)pipeline.EndFrame();
    TickInput t1 = pipeline.PollTick(1);
    TEST_ASSERT(t1.Axis(GameAxis::MoveX) == -1.0f);

    // Press D while continuing to hold A -> D wins
    pipeline.BeginFrame();
    pipeline.FeedButton(Source::Key(kKeyD), true);
    (void)pipeline.EndFrame();
    TickInput t2 = pipeline.PollTick(2);
    TEST_ASSERT(t2.Axis(GameAxis::MoveX) == 1.0f);

    // Release D while A is still down -> reverts to A (-1.0f)
    pipeline.BeginFrame();
    pipeline.FeedButton(Source::Key(kKeyD), false);
    (void)pipeline.EndFrame();
    TickInput t3 = pipeline.PollTick(3);
    TEST_ASSERT(t3.Axis(GameAxis::MoveX) == -1.0f);

    // Neutral policy
    pipeline.Map().BindAxis(GameAxis::MoveX, AxisBinding{
        .negative = Source::Key(kKeyA),
        .positive = Source::Key(kKeyD),
        .socd     = SocdPolicy::Neutral,
        .deadzone = 0.0f
    });

    pipeline.BeginFrame();
    pipeline.FeedButton(Source::Key(kKeyD), true); // Both A and D are now held
    (void)pipeline.EndFrame();
    TickInput t4 = pipeline.PollTick(4);
    TEST_ASSERT(t4.Axis(GameAxis::MoveX) == 0.0f);
}

void TestActionBufferForgivenessAndHitstopFreeze() {
    ActionBuffer buffer;

    TickInput pressTick{};
    pressTick.pressed = MakeActionMask(GameAction::Jump);

    TickInput idleTick{};

    // Feed jump press on tick 1
    buffer.Feed(pressTick, false);
    TEST_ASSERT(buffer.Peek(GameAction::Jump, 4));

    // Advance 3 ticks
    buffer.Feed(idleTick, false); // age = 1
    buffer.Feed(idleTick, false); // age = 2
    buffer.Feed(idleTick, false); // age = 3

    TEST_ASSERT(buffer.Peek(GameAction::Jump, 3));
    TEST_ASSERT(!buffer.Peek(GameAction::Jump, 2)); // age > 2

    // Consume intent
    TEST_ASSERT(buffer.Consume(GameAction::Jump, 3));
    TEST_ASSERT(!buffer.Peek(GameAction::Jump, 5)); // consumed, no longer available

    // Hitstop freeze retention
    buffer.Feed(pressTick, false); // age = 0
    buffer.Feed(idleTick, true);   // channel frozen: age does NOT advance
    buffer.Feed(idleTick, true);   // channel frozen: age does NOT advance

    TEST_ASSERT(buffer.Peek(GameAction::Jump, 0)); // age remained 0 during hitstop!
}

void TestSdlEventIngestion() {
    InputPipeline pipeline;
    pipeline.Map().Bind(GameAction::Jump, {Source::Key(SDL_SCANCODE_SPACE)});

    pipeline.BeginFrame();

    SDL_Event keyEvent{};
    keyEvent.type         = SDL_EVENT_KEY_DOWN;
    keyEvent.key.scancode = SDL_SCANCODE_SPACE;
    keyEvent.key.down     = true;
    keyEvent.key.repeat   = false;
    IngestSdlEvent(pipeline, keyEvent);

    SDL_Event mouseEvent{};
    mouseEvent.type         = SDL_EVENT_MOUSE_MOTION;
    mouseEvent.motion.x     = 320.0f;
    mouseEvent.motion.y     = 240.0f;
    mouseEvent.motion.xrel  = 5.0f;
    mouseEvent.motion.yrel  = -3.0f;
    IngestSdlEvent(pipeline, mouseEvent);

    const FrameInput& frame = pipeline.EndFrame();
    TEST_ASSERT(frame.Pressed(GameAction::Jump));
    TEST_ASSERT(frame.mousePos.x == 320.0f && frame.mousePos.y == 240.0f);
    TEST_ASSERT(frame.mouseDelta.x == 5.0f && frame.mouseDelta.y == -3.0f);
    TEST_ASSERT(frame.activeDevice == Device::Keyboard);

    TickInput tick = pipeline.PollTick(1);
    TEST_ASSERT(tick.Pressed(GameAction::Jump));
}

} // namespace

int main() {
    std::cout << "Running EngineN Platform Input Subsystem Tests...\n";

    TestCompileTimeTypesAndLayout();
    TestDirectBindingAndPullEvaluation();
    TestSubFrameTapPreservation();
    TestActionMaskingAndStuckKeyImmunity();
    TestDeadzoneCalculations();
    TestSocdPolicies();
    TestActionBufferForgivenessAndHitstopFreeze();
    TestSdlEventIngestion();

    std::cout << "Test Summary: " << g_passedAssertions << " passed, "
              << g_failedAssertions << " failed.\n";

    return (g_failedAssertions == 0) ? 0 : 1;
}
