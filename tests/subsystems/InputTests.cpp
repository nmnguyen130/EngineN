#include "tests/subsystems/CtsHarness.h"

#include "input/ActionBuffer.h"
#include "input/InputSystem.h"

#include <vector>

namespace {

using namespace en;
using namespace en::cts;

struct MockEvent {
    u8  kind{0};
    u16 code{0};
    i16 value{0};

    [[nodiscard]] constexpr u8  Kind() const noexcept { return kind; }
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

bool CtsSubframeTapsRetriggersAndCharge(CtsRunner& r) noexcept {
    InputSystem input;
    input.Reset();
    // Bind Action 0 to primary key 44 (Space) and secondary PadBtn 0 (South).
    input.Map().Bind(0U, {InputSource::Key(44U), InputSource::PadBtn(0U)});

    // Sub-frame tap: press and release within the same frame before PollTick.
    MockBatch b1{};
    b1.events.push_back({0U, 44U, 1});
    b1.events.push_back({0U, 44U, 0});
    const FrameInput& fi1 = input.Ingest(b1);
    CTS_CHECK(r, fi1.Pressed(0U) && fi1.Held(0U) && !fi1.Released(0U));

    const TickInput t1 = input.PollTick(1ULL);
    CTS_CHECK(r, t1.Pressed(0U) && t1.Held(0U) && t1.HeldTicks(0U) == 1U);

    const TickInput t2 = input.PollTick(2ULL);
    CTS_CHECK(r, t2.Released(0U) && !t2.Held(0U) && t2.HeldTicks(0U) == 1U && t2.TapReleased(0U, 6U));

    // Hold duration + ChargeRatio + multi-source hold continuity.
    MockBatch b2{};
    b2.events.push_back({0U, 44U, 1});
    (void)input.Ingest(b2);
    for (u16 tick = 1U; tick <= 15U; ++tick) {
        const TickInput ti = input.PollTick(tick + 2ULL);
        CTS_CHECK(r, ti.Held(0U) && ti.HeldTicks(0U) == tick);
        CTS_CHECK(r, Near(ti.ChargeRatio(0U, 30U), static_cast<f32>(tick) / 30.0f));
    }

    // Pressing secondary source while primary is already held must NOT retrigger.
    MockBatch b3{};
    b3.events.push_back({2U, 0U, 1});
    (void)input.Ingest(b3);
    const TickInput tSecondary = input.PollTick(20ULL);
    CTS_CHECK(r, tSecondary.Held(0U) && !tSecondary.Pressed(0U) && tSecondary.retriggered == 0U);

    // Releasing primary while secondary stays down keeps action held!
    MockBatch b4{};
    b4.events.push_back({0U, 44U, 0});
    (void)input.Ingest(b4);
    const TickInput tStillHeld = input.PollTick(21ULL);
    CTS_CHECK(r, tStillHeld.Held(0U) && !tStillHeld.Released(0U));
    return true;
}

bool CtsSocdSchmittHysteresisAndRebind(CtsRunner& r) noexcept {
    InputSystem input;
    input.Reset();
    input.Map().BindAxis(0U, AxisBinding{
        .neg         = InputSource::Key(4U),  // A
        .pos         = InputSource::Key(7U),  // D
        .altNeg      = InputSource::Key(80U), // Left
        .altPos      = InputSource::Key(79U), // Right
        .deadzone    = 0.20f,
        .gamepadAxis = 0U,
    });
    // Bind virtual axis 0 negative/positive buttons to Actions 2 and 3.
    input.Map().Bind(2U, {InputSource::PadAxis(0U, false)});
    input.Map().Bind(3U, {InputSource::PadAxis(0U, true)});

    // Last-Input-Priority SOCD: Hold Left (A), press Right (D) -> +1; release Right (D) -> returns to -1!
    MockBatch b{};
    b.events.push_back({0U, 4U, 1});
    (void)input.Ingest(b);
    CTS_CHECK(r, input.PollTick(1ULL).Axis(0U) == -1.0f);

    b.events = {{0U, 7U, 1}};
    (void)input.Ingest(b);
    CTS_CHECK(r, input.PollTick(2ULL).Axis(0U) == 1.0f);

    b.events = {{0U, 7U, 0}};
    (void)input.Ingest(b);
    CTS_CHECK(r, input.PollTick(3ULL).Axis(0U) == -1.0f);

    b.events = {{0U, 4U, 0}};
    (void)input.Ingest(b);
    CTS_CHECK(r, input.PollTick(4ULL).Axis(0U) == 0.0f);

    // Schmitt-trigger hysteresis on Gamepad Axis 0:
    // Press threshold = 18000, Release threshold = 12000.
    b.events = {{3U, 0U, 17500}};
    (void)input.Ingest(b);
    CTS_CHECK(r, !input.PollTick(5ULL).Held(3U));

    b.events = {{3U, 0U, 18500}};
    (void)input.Ingest(b);
    CTS_CHECK(r, input.PollTick(6ULL).Pressed(3U));

    // Dropping to 15000 (between 12000 and 18000) must stay held without oscillation!
    b.events = {{3U, 0U, 15000}};
    (void)input.Ingest(b);
    CTS_CHECK(r, input.PollTick(7ULL).Held(3U));

    // Dropping below 12000 releases the virtual button.
    b.events = {{3U, 0U, 11500}};
    (void)input.Ingest(b);
    CTS_CHECK(r, input.PollTick(8ULL).Released(3U));

    // Runtime interactive rebind + focus loss clearing.
    input.BeginRebind(5U, 0U);
    CTS_CHECK(r, input.WaitingRebind());
    b.events = {{0U, 29U, 1}};
    (void)input.Ingest(b);
    CTS_CHECK(r, !input.WaitingRebind() && input.Map().Action(5U)[0] == InputSource::Key(29U));

    b.focused = 0U;
    b.events.clear();
    (void)input.Ingest(b);
    CTS_CHECK(r, !input.IsDown(InputSource::Key(29U)));

    // Adversarial InputMap::Deserialize sanitization check.
    std::array<u8, InputMap::kSerializedSize> garbage{};
    garbage.fill(0xFFU);
    InputMap sanitized{};
    CTS_CHECK(r, sanitized.Deserialize(garbage));
    for (u8 a = 0U; a < kMaxActions; ++a) {
        for (const InputSource s : sanitized.Action(a)) {
            CTS_CHECK(r, !s.IsValid());
        }
    }
    for (u8 ax = 0U; ax < kMaxAxes; ++ax) {
        const AxisBinding& ab = sanitized.Axis(ax);
        CTS_CHECK(r, ab.deadzone == 0.15f && ab.gamepadAxis == 0xFFU && ab.pad == 0U);
    }
    return true;
}

bool DstActionBufferOracleFuzz(CtsRunner& r) noexcept {
    ActionBuffer buf;
    std::array<u8, kMaxActions> oraclePress{};
    std::array<u8, kMaxActions> oracleRelease{};
    oraclePress.fill(0xFFU);
    oracleRelease.fill(0xFFU);

    Rng rng(0xB0FFE22026ULL);

    for (u32 step = 0U; step < 5000U; ++step) {
        const bool frozen = (rng.Bounded(5U) == 0U);
        TickInput in{};
        in.pressed  = static_cast<u16>(rng.Bounded(0x10000U) & rng.Bounded(0x10000U));
        in.released = static_cast<u16>(rng.Bounded(0x10000U) & rng.Bounded(0x10000U));

        buf.Feed(in, frozen);

        if (!frozen) {
            for (u8& a : oraclePress)   if (a < 0xFFU) ++a;
            for (u8& a : oracleRelease) if (a < 0xFFU) ++a;
        }
        for (u8 i = 0U; i < kMaxActions; ++i) {
            if ((in.released & (1U << i)) != 0U) oracleRelease[i] = 0U;
            if ((in.pressed  & (1U << i)) != 0U) oraclePress[i]   = 0U;
        }

        for (u8 i = 0U; i < kMaxActions; ++i) {
            const u8 win = static_cast<u8>(rng.Range(1, 20));
            const bool expectedPeek = (oraclePress[i] <= win);
            CTS_CHECK(r, buf.Peek(i, win) == expectedPeek);
        }

        const u8 act = static_cast<u8>(rng.Bounded(kMaxActions));
        const u8 win = static_cast<u8>(rng.Range(4, 15));
        if (rng.Bounded(3U) == 0U) {
            const bool consumed = buf.Consume(act, win);
            const bool exp      = (oraclePress[act] <= win);
            CTS_CHECK(r, consumed == exp);
            if (exp) oraclePress[act] = 0xFFU;
        } else if (rng.Bounded(3U) == 0U) {
            const bool consumed = buf.ConsumeRelease(act, win);
            const bool exp      = (oracleRelease[act] <= win);
            CTS_CHECK(r, consumed == exp);
            if (exp) oracleRelease[act] = 0xFFU;
        } else if (rng.Bounded(10U) == 0U) {
            buf.Cancel(act);
            oraclePress[act]   = 0xFFU;
            oracleRelease[act] = 0xFFU;
        }
    }

    r.RecordFuzzOps(5000U);
    r.MixDigest(oraclePress[0]);
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "InputTests (CTS+DST)"};
    runner.RunCase("Sub-Frame Taps, Multi-Source Hold & Charge", CtsSubframeTapsRetriggersAndCharge);
    runner.RunCase("SOCD, Schmitt Hysteresis & Adversarial Map", CtsSocdSchmittHysteresisAndRebind);
    runner.RunCase("DST: 5,000-Tick ActionBuffer Oracle Fuzz", DstActionBufferOracleFuzz);
    return runner.Finish();
}
