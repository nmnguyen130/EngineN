#include "input/InputPipeline.h"
#include "platform/PlatformTypes.h"
#include <cmath>
#include <cstdio>

namespace en {

namespace {

enum class TestAction : u8 {
    Jump = 0,
    Attack = 1,
    Dash = 2,
    Parry = 3,
    ShootR2 = 4,
};

enum class TestAxis : u8 {
    MoveX = 0,
    MoveY = 1,
};

[[nodiscard]] consteval bool VerifyConstexprInputSubsystem() noexcept {
    // 1. InputSource per-device validation (including virtual Schmitt-trigger PadTrigger/PadAxisButton)
    if (InputSource{}.IsValid()) return false;
    if (InputSource::Key(0U).IsValid() || !InputSource::Key(44U).IsValid() || InputSource::Key(512U).IsValid()) {
        return false;
    }
    if (InputSource::Mouse(0U).IsValid() || !InputSource::Mouse(1U).IsValid() || InputSource::Mouse(8U).IsValid()) {
        return false;
    }
    if (!InputSource::Pad(0U, 0U).IsValid() || InputSource::Pad(32U, 0U).IsValid() || InputSource::Pad(0U, 4U).IsValid()) {
        return false;
    }
    const InputSource r2Trig = InputSource::PadTrigger(true);
    if (!r2Trig.IsValid() || r2Trig.code != 31U) return false;
    if (!InputSource::PadAxisButton(0U, false).IsValid() || InputSource::PadAxisButton(6U, true).IsValid()) {
        return false;
    }

    // 2. Constexpr Radial & Axial Deadzones, Non-Linear Response Curve & 8-Way Octagonal / Numpad
    const Vec2 radZero = TickInput::ApplyRadialDeadzone({0.10f, 0.05f}, 0.20f, 0.95f);
    if (radZero.x != 0.0f || radZero.y != 0.0f) return false;
    const Vec2 radFull = TickInput::ApplyRadialDeadzone({0.98f, 0.0f}, 0.15f, 0.95f);
    if (std::abs(radFull.x - 1.0f) > 1e-4f || std::abs(radFull.y) > 1e-4f) return false;
    const Vec2 radCurved = TickInput::ApplyRadialDeadzone({0.55f, 0.0f}, 0.15f, 0.95f, 2.0f);
    if (std::abs(radCurved.x - (1.0f / 3.0f)) > 1e-3f) return false;

    if (TickInput::OctagonalToNumpad({0, 0}) != 5U) return false;
    if (TickInput::OctagonalToNumpad({0, -1}) != 2U) return false;
    if (TickInput::OctagonalToNumpad({1, -1}) != 3U) return false;
    if (TickInput::OctagonalToNumpad({1, 0}) != 6U) return false;

    // 3. InputPipeline: Dual sub-frame tap latches, Dual Axis Bindings (WASD + Arrows), & Schmitt-Trigger R2
    InputPipeline pipe{};
    pipe.Reset();
    pipe.Map()
        .Bind(TestAction::Jump, {InputSource::Key(44U), InputSource::Key(26U)})
        .Bind(TestAction::Attack, {InputSource::Key(13U), InputSource::Mouse(1U)})
        .Bind(TestAction::Dash, {InputSource::Key(225U)})
        .Bind(TestAction::Parry, {InputSource::Key(14U)})
        .Bind(TestAction::ShootR2, {InputSource::PadTrigger(true)})
        .BindAxis(TestAxis::MoveX, AxisBinding{
            .negSource = InputSource::Key(4U),     // A
            .posSource = InputSource::Key(7U),     // D
            .altNegSource = InputSource::Key(80U), // Left Arrow
            .altPosSource = InputSource::Key(79U), // Right Arrow
            .deadzone = 0.15f,
            .outerDeadzone = 0.95f,
            .sensitivity = 1.0f,
            .gamepadAxis = 0U,
            .padSlot = kAnyGamepad,
            .socd = SocdPolicy::LastInputWins,
            .invert = false,
        })
        .BindAxis(TestAxis::MoveY, AxisBinding{
            .negSource = InputSource::Key(22U), // S (Down = -1)
            .posSource = InputSource::Key(26U), // W (Up = +1)
            .deadzone = 0.15f,
            .outerDeadzone = 0.95f,
            .sensitivity = 1.0f,
            .gamepadAxis = 1U,
            .padSlot = kAnyGamepad,
        });

    // Sub-frame tap: Key Down + Up before EndFrame() or PollTick()
    pipe.FeedButton(InputSource::Key(44U), true);
    pipe.FeedButton(InputSource::Key(44U), false);

    const FrameInput& f1 = pipe.EndFrame();
    if (!f1.Pressed(TestAction::Jump) || !f1.Held(TestAction::Jump)) return false;
    const FrameInput& f2 = pipe.EndFrame();
    if (f2.Pressed(TestAction::Jump) || f2.Held(TestAction::Jump) || !f2.Released(TestAction::Jump)) {
        return false;
    }

    const TickInput t1 = pipe.PollTick(1ULL);
    if (!t1.Pressed(TestAction::Jump) || !t1.Held(TestAction::Jump) || t1.HeldTicks(TestAction::Jump) != 1U) {
        return false;
    }
    const TickInput t2 = pipe.PollTick(2ULL);
    if (!t2.Released(TestAction::Jump) || !t2.TapReleased(TestAction::Jump, 5U)) return false;

    // 4. Unreal Enhanced Input Triggers on TickInput: HeldOneShot, Pulsed, TapReleased, WasChargedRelease
    pipe.FeedButton(InputSource::Key(13U), true);
    const TickInput th1 = pipe.PollTick(3ULL); // heldTicks == 1
    if (!th1.Pulsed(TestAction::Attack, 3U, 2U) || th1.HeldOneShot(TestAction::Attack, 3U)) return false;
    const TickInput th2 = pipe.PollTick(4ULL); // heldTicks == 2
    if (th2.Pulsed(TestAction::Attack, 3U, 2U) || th2.HeldOneShot(TestAction::Attack, 3U)) return false;
    const TickInput th3 = pipe.PollTick(5ULL); // heldTicks == 3 -> HeldOneShot(3) fires exactly once!
    if (!th3.HeldOneShot(TestAction::Attack, 3U) || th3.Pulsed(TestAction::Attack, 3U, 2U)) return false;
    const TickInput th4 = pipe.PollTick(6ULL); // heldTicks == 4 -> HeldOneShot(3) is now false!
    if (th4.HeldOneShot(TestAction::Attack, 3U)) return false;
    const TickInput th5 = pipe.PollTick(7ULL); // heldTicks == 5 -> Pulsed(delay=3, step=2) fires!
    if (!th5.Pulsed(TestAction::Attack, 3U, 2U)) return false;

    pipe.FeedButton(InputSource::Key(13U), false);
    const TickInput thRel = pipe.PollTick(8ULL);
    // Held for 5 ticks: TapReleased(maxTap=3) must be false, while WasChargedRelease(minCharge=4) is true!
    if (thRel.TapReleased(TestAction::Attack, 3U) || !thRel.WasChargedRelease(TestAction::Attack, 4U)) {
        return false;
    }

    // 5. Schmitt-Trigger Hysteresis on Analog Trigger R2 (axis 5)
    pipe.FeedGamepadAxis(0U, 5U, 10000); // Below press threshold (18000) -> not pressed
    if (pipe.PollTick(9ULL).Held(TestAction::ShootR2)) return false;
    pipe.FeedGamepadAxis(0U, 5U, 20000); // Crosses press threshold (>= 18000) -> pressed!
    const TickInput tr1 = pipe.PollTick(10ULL);
    if (!tr1.Pressed(TestAction::ShootR2) || !tr1.Held(TestAction::ShootR2)) return false;
    pipe.FeedGamepadAxis(0U, 5U, 15000); // Inside hysteresis band (12000..18000) -> stays held without chatter!
    if (!pipe.PollTick(11ULL).Held(TestAction::ShootR2)) return false;
    pipe.FeedGamepadAxis(0U, 5U, 11000); // Drops below release threshold (<= 12000) -> released!
    if (!pipe.PollTick(12ULL).Released(TestAction::ShootR2)) return false;
    pipe.FeedGamepadAxis(0U, 5U, 0);

    // 6. Alternate Digital Axis Bindings (Arrow Keys) & Sub-Frame Axis Tap
    pipe.FeedButton(InputSource::Key(79U), true);  // Right Arrow (altPosSource) sub-frame tap
    pipe.FeedButton(InputSource::Key(79U), false);
    if (pipe.PollTick(13ULL).Axis(TestAxis::MoveX) != 1.0f) return false;
    if (pipe.PollTick(14ULL).Axis(TestAxis::MoveX) != 0.0f) return false;

    // 7. 64B ActionBuffer: Numpad Motion (236 Hadouken facing Left/Right) & [4]6 Charge Motion
    ActionBuffer buf{};
    // Execute Quarter-Circle Forward while facing LEFT (2 -> 1 -> 4 + Attack):
    // Tick 15: Down (2)
    pipe.FeedButton(InputSource::Key(22U), true); // S (Down)
    buf.Feed(pipe.PollTick(15ULL));
    // Tick 16: Down-Left (1)
    pipe.FeedButton(InputSource::Key(4U), true);  // A (Left) while S held
    buf.Feed(pipe.PollTick(16ULL));
    // Tick 17: Left (4) + Attack
    pipe.FeedButton(InputSource::Key(22U), false); // Release S, keep A (Left)
    pipe.FeedButton(InputSource::Key(13U), true);  // Press Attack
    buf.Feed(pipe.PollTick(17ULL));

    constexpr std::array<u8, 3> kQcf{2U, 3U, 6U};
    // Facing right should NOT match 214, but facing left (facingRight = false) MUST match 236!
    if (buf.CheckMotion(kQcf, true, 20U)) return false;
    if (!buf.ConsumeMotion(kQcf, TestAction::Attack, false, 20U, 5U)) return false;
    if (buf.Peek(TestAction::Attack)) return false; // Attack press was consumed by the special move!

    pipe.FeedButton(InputSource::Key(4U), false);
    pipe.FeedButton(InputSource::Key(13U), false);
    (void)pipe.PollTick(18ULL);

    // Execute [4]6 Back-Forward Charge while facing RIGHT (hold Down-Back '1' for 10 ticks, then Forward '6')
    buf.Clear();
    pipe.FeedButton(InputSource::Key(22U), true); // Down
    pipe.FeedButton(InputSource::Key(4U), true);  // Left -> Down-Back (1) charges both Back (4) and Down (2)!
    for (u64 tick = 20ULL; tick < 30ULL; ++tick) {
        buf.Feed(pipe.PollTick(tick));
    }
    pipe.FeedButton(InputSource::Key(22U), false);
    pipe.FeedButton(InputSource::Key(4U), false);
    pipe.FeedButton(InputSource::Key(7U), true); // Forward (6)
    buf.Feed(pipe.PollTick(30ULL));
    if (!buf.CheckChargeMotion(4U, 6U, 10U, true, 6U)) return false;
    pipe.FeedButton(InputSource::Key(7U), false);

    // 8. Release-Gated Double-Tap (holding Key 1 and pressing Key 2 without release must NOT double-tap!)
    buf.Clear();
    pipe.FeedButton(InputSource::Key(44U), true); // Hold Space (Jump)
    buf.Feed(pipe.PollTick(31ULL));
    pipe.FeedButton(InputSource::Key(26U), true); // Press W (also bound to Jump) while Space is STILL HELD
    buf.Feed(pipe.PollTick(32ULL));
    if (buf.WasDoubleTapped(TestAction::Jump)) return false; // Must NOT trigger without an intervening release!
    pipe.FeedButton(InputSource::Key(44U), false);
    pipe.FeedButton(InputSource::Key(26U), false);
    buf.Feed(pipe.PollTick(33ULL)); // Now Jump is released -> arms double-tap!
    pipe.FeedButton(InputSource::Key(44U), true);
    buf.Feed(pipe.PollTick(34ULL)); // Second press after release -> triggers double-tap!
    if (!buf.WasDoubleTapped(TestAction::Jump) || !buf.ConsumeDoubleTap(TestAction::Jump)) return false;

    if (pipe.ComputeAuditHash() == 0ULL || buf.ComputeAuditHash() == 0ULL) return false;
    return true;
}

static_assert(VerifyConstexprInputSubsystem());

} // namespace

} // namespace en

