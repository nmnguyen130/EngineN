#include "time/TimePipeline.h"
#include <cmath>
#include <cstdio>
#include <limits>

namespace en::tests {

struct TestStats {
    u32 passed{0};
    u32 failed{0};
};

#define EN_TEST_CHECK(stats, ...)                                                                \
    do {                                                                                         \
        if (__VA_ARGS__) {                                                                       \
            ++(stats).passed;                                                                    \
        } else {                                                                                 \
            ++(stats).failed;                                                                    \
            std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);      \
        }                                                                                        \
    } while (false)

// Compile-time verification proving Layer 1 TimePipeline is 100% pure constexpr C++23.
consteval bool VerifyConstexprLayer1Pipeline() noexcept {
    TimePipeline tp{};
    tp.BeginFrameManual(kFixedDtNs * 2ULL);
    const SimStepView s1 = tp.PollTick();
    const SimStepView s2 = tp.PollTick();
    const SimStepView s3 = tp.PollTick();
    if (!s1.valid || !s2.valid || s3.valid) return false;
    if (s1.substepTotal != 2U || s2.tickIndex != 2ULL) return false;

    tp.RequestHitstop(TimeChannel::World, 1U);
    if ((tp.FrozenMask() & kGameplayChannelsMask) != kGameplayChannelsMask) return false;
    tp.BeginFrameManual(kFixedDtNs);
    const SimStepView hs = tp.PollTick();
    if (!hs.IsFrozen(TimeChannel::Player) || hs.IsFrozen(TimeChannel::Ui)) return false;
    if (tp.FrozenMask() != 0U) return false;

    return tp.ComputeAuditHash() != 0ULL;
}
static_assert(VerifyConstexprLayer1Pipeline());

