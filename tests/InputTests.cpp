#include "input/ActionBuffer.h"
#include "input/InputPipeline.h"
#include "input/InputTypes.h"

#include <cmath>
#include <cstdio>

namespace {

using namespace en;
using namespace en::input;

enum class TestAction : u8 {
    Jump     = 0,
    Attack   = 1,
    Dash     = 2,
    Interact = 3,
    Pause    = 15
};

enum class TestAxis : u8 {
    MoveX = 0,
    MoveY = 1,
    AimX  = 2,
    AimY  = 3
};

struct alignas(4) MockPlatformInputEvent {
    u16 packedHeader = 0;
    i16 value        = 0;

    [[nodiscard]] static constexpr MockPlatformInputEvent Make(u8 kind, u8 padSlot, u16 code, i16 val) noexcept {
        return {
            .packedHeader = static_cast<u16>((code & 0x0FFFu) | ((padSlot & 0x03u) << 12) | ((kind & 0x03u) << 14)),
            .value        = val
        };
    }
};

struct alignas(64) MockPlatformFrameEvents {
    f32 mouseX              = 0.0f;
    f32 mouseY              = 0.0f;
    f32 mouseDx             = 0.0f;
    f32 mouseDy             = 0.0f;
    f32 wheelX              = 0.0f;
    f32 wheelY              = 0.0f;
    u8  eventCount          = 0;
    u8  disconnectedPadMask = 0;
    bool focusCleared       = false;
    bool hasMouseMotion     = false;
    std::array<MockPlatformInputEvent, 57> events{};