int main() {
    using namespace en;

    u32 passed = 0U;
    u32 failed = 0U;
    auto check = [&](bool cond, const char* expr, int line) noexcept {
        if (cond) {
            ++passed;
        } else {
            ++failed;
            std::fprintf(stderr, "[FAIL] Line %d: %s\n", line, expr);
        }
    };
#define EN_CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

    // 1. Struct byte sizes & cache-line alignment invariants (0B padding across all structs)
    EN_CHECK(sizeof(InputSource) == 4U && alignof(InputSource) == 4U);
    EN_CHECK(sizeof(AxisBinding) == 32U && alignof(AxisBinding) == 32U);
    EN_CHECK(sizeof(InputMap) == 384U && alignof(InputMap) == 64U);
    EN_CHECK(sizeof(TickInput) == 64U && alignof(TickInput) == 64U);
    EN_CHECK(sizeof(FrameInput) == 64U && alignof(FrameInput) == 64U);
    EN_CHECK(sizeof(ActionBuffer) == 64U && alignof(ActionBuffer) == 64U);
    EN_CHECK(sizeof(InputReplayStream::Segment) == 12U && alignof(InputReplayStream::Segment) == 4U);
    EN_CHECK(sizeof(InputPipeline::GamepadState) == 16U && alignof(InputPipeline::GamepadState) == 16U);
    EN_CHECK(sizeof(InputPipeline) == 640U && alignof(InputPipeline) == 64U);

    // 2. End-to-end PlatformFrameEvents -> InputPipeline::IngestPlatformFrame integration
    InputPipeline input{};
    input.Reset();
    input.Map()
        .Bind(0U, {InputSource::Key(44U), InputSource::Pad(0U)})
        .Bind(1U, {InputSource::Mouse(1U), InputSource::PadTrigger(true)})
        .BindAxis(0U, AxisBinding{
            .negSource = InputSource::Key(4U),
            .posSource = InputSource::Key(7U),
            .altNegSource = InputSource::Key(80U),
            .altPosSource = InputSource::Key(79U),
            .deadzone = 0.15f,
            .outerDeadzone = 0.95f,
            .sensitivity = 1.0f,
            .gamepadAxis = 0U,
            .padSlot = kAnyGamepad,
            .socd = SocdPolicy::LastInputWins,
            .invert = false,
        })
        .BindAxis(1U, AxisBinding{
            .negSource = InputSource::Key(22U),
            .posSource = InputSource::Key(26U),
            .gamepadAxis = 1U,
            .padSlot = kAnyGamepad,
        });

    PlatformFrameEvents batch{};
    batch.IngestMouseMotion(320.0f, 180.0f, 12.0f, -6.0f, 2.0f, true, EventCapture::None);
    batch.IngestMouseWheel(0.0f, 2.0f, false, true, EventCapture::None);
    batch.IngestTextInput("bind r2 attack", true);
    batch.IngestKey(44U, true, false, true, EventCapture::None);
    batch.IngestGamepadAxis(2U, 0U, 32767, true, EventCapture::None);
    batch.IngestGamepadAxis(2U, 1U, 32767, true, EventCapture::None);
    batch.IngestGamepadAxis(2U, 5U, 25000, true, EventCapture::None); // R2 trigger >= 18000!

    const FrameInput& fi = input.IngestPlatformFrame(batch);
    EN_CHECK(fi.hasFocus && fi.Pressed(0U) && fi.Held(0U));
    EN_CHECK(fi.Pressed(1U) && fi.Held(1U)); // R2 analog trigger fired Action 1!
    EN_CHECK(fi.mousePos == Vec2{640.0f, 360.0f} && fi.mouseDelta == Vec2{24.0f, -12.0f});
    EN_CHECK(fi.mouseWheel == Vec2{0.0f, 2.0f});
    EN_CHECK(fi.TextView() == "bind r2 attack");

    const TickInput ti = input.PollTick(1ULL);
    EN_CHECK(ti.Pressed(0U) && ti.Pressed(1U) && std::abs(ti.Axis(0U) - 1.0f) < 1e-4f);
    EN_CHECK((ti.StickOctagonal(0U, 1U) == IVec2{1, 1}));
    EN_CHECK(ti.StickNumpad(0U, 1U) == 9U);
    EN_CHECK(std::abs(ti.Stick(0U, 1U).LengthSq() - 1.0f) < 1e-4f);

    // 3. Disconnected gamepad mask in PlatformFrameEvents clears stuck pad axes/buttons
    batch.ClearFrameDeltas();
    batch.IngestGamepadDisconnect(2U);
    (void)input.IngestPlatformFrame(batch);
    const TickInput tiAfterDisc = input.PollTick(2ULL);
    EN_CHECK(std::abs(tiAfterDisc.Axis(0U)) < 1e-4f && !tiAfterDisc.Held(1U));

    // 4. Runtime Rebind Capture via Analog Trigger Pull (L2 axis 4)
    input.BeginRebindCapture(2U, 0U);
    EN_CHECK(input.IsWaitingForRebind());
    input.FeedGamepadAxis(0U, 4U, 24000); // Pull L2 trigger
    EN_CHECK(!input.IsWaitingForRebind());
    EN_CHECK(input.Map().ActionBindings(2U)[0] == InputSource::PadTrigger(false, 0U));

    // 5. Binary Serialization (384B exact) & Corrupted Byte Sanitization
    std::array<u8, InputMap::kSerializedSize> rawBytes{};
    const usize written = input.Map().SerializeBindings(rawBytes);
    EN_CHECK(written == 384U);

    InputMap restored{};
    EN_CHECK(restored.DeserializeBindings(rawBytes));
    EN_CHECK(restored.ActionBindings(2U)[0] == InputSource::PadTrigger(false, 0U));

    rawBytes[0] = static_cast<u8>(InputDevice::Keyboard);
    rawBytes[1] = kAnyGamepad;
    rawBytes[2] = 0x0FU;
    rawBytes[3] = 0x27U; // 0x270F = 9999 >= 512
    EN_CHECK(restored.DeserializeBindings(rawBytes));
    EN_CHECK(!restored.ActionBindings(0U)[0].IsValid());

    std::printf("InputTests: %u passed, %u failed.\n", passed, failed);
    return failed == 0U ? 0 : 1;
}
