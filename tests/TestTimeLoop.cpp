#include "core/time/ChannelModulator.h"
#include "core/time/SdlClock.h"
#include "core/time/TimePipeline.h"
#include "core/time/TimeTypes.h"
#include "core/time/TimeViews.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

using namespace en;
using namespace en::time;

namespace {

static u32 g_failedAssertions = 0;
static u32 g_passedAssertions = 0;

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


void TestCompileTimeConstantsAndLiterals() {
    static_assert(kSimHz == 120);
    static_assert(kNsPerSecond == 1'000'000'000ULL);
    static_assert(kFixedDtNs == 8'333'333ULL);
    static_assert(kMaxTimeChannels == 8);

    constexpr u32 ticksZero = 0_f60;
    constexpr u32 ticksSix  = 6_f60;
    constexpr u32 ticksFull = 60_f60;

    TEST_ASSERT(ticksZero == 0);
    TEST_ASSERT(ticksSix == 12);
    TEST_ASSERT(ticksFull == 120);
    TEST_ASSERT(std::abs(kFixedDtSec - (1.0f / 120.0f)) < 1e-6f);
}

void TestMemoryAlignmentAndCacheLines() {
    TEST_ASSERT(alignof(BakedTickState) == 64);
    TEST_ASSERT(sizeof(BakedTickState) == 64);
    TEST_ASSERT(alignof(BakedRenderState) == 32);
    TEST_ASSERT(sizeof(BakedRenderState) == 32);
}

void TestChannelConversionsAndSafety() {
    TEST_ASSERT(ToChannelIndex(TimeChannel::World) == 0);
    TEST_ASSERT(ToChannelIndex(TimeChannel::Player) == 1);
    TEST_ASSERT(ToChannelIndex(TimeChannel::Enemy) == 2);
    TEST_ASSERT(ToChannelIndex(TimeChannel::Projectile) == 3);
    TEST_ASSERT(ToChannelIndex(TimeChannel::Vfx) == 4);
    TEST_ASSERT(ToChannelIndex(TimeChannel::Camera) == 5);
    TEST_ASSERT(ToChannelIndex(TimeChannel::Ui) == 6);
    TEST_ASSERT(ToChannelIndex(TimeChannel::Custom) == 7);

    // Out-of-bounds channels must safely clamp to 0 (World)
    TEST_ASSERT(ToChannelIndex(static_cast<TimeChannel>(8)) == 0);
    TEST_ASSERT(ToChannelIndex(static_cast<TimeChannel>(255)) == 0);
    TEST_ASSERT(ToChannelIndex(3u) == 3);
}

void TestAccumulatorAndZeroDrift() {
    ManualClock clock{0};
    TimePipelineConfig cfg{
        .maxCatchupTicks   = 16,
        .maxFrameDeltaNs   = 1'000'000'000ULL,
        .vsyncSnapMarginNs = 0,
        .enableVsyncSnap   = false
    };
    TimePipeline<ManualClock> pipeline(cfg, clock);

    // Simulate 100 seconds with irregular, non-integer delta slices (7ms, 16ms, 23ms, 33ms)
    constexpr u64 deltasNs[] = {
        7'123'456ULL,
        16'666'666ULL,
        23'987'654ULL,
        33'333'333ULL,
        18'888'888ULL
    };

    u64 totalFeedNs = 0;
    u64 totalSimulatedTicks = 0;
    constexpr u64 targetNs = 100ULL * kNsPerSecond;

    usize deltaIdx = 0;
    while (totalFeedNs < targetNs) {
        u64 stepNs = deltasNs[deltaIdx % std::size(deltasNs)];
        if (totalFeedNs + stepNs > targetNs) {
            stepNs = targetNs - totalFeedNs;
        }
        totalFeedNs += stepNs;
        ++deltaIdx;

        pipeline.BeginFrameManual(stepNs);
        while (const auto step = pipeline.PollTick()) {
            (void)step;
            ++totalSimulatedTicks;
        }
    }

    // 100 seconds at 120Hz must yield exactly 12,000 ticks with zero remainder and zero dropped ticks
    TEST_ASSERT(totalSimulatedTicks == 12'000ULL);
    TEST_ASSERT(pipeline.Telemetry().totalSimTicks == 12'000ULL);
    TEST_ASSERT(pipeline.Telemetry().droppedTicks == 0);
    TEST_ASSERT(pipeline.Telemetry().accumulatorNs == 0);
}

void TestRenderAlphaInterpolation() {
    ManualClock clock{0};
    TimePipelineConfig cfg{
        .enableVsyncSnap = false
    };
    TimePipeline<ManualClock> pipeline(cfg, clock);

    pipeline.BeginFrameManual(0);
    while (pipeline.PollTick()) {}
    TEST_ASSERT(pipeline.GetRenderView().rawAlpha == 0.0f);

    // Half of a tick duration
    constexpr u64 halfTickNs = kFixedDtNs / 2;
    pipeline.BeginFrameManual(halfTickNs);
    while (pipeline.PollTick()) {}

    const auto renderView = pipeline.GetRenderView();
    TEST_ASSERT(std::abs(renderView.rawAlpha - 0.5f) < 0.01f);
}

void TestSpiralOfDeathClamping() {
    ManualClock clock{0};
    TimePipelineConfig cfg{
        .maxCatchupTicks = 8,
        .maxFrameDeltaNs = 250'000'000ULL, // 250ms = 30 ticks
        .enableVsyncSnap = false
    };
    TimePipeline<ManualClock> pipeline(cfg, clock);

    // Simulate huge stall of 500ms (clamped by maxFrameDeltaNs to 250ms = 30 ticks)
    pipeline.BeginFrameManual(500'000'000ULL);

    u32 ticksConsumed = 0;
    while (pipeline.PollTick()) {
        ++ticksConsumed;
    }

    // Must clamp to maxCatchupTicks = 8 ticks
    TEST_ASSERT(ticksConsumed == 8);
    TEST_ASSERT(pipeline.Telemetry().ticksLastFrame == 8);
    // 30 accumulated ticks - 8 consumed = 22 dropped ticks
    TEST_ASSERT(pipeline.Telemetry().droppedTicks == 22);
}

void TestVsyncHarmonicSnapping() {
    ManualClock clock{0};
    TimePipelineConfig cfg{
        .vsyncSnapMarginNs = 150'000ULL,
        .enableVsyncSnap   = true
    };
    TimePipeline<ManualClock> pipeline(cfg, clock);

    // 60Hz harmonic is kFixedDtNs * 2 = 16'666'666 ns.
    // Jitter within 150us margin (e.g. 16'600'000 ns -> 66.6us delta) snaps to exactly 16'666'666 ns.
    pipeline.BeginFrameManual(16'600'000ULL);
    while (pipeline.PollTick()) {}

    // Verify accumulator has exact remainder from snapped harmonic (16'666'666 * 120 = 1'999'999'920)
    constexpr u64 expectedAccNs = (kFixedDtNs * 2 * kSimHz % kNsPerSecond) / kSimHz;
    TEST_ASSERT(pipeline.Telemetry().accumulatorNs == expectedAccNs);
}

void TestPauseAndSingleStep() {
    ManualClock clock{0};
    TimePipeline<ManualClock> pipeline({}, clock);

    pipeline.SetPaused(true);
    TEST_ASSERT(pipeline.IsPaused());

    pipeline.BeginFrameManual(100'000'000ULL);
    u32 ticks = 0;
    while (pipeline.PollTick()) {
        ++ticks;
    }
    TEST_ASSERT(ticks == 0);
    TEST_ASSERT(pipeline.GetRenderView().rawAlpha == 1.0f);

    // Request single step
    pipeline.RequestSingleStep();
    pipeline.BeginFrameManual(0);
    ticks = 0;
    while (pipeline.PollTick()) {
        ++ticks;
    }
    TEST_ASSERT(ticks == 1);

    // Unpause and verify clean recovery
    pipeline.SetPaused(false);
    TEST_ASSERT(!pipeline.IsPaused());
}

void TestHitstopAndChannelModulation() {
    ManualClock clock{0};
    TimePipelineConfig cfg{.enableVsyncSnap = false};
    TimePipeline<ManualClock> pipeline(cfg, clock);

    constexpr u64 kOneSimTickNs = (kNsPerSecond + kSimHz - 1) / kSimHz;

    // Initial frame to prime pipeline
    pipeline.BeginFrameManual(kOneSimTickNs);
    if (const auto step = pipeline.PollTick()) {
        // Freeze Player for 3 subsequent ticks (4 ticks including active tick) with hard freeze (intensity 0.0f)
        step.RequestHitstop(TimeChannel::Player, 4, 0.0f);
        // Enemy receives partial slowdown for 2 subsequent ticks (intensity 0.25f)
        step.RequestHitstop(TimeChannel::Enemy, 3, 0.25f);
    }

    // Tick 1 of hitstop
    pipeline.BeginFrameManual(kOneSimTickNs);
    {
        const auto step = pipeline.PollTick();
        TEST_ASSERT(step);
        TEST_ASSERT(step.IsFrozen(TimeChannel::Player));
        TEST_ASSERT(step.Dt(TimeChannel::Player) == 0.0f);
        TEST_ASSERT(!step.IsFrozen(TimeChannel::World));
        TEST_ASSERT(step.Dt(TimeChannel::World) > 0.0f);
        TEST_ASSERT(!step.IsFrozen(TimeChannel::Enemy)); // Partial intensity is not hard frozen
        TEST_ASSERT(step.EffectiveScale(TimeChannel::Enemy) == 0.25f);

        // Frozen channels must pin render alpha to 1.0f
        const auto rv = pipeline.GetRenderView();
        TEST_ASSERT(rv.Alpha(TimeChannel::Player) == 1.0f);
    }

    // Tick 2 of hitstop
    pipeline.BeginFrameManual(kOneSimTickNs);
    {
        const auto step = pipeline.PollTick();
        TEST_ASSERT(step);
        TEST_ASSERT(step.IsFrozen(TimeChannel::Player));
        // Enemy hitstop (2 ticks) should expire after this tick closes
    }

    // Tick 3 of hitstop
    pipeline.BeginFrameManual(kOneSimTickNs);
    {
        const auto step = pipeline.PollTick();
        TEST_ASSERT(step);
        TEST_ASSERT(step.IsFrozen(TimeChannel::Player));
        TEST_ASSERT(step.EffectiveScale(TimeChannel::Enemy) == 1.0f);
    }

    // Tick 4: Player hitstop has expired, channel unfreezes
    pipeline.BeginFrameManual(kOneSimTickNs);
    {
        const auto step = pipeline.PollTick();
        TEST_ASSERT(step);
        TEST_ASSERT(!step.IsFrozen(TimeChannel::Player));
        TEST_ASSERT(step.Dt(TimeChannel::Player) > 0.0f);
    }
}

void TestTimeRampAndHitstopPause() {
    ChannelModulator modulator;

    // Ramp World channel to 0.5x, holding for 2 ticks, then blending out over 4 ticks
    modulator.RequestTimeRamp(TimeChannel::World, 0.5f, 2, 4);
    modulator.BakeTickState();

    // Tick 1: hold
    TEST_ASSERT(modulator.BakedTick().effectiveScale[0] == 0.5f);
    modulator.AdvanceTick();
    modulator.BakeTickState();

    // Tick 2: hold
    TEST_ASSERT(modulator.BakedTick().effectiveScale[0] == 0.5f);
    modulator.AdvanceTick();
    modulator.BakeTickState();

    // Tick 3: start of blend phase (elapsedBlend = 0 starts seamlessly at 0.5f)
    TEST_ASSERT(modulator.BakedTick().effectiveScale[0] == 0.5f);
    modulator.AdvanceTick(); // elapsedBlend becomes 1
    modulator.BakeTickState();
    const f32 blendScale1 = modulator.BakedTick().effectiveScale[0];
    TEST_ASSERT(blendScale1 > 0.5f && blendScale1 < 1.0f);

    // Freeze channel with hitstop: ramp progression must pause while hard-frozen
    modulator.RequestHitstop(TimeChannel::World, 2, 0.0f);
    modulator.BakeTickState();
    TEST_ASSERT(modulator.IsFrozen(TimeChannel::World));

    modulator.AdvanceTick();
    modulator.BakeTickState();
    modulator.AdvanceTick();
    modulator.BakeTickState();

    // Hitstop expired, ramp resumes blend progression
    TEST_ASSERT(!modulator.IsFrozen(TimeChannel::World));
}

} // namespace

int main() {
    std::cout << "Running EngineN Core Time Subsystem Tests...\n";

    TestCompileTimeConstantsAndLiterals();
    TestMemoryAlignmentAndCacheLines();
    TestChannelConversionsAndSafety();
    TestAccumulatorAndZeroDrift();
    TestRenderAlphaInterpolation();
    TestSpiralOfDeathClamping();
    TestVsyncHarmonicSnapping();
    TestPauseAndSingleStep();
    TestHitstopAndChannelModulation();
    TestTimeRampAndHitstopPause();

    std::cout << "Test Summary: " << g_passedAssertions << " passed, "
              << g_failedAssertions << " failed.\n";

    return (g_failedAssertions == 0) ? 0 : 1;
}