void Test1440HzHarmonicSnappingAndVrrFilter(TestStats& stats) noexcept {
    using namespace en::literals;
    static_assert(6_f60 == 12U);
    static_assert(5_ticks == 5U);

    TimePipeline time{};

    // 1. 144 Hz display (6,944,444 ns +- 75,000 ns jitter): 6 frames must equal 5 exact 120Hz ticks with 0 remainder
    u32 ticks144Hz = 0U;
    for (u32 frame = 0; frame < 6U; ++frame) {
        const u64 jittered144Ns = (frame & 1U) ? (6'944'444ULL + 75'000ULL) : (6'944'444ULL - 75'000ULL);
        time.BeginFrameManual(jittered144Ns);
        while (time.PollTick()) {
            ++ticks144Hz;
        }
    }
    EN_TEST_CHECK(stats, ticks144Hz == 5U && time.AccumulatorNs() == 0ULL);
    EN_TEST_CHECK(stats, std::abs(time.GetRenderView().rawAlpha) < 1e-6f);

    // 2. 90 Hz Steam Deck OLED (11,111,111 ns +- 90,000 ns jitter): 3 frames must equal 4 exact 120Hz ticks with 0 remainder
    time.Reset();
    u32 ticks90Hz = 0U;
    for (u32 frame = 0; frame < 3U; ++frame) {
        time.BeginFrameManual(11'111'111ULL + 90'000ULL);
        while (time.PollTick()) {
            ++ticks90Hz;
        }
    }
    EN_TEST_CHECK(stats, ticks90Hz == 4U && time.AccumulatorNs() == 0ULL);

    // 3. 360 Hz display (2,777,778 ns - 50,000 ns jitter): 3 frames must equal 1 exact 120Hz tick with 0 remainder
    time.Reset();
    u32 ticks360Hz = 0U;
    for (u32 frame = 0; frame < 3U; ++frame) {
        time.BeginFrameManual(2'777'778ULL - 50'000ULL);
        while (time.PollTick()) {
            ++ticks360Hz;
        }
    }
    EN_TEST_CHECK(stats, ticks360Hz == 1U && time.AccumulatorNs() == 0ULL);

    // 4. 240 Hz display (4,166,666 ns + 80,000 ns jitter): 1st frame -> alpha == 0.5f, 2nd frame -> 1 tick, alpha == 0.0f
    time.Reset();
    time.BeginFrameManual(4'166'666ULL + 80'000ULL);
    EN_TEST_CHECK(stats, !time.PollTick().valid);
    EN_TEST_CHECK(stats, std::abs(time.GetRenderView().rawAlpha - 0.5f) < 1e-6f);
    time.BeginFrameManual(4'166'666ULL - 80'000ULL);
    EN_TEST_CHECK(stats, time.PollTick().valid && time.AccumulatorNs() == 0ULL);

    // 5. 40 Hz Steam Deck LCD (25,000,000 ns - 100,000 ns jitter) -> 3 exact ticks per frame
    time.Reset();
    time.BeginFrameManual(24'900'000ULL);
    u32 ticks40Hz = 0U;
    while (const SimStepView step = time.PollTick()) {
        EN_TEST_CHECK(stats, step.substepTotal == 3U);
        ++ticks40Hz;
    }
    EN_TEST_CHECK(stats, ticks40Hz == 3U && time.AccumulatorNs() == 0ULL);

    // 6. VRR unsnapped micro-jitter EWMA smoothing (within +-20% band)
    time.Reset();
    time.BeginFrameManual(10'000'000ULL); // Establish 100 FPS VRR baseline (10 ms)
    while (time.PollTick()) {}
    const u64 remBefore = time.AccumulatorNs();
    time.BeginFrameManual(10'800'000ULL); // +8% spike smoothed toward 10.2 ms instead of raw 10.8 ms
    const u64 smoothedAdvanceNs = time.AccumulatorNs() - remBefore;
    EN_TEST_CHECK(stats, smoothedAdvanceNs == 10'200'000ULL);

    // 7. Spiral-of-death clamp (1 second spike clamps to 250ms and maxCatchupTicks = 8)
    time.Reset();
    time.BeginFrameManual(1'000'000'000ULL);
    EN_TEST_CHECK(stats, time.Telemetry().droppedTicks > 0U);
    u32 catchupCount = 0U;
    while (const SimStepView step = time.PollTick()) {
        EN_TEST_CHECK(stats, step.substepTotal == 8U);
        ++catchupCount;
    }
    EN_TEST_CHECK(stats, catchupCount == 8U && time.Telemetry().ticksLastFrame == 8U);
}

void TestHierarchyHitstopAndHermiteRamps(TestStats& stats) noexcept {
    TimePipeline time{};

    // 1. World base scale cascades to Player..Camera (1..5), while Ui (6) & Custom (7) stay at 1.0f
    time.SetBaseScale(TimeChannel::World, 0.25f);
    time.BeginFrameManual(kFixedDtNs);
    const SimStepView sBase = time.PollTick();
    EN_TEST_CHECK(stats, std::abs(sBase.EffectiveScale(TimeChannel::World) - 0.25f) < 1e-5f);
    EN_TEST_CHECK(stats, std::abs(sBase.EffectiveScale(TimeChannel::Player) - 0.25f) < 1e-5f);
    EN_TEST_CHECK(stats, std::abs(sBase.EffectiveScale(TimeChannel::Enemy) - 0.25f) < 1e-5f);
    EN_TEST_CHECK(stats, std::abs(sBase.EffectiveScale(TimeChannel::Ui) - 1.0f) < 1e-5f);
    EN_TEST_CHECK(stats, std::abs(sBase.EffectiveScale(TimeChannel::Custom) - 1.0f) < 1e-5f);
    time.SetBaseScale(TimeChannel::World, 1.0f);

    // 2. Parent Hitstop on World freezes all child channels (Player..Camera) AND unfreezes them on expiration
    time.RequestHitstop(TimeChannel::World, 2U);
    EN_TEST_CHECK(stats, (time.FrozenMask() & kGameplayChannelsMask) == kGameplayChannelsMask);
    EN_TEST_CHECK(stats, (time.FrozenMask() & (1U << static_cast<u8>(TimeChannel::Ui))) == 0U);

    time.BeginFrameManual(kFixedDtNs / 2ULL);
    const RenderTimeView frozenRv = time.GetRenderView();
    EN_TEST_CHECK(stats, frozenRv.Alpha(TimeChannel::Player) == 1.0f);
    EN_TEST_CHECK(stats, frozenRv.Alpha(TimeChannel::Ui) < 0.6f);

    time.BeginFrameManual(kFixedDtNs);
    const SimStepView hs1 = time.PollTick();
    EN_TEST_CHECK(stats, hs1.IsFrozen(TimeChannel::World) && hs1.IsFrozen(TimeChannel::Player));
    EN_TEST_CHECK(stats, hs1.Dt(TimeChannel::Player) == 0.0f && !hs1.IsFrozen(TimeChannel::Ui));

    time.BeginFrameManual(kFixedDtNs);
    const SimStepView hs2 = time.PollTick();
    EN_TEST_CHECK(stats, hs2.IsFrozen(TimeChannel::Player));

    // After hitstop expiration, BOTH World and child channels (Player..Camera) are unfrozen
    EN_TEST_CHECK(stats, time.FrozenMask() == 0U);
    time.BeginFrameManual(kFixedDtNs);
    const SimStepView hs3 = time.PollTick();
    EN_TEST_CHECK(stats, !hs3.IsFrozen(TimeChannel::World) && !hs3.IsFrozen(TimeChannel::Player));
    EN_TEST_CHECK(stats, std::abs(hs3.Dt(TimeChannel::Player) - kFixedDtSec) < 1e-6f);

    // 3. C1 Hermite SmoothStep TimeRamp on World cascades to child channels
    time.RequestTimeRamp(TimeChannel::World, 0.2f, 1U, 4U);
    time.BeginFrameManual(kFixedDtNs);
    const SimStepView rHold = time.PollTick(); // Hold tick 0 -> scale == 0.2f
    EN_TEST_CHECK(stats, std::abs(rHold.EffectiveScale(TimeChannel::Player) - 0.2f) < 1e-5f);

    time.BeginFrameManual(kFixedDtNs);
    const SimStepView rBlend1 = time.PollTick(); // rawT = 1/4 = 0.25 -> SmoothStep(0.25) = 0.15625 -> 0.2 + 0.8*0.15625 = 0.325
    EN_TEST_CHECK(stats, std::abs(rBlend1.EffectiveScale(TimeChannel::Player) - 0.325f) < 1e-4f);

    time.BeginFrameManual(kFixedDtNs);
    const SimStepView rBlend2 = time.PollTick(); // rawT = 2/4 = 0.50 -> SmoothStep(0.50) = 0.5 -> 0.6
    EN_TEST_CHECK(stats, std::abs(rBlend2.EffectiveScale(TimeChannel::Player) - 0.6f) < 1e-4f);

    time.ClearChannelEffects(TimeChannel::World);
    EN_TEST_CHECK(stats, std::abs(time.EffectiveScales()[static_cast<u8>(TimeChannel::Player)] - 1.0f) < 1e-5f);
}

void TestTruePauseSteppingAndGameplayFreeze(TestStats& stats) noexcept {
    TimePipeline time{};

    // Trigger a 2-tick hitstop on Player, then pause the simulation clock
    time.RequestHitstop(TimeChannel::Player, 2U);
    time.SetPaused(true);
    EN_TEST_CHECK(stats, (time.FrozenMask() & kGameplayChannelsMask) == kGameplayChannelsMask);
    EN_TEST_CHECK(stats, time.EffectiveScales()[static_cast<u8>(TimeChannel::Player)] == 0.0f);
    EN_TEST_CHECK(stats, time.EffectiveScales()[static_cast<u8>(TimeChannel::Ui)] == 1.0f);

    // While paused without RequestStep, BeginFrameManual updates realDt for UI but PollTick does NOT advance totalSimTicks
    for (u32 i = 0; i < 10U; ++i) {
        time.BeginFrameManual(kFixedDtNs);
        EN_TEST_CHECK(stats, !time.PollTick().valid);
    }
    EN_TEST_CHECK(stats, time.Telemetry().totalSimTicks == 0ULL);

    // RequestStep(1) followed by BeginFrameManual(kFixedDtNs) executes EXACTLY 1 tick (no double-accumulation!)
    time.RequestStep(1U);
    time.BeginFrameManual(kFixedDtNs);
    const SimStepView stepped1 = time.PollTick();
    const SimStepView stepped2 = time.PollTick();
    EN_TEST_CHECK(stats, stepped1.valid && !stepped2.valid);
    EN_TEST_CHECK(stats, stepped1.IsFrozen(TimeChannel::Player));
    EN_TEST_CHECK(stats, (time.FrozenMask() & kGameplayChannelsMask) == kGameplayChannelsMask);

    // Unpause: Player still has 1 hitstop tick remaining
    time.SetPaused(false);
    time.BeginFrameManual(kFixedDtNs);
    EN_TEST_CHECK(stats, time.PollTick().IsFrozen(TimeChannel::Player));
    EN_TEST_CHECK(stats, time.FrozenMask() == 0U);
}

void TestSdlClockAndFramePacer(TestStats& stats) noexcept {
    TimePipeline timeA{};
    TimePipeline timeB{};
    timeA.BeginFrameManual(kFixedDtNs);
    timeB.BeginFrameManual(kFixedDtNs);
    (void)timeA.PollTick();
    (void)timeB.PollTick();
    EN_TEST_CHECK(stats, timeA.ComputeAuditHash() == timeB.ComputeAuditHash());

    timeB.SetMasterScale(0.5f);
    EN_TEST_CHECK(stats, timeA.ComputeAuditHash() != timeB.ComputeAuditHash());

    timeB.SetMasterScale(-5.0f);
    EN_TEST_CHECK(stats, timeB.MasterScale() == 0.0f);
    timeB.SetMasterScale(std::numeric_limits<f32>::quiet_NaN());
    EN_TEST_CHECK(stats, timeB.MasterScale() == 0.0f);

    // Layer 2 SdlClock & FramePacer verification
    (void)SdlClock::NowNs();
    SdlClock::SleepNs(1'000'000ULL);
    const u64 t0 = SdlClock::NowNs();
    timeA.BeginFrame();
    const u64 t1 = SdlClock::NowNs();
    EN_TEST_CHECK(stats, t0 > 0ULL && t1 >= t0);

    FramePacer pacer{240U};
    EN_TEST_CHECK(stats, pacer.TargetFps() == 240U && pacer.TargetFrameNs() > 0ULL);
    EN_TEST_CHECK(stats, pacer.SleepMarginNs() == FramePacer::kDefaultSpinMarginNs);
    pacer.BeginFrame();
    pacer.EndFrameAndPace();
    EN_TEST_CHECK(stats, pacer.SleepMarginNs() >= FramePacer::kMinSpinMarginNs &&
                         pacer.SleepMarginNs() <= FramePacer::kMaxSpinMarginNs);
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestStats stats{};

    Test1440HzHarmonicSnappingAndVrrFilter(stats);
    TestHierarchyHitstopAndHermiteRamps(stats);
    TestTruePauseSteppingAndGameplayFreeze(stats);
    TestSdlClockAndFramePacer(stats);

    std::printf(
        "EngineN Time Subsystem Verification Suite: %u passed, %u failed\n",
        stats.passed,
        stats.failed);
    return (stats.failed == 0U) ? 0 : 1;
}