    constexpr void Push(u8 kind, u8 padSlot, u16 code, i16 val) noexcept {
        if (eventCount < events.size()) {
            events[eventCount++] = MockPlatformInputEvent::Make(kind, padSlot, code, val);
        }
    }
};

static_assert(sizeof(MockPlatformFrameEvents) == 256);

consteval bool TestConstexprPipeline() noexcept {
    InputPipeline pipe{};
    pipe.Map()
        .Bind(TestAction::Jump, {Source::Key(44), Source::Pad(0)})
        .Bind(TestAction::Attack, {Source::Mouse(1), Source::Pad(2, 0)})
        .BindAxis(TestAxis::MoveX, {
            .negative    = Source::Key(4),
            .positive    = Source::Key(7),
            .gamepadAxis = GamepadAxis::LeftX,
            .socd        = SocdPolicy::LastInputWins,
            .deadzone    = 0.15f,
            .scale       = 1.0f
        });

    pipe.BeginFrame();
    pipe.FeedButton(Source::Key(44), true);
    pipe.FeedButton(Source::Key(44), false);
    const FrameInput& f1 = pipe.EndFrame();
    if (!f1.Pressed(TestAction::Jump) || !f1.Released(TestAction::Jump) || f1.Held(TestAction::Jump)) {
        return false;
    }

    pipe.BeginFrame();
    const FrameInput& f2 = pipe.EndFrame();
    if (f2.Pressed(TestAction::Jump) || f2.Released(TestAction::Jump) || f2.Held(TestAction::Jump)) {
        return false;
    }

    const TickInput t1 = pipe.PollTick(1);
    if (!t1.Pressed(TestAction::Jump) || !t1.Held(TestAction::Jump) || t1.Released(TestAction::Jump) ||
        t1.HeldTicks(TestAction::Jump) != 1) {
        return false;
    }

    const TickInput t2 = pipe.PollTick(2);
    if (t2.Pressed(TestAction::Jump) || t2.Held(TestAction::Jump) || !t2.Released(TestAction::Jump) ||
        t2.HeldTicks(TestAction::Jump) != 0) {
        return false;
    }

    return true;
}

static_assert(TestConstexprPipeline());

consteval bool TestConstexprActionBuffer() noexcept {
    ActionBuffer buf{};
    TickInput t{};
    t.pressed = MakeActionMask(TestAction::Jump);
    buf.Feed(t, false);

    if (buf.PendingPressMask() != MakeActionMask(TestAction::Jump)) {
        return false;
    }

    TickInput empty{};
    for (int i = 0; i < 100; ++i) {
        buf.Feed(empty, true);
    }
    if (!buf.Peek(TestAction::Jump, 0)) {
        return false;
    }

    for (int i = 0; i < 5; ++i) {
        buf.Feed(empty, false);
    }
    if (buf.Peek(TestAction::Jump, 4) || !buf.Peek(TestAction::Jump, 5)) {
        return false;
    }
    if (!buf.Consume(TestAction::Jump, 5) || buf.PendingPressMask() != 0) {
        return false;
    }
    return true;
}

static_assert(TestConstexprActionBuffer());

bool RunRuntimeTests() noexcept {
    InputPipeline pipe{};
    ActionBuffer  buf{};

    pipe.Map()
        .Bind(TestAction::Jump, {Source::Key(44), Source::Pad(0)})
        .Bind(TestAction::Attack, {Source::Mouse(1), Source::Pad(1, 0)})
        .Bind(TestAction::Dash, {Source::Key(225)})
        .BindAxis(TestAxis::MoveX, {
            .negative    = Source::Key(4),
            .positive    = Source::Key(7),
            .gamepadAxis = GamepadAxis::LeftX,
            .socd        = SocdPolicy::LastInputWins,
            .deadzone    = 0.15f,
            .scale       = 1.0f
        })
        .BindAxis(TestAxis::MoveY, {
            .negative    = Source::Key(26),
            .positive    = Source::Key(22),
            .gamepadAxis = GamepadAxis::LeftY,
            .socd        = SocdPolicy::Neutral,
            .inverted    = true,
            .deadzone    = 0.15f,
            .scale       = 1.0f
        });

    MockPlatformFrameEvents batch{};
    batch.hasMouseMotion = true;
    batch.mouseX         = 640.0f;
    batch.mouseY         = 360.0f;
    batch.mouseDx        = 12.5f;
    batch.mouseDy        = -4.0f;
    batch.wheelY         = 1.0f;
    batch.Push(0, 0, 4, 1);
    batch.Push(0, 0, 7, 1);
    batch.Push(1, 0, 1, 1);
    batch.Push(2, 1, 0, 1);

    const FrameInput& frame = pipe.IngestPlatformFrame(batch);
    if (!frame.Pressed(TestAction::Attack) || !frame.Pressed(TestAction::Jump) ||
        frame.mousePos.x != 640.0f || frame.mouseDelta.x != 12.5f || frame.mouseWheel.y != 1.0f ||
        frame.activeDevice != Device::Gamepad) {
        return false;
    }

    const TickInput tick1 = pipe.PollTick(1);
    if (!tick1.Held(TestAction::Jump) || !tick1.Held(TestAction::Attack) ||
        std::abs(tick1.Axis(TestAxis::MoveX) - 1.0f) > 1e-4f) {
        return false;
    }

    pipe.FeedButton(Source::Key(26), true);
    pipe.FeedButton(Source::Key(22), true);
    const TickInput tick2 = pipe.PollTick(2);
    if (tick2.Axis(TestAxis::MoveY) != 0.0f || tick2.HeldTicks(TestAction::Jump) != 2) {
        return false;
    }

    pipe.FeedButton(Source::Key(225), true);
    pipe.SetActionMask(MakeActionMask(TestAction::Jump));
    const TickInput tick3 = pipe.PollTick(3);
    if (tick3.Held(TestAction::Attack) || !tick3.Released(TestAction::Attack) ||
        tick3.Pressed(TestAction::Dash) || tick3.Held(TestAction::Dash) ||
        tick3.HeldTicks(TestAction::Attack) != 0 || !tick3.Held(TestAction::Jump)) {
        return false;
    }

    pipe.FeedButton(Source::Key(225), false);
    pipe.SetActionMask(InputPipeline::kAllActionsMask);
    const TickInput tick4 = pipe.PollTick(4);
    if (tick4.Pressed(TestAction::Dash) || tick4.Held(TestAction::Dash)) {
        return false;
    }

    pipe.FeedGamepadAxis(1, 0, 32767);
    pipe.FeedGamepadAxis(1, 1, 32767);
    const TickInput tick5 = pipe.PollTick(5);
    const Vec2      stick = tick5.Stick(TestAxis::MoveX, TestAxis::MoveY, 0.15f);
    const f32       len   = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (std::abs(len - 1.0f) > 1e-3f || stick.x <= 0.0f || stick.y >= 0.0f) {
        return false;
    }

    MockPlatformFrameEvents discBatch{};
    discBatch.disconnectedPadMask = (1u << 1);
    (void)pipe.IngestPlatformFrame(discBatch);
    pipe.FeedButton(Source::Key(4), false);
    pipe.FeedButton(Source::Key(7), false);
    pipe.FeedButton(Source::Key(26), false);
    pipe.FeedButton(Source::Key(22), false);

    const TickInput tick6 = pipe.PollTick(6);
    if (tick6.Held(TestAction::Jump) || !tick6.Released(TestAction::Jump) ||
        tick6.Axis(TestAxis::MoveX) != 0.0f || tick6.Axis(TestAxis::MoveY) != 0.0f) {
        return false;
    }

    MockPlatformFrameEvents focusLostBatch{};
    focusLostBatch.focusCleared = true;
    (void)pipe.IngestPlatformFrame(focusLostBatch);
    const TickInput tick7 = pipe.PollTick(7);
    if (tick7.Held(TestAction::Attack) || !tick7.Released(TestAction::Attack)) {
        return false;
    }

    buf.Feed(tick7, false);
    if (pipe.ComputeAuditHash() == 0 || buf.ComputeAuditHash() == 0 || tick7.ComputeAuditHash() == 0) {
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!RunRuntimeTests()) {
        std::puts("InputTests FAILED");
        return 1;
    }
    std::puts("InputTests PASSED");
    return 0;
}
