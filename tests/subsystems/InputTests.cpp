#include "input/ActionBuffer.h"
#include "input/InputSystem.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace en;

struct MockEvent {
    u8  kind{0};
    u16 code{0};
    i16 value{0};

    [[nodiscard]] constexpr u8 Kind() const noexcept { return kind; }
    [[nodiscard]] constexpr u16 Code() const noexcept { return code; }
    [[nodiscard]] constexpr bool Down() const noexcept { return value != 0; }
};

struct MockBatch {
    f32 mouseX{0.0f}, mouseY{0.0f};
    f32 mouseDx{0.0f}, mouseDy{0.0f};
    f32 wheelX{0.0f}, wheelY{0.0f};
    u8  padDisconnected{0};
    u8  focused{1};
    std::string_view text{};
    std::vector<MockEvent> events{};

    [[nodiscard]] std::span<const MockEvent> Events() const noexcept { return events; }
    [[nodiscard]] std::string_view TextView() const noexcept { return text; }
};

bool TestSubframeTapAndHoldDuration() noexcept {
    InputSystem input;
    input.Reset();
    input.Map().Bind(0U, {InputSource::Key(44U)});

    MockBatch batch{};
    batch.events.push_back({0U, 44U, 1});
    batch.events.push_back({0U, 44U, 0});
    const FrameInput& fi = input.Ingest(batch);
    if (!fi.Pressed(0U) || !fi.Held(0U)) return false;

    const TickInput t1 = input.PollTick(1ULL);
    if (!t1.Pressed(0U) || !t1.Held(0U) || t1.HeldTicks(0U) != 1U) return false;

    const TickInput t2 = input.PollTick(2ULL);
    if (!t2.Released(0U) || t2.Held(0U) || !t2.TapReleased(0U, 5U)) return false;

    return true;
}

bool TestSocdAndAnalogStick() noexcept {
    InputSystem input;
    input.Reset();
    input.Map().BindAxis(0U, AxisBinding{
        .neg = InputSource::Key(4U),
        .pos = InputSource::Key(7U),
        .deadzone = 0.2f,
        .gamepadAxis = 0U,
    });

    MockBatch b1{};
    b1.events.push_back({0U, 4U, 1});
    (void)input.Ingest(b1);
    if (input.PollTick(1ULL).Axis(0U) != -1.0f) return false;

    MockBatch b2{};
    b2.events.push_back({0U, 7U, 1});
    (void)input.Ingest(b2);
    if (input.PollTick(2ULL).Axis(0U) != 1.0f) return false;

    AxisBinding neutral = input.Map().Axis(0U);
    neutral.SetNeutralSocd(true);
    input.Map().BindAxis(0U, neutral);
    if (input.PollTick(3ULL).Axis(0U) != 0.0f) return false;

    std::array<u8, InputMap::kSerializedSize> raw{};
    if (input.Map().Serialize(raw) != InputMap::kSerializedSize) return false;
    InputMap restored{};
    if (!restored.Deserialize(raw) || !restored.Axis(0U).NeutralSocd()) return false;

    return true;
}

bool TestActionBufferAndCoyoteTime() noexcept {
    ActionBuffer buf;
    TickInput in{};
    in.pressed = 1U << 0U;
    buf.Feed(in, false);

    in.pressed = 0U;
    for (int i = 0; i < 3; ++i) {
        buf.Feed(in, false);
    }
    for (int i = 0; i < 20; ++i) {
        buf.Feed(in, true);
    }
    if (!buf.Peek(0U, 6U)) return false;

    if (buf.TryJump(0U, false, 10U, 6U, 5U)) return false;
    if (!buf.TryJump(0U, false, 4U, 6U, 5U)) return false;
    if (buf.Peek(0U, 6U)) return false;

    return true;
}

} // namespace

int main() {
    if (!TestSubframeTapAndHoldDuration()) return 1;
    if (!TestSocdAndAnalogStick()) return 2;
    if (!TestActionBufferAndCoyoteTime()) return 3;
    std::printf("InputTests: ALL PASSED\n");
    return 0;
}
