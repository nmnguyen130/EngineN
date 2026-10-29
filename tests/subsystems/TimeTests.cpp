#include "tests/subsystems/CtsHarness.h"

#include "time/Clock.h"
#include "time/TimeSystem.h"

namespace {

using namespace en;
using namespace en::cts;
using namespace en::literals;

bool CtsHarmonicRefreshRatesAndInterpolation(CtsRunner& r) noexcept {
    // All 14 exact divisors k of 1440 Hz: refreshHz = 1440 / k.
    constexpr u32 kDivisors[]{3, 4, 6, 8, 9, 10, 12, 16, 18, 20, 24, 30, 36, 48};

    for (const u32 k : kDivisors) {
        TimeSystem time;
        const u64 idealNs = (kNsPerSec * k) / kHarmonicHz;

        // Feed 120 frames with synthetic +-120us OS jitter; verify exact harmonic snapping.
        u64 totalTicks = 0ULL;
        for (u32 f = 0U; f < 120U; ++f) {
            const u64 jitteredNs = (f & 1U) != 0U ? (idealNs + 120'000ULL) : (idealNs - 120'000ULL);
            time.BeginFrame(jitteredNs);
            while (const TickStep step = time.PollTick()) {
                CTS_CHECK(r, Near(step.Dt(), kFixedDt, 1e-6f));
                ++totalTicks;
            }
        }
        CTS_CHECK(r, totalTicks == 10ULL * k);
        CTS_CHECK(r, time.AccumulatorNs() == 0ULL);
        CTS_CHECK(r, Near(time.RenderView().Alpha(), 0.0f, 1e-5f));
        r.RecordFuzzOps(120U);
    }

    // Sub-tick alpha verification at quarter, half, and three-quarter tick steps.
    TimeSystem sub;
    sub.BeginFrame(kFixedDtNs / 4ULL);
    CTS_CHECK(r, !sub.PollTick() && Near(sub.RenderView().Alpha(), 0.25f, 1e-3f));
    sub.BeginFrame(kFixedDtNs / 4ULL);
    CTS_CHECK(r, !sub.PollTick() && Near(sub.RenderView().Alpha(), 0.50f, 1e-3f));
    sub.BeginFrame(kFixedDtNs / 4ULL);
    CTS_CHECK(r, !sub.PollTick() && Near(sub.RenderView().Alpha(), 0.75f, 1e-3f));
    return true;
}

bool CtsChannelHierarchyHitstopAndRamps(CtsRunner& r) noexcept {
    TimeSystem time;
    time.SetMasterScale(0.5f);
    time.SetScale(TimeChannel::World, 0.5f);
    time.SetScale(TimeChannel::Player, 2.0f);
    time.SetScale(TimeChannel::Enemy, 0.5f);

    time.BeginFrame(kFixedDtNs);
    const TickStep s0 = time.PollTick();
    // World = 0.5 * 0.5 = 0.25; Player = World * 2.0 = 0.5; Enemy = World * 0.5 = 0.125; Ui = always 1.0.
    CTS_CHECK(r, s0 && Near(s0.Scale(TimeChannel::World), 0.25f));
    CTS_CHECK(r, Near(s0.Scale(TimeChannel::Player), 0.50f));
    CTS_CHECK(r, Near(s0.Scale(TimeChannel::Enemy), 0.125f));
    CTS_CHECK(r, Near(s0.Scale(TimeChannel::Ui), 1.0f));

    // Overlapping MaskHitstop takes max duration; World freeze cascades to all gameplay channels.
    time.Reset();
    time.MaskHitstop(ChannelBit(TimeChannel::Player) | ChannelBit(TimeChannel::Enemy), 2_ticks);
    time.Hitstop(TimeChannel::Player, 4_ticks);
    time.Hitstop(TimeChannel::Player, 1_ticks);

    time.BeginFrame(kFixedDtNs * 5ULL);
    for (u32 t = 1U; t <= 5U; ++t) {
        const TickStep st = time.PollTick();
        CTS_CHECK(r, static_cast<bool>(st));
        CTS_CHECK(r, st.IsFrozen(TimeChannel::Player) == (t <= 4U));
        CTS_CHECK(r, st.IsFrozen(TimeChannel::Enemy) == (t <= 2U));
        CTS_CHECK(r, !st.IsFrozen(TimeChannel::Ui));
    }

    // SmoothStep time-scale ramp verification: monotonic recovery from 0.2f -> 1.0f.
    time.Ramp(TimeChannel::Enemy, 0.2f, 2U, 4U);
    time.BeginFrame(kFixedDtNs * 7ULL);
    f32 prevScale = 0.2f;
    for (u32 t = 1U; t <= 7U; ++t) {
        const TickStep st = time.PollTick();
        const f32 sc = st.Scale(TimeChannel::Enemy);
        if (t <= 3U) {
            CTS_CHECK(r, Near(sc, 0.2f));
        } else if (t <= 6U) {
            CTS_CHECK(r, sc > prevScale && sc < 1.0f);
        } else {
            CTS_CHECK(r, Near(sc, 1.0f));
        }
        prevScale = sc;
    }
    return true;
}

bool CtsPauseStepBudgetAndSpiralProtection(CtsRunner& r) noexcept {
    TimeSystem time;
    time.BeginFrame(1'000'000'000ULL);
    CTS_CHECK(r, time.Stats().clampedFrames == 1U);
    CTS_CHECK(r, time.Stats().droppedTicks == 22U);

    u32 executed = 0U;
    while (time.PollTick()) ++executed;
    CTS_CHECK(r, executed == 8U);

    // Paused frame-stepping budget verification.
    time.SetPaused(true);
    CTS_CHECK(r, time.IsPaused() && Near(time.RenderView().Alpha(TimeChannel::World), 1.0f));
    time.BeginFrame(kFixedDtNs * 4ULL);
    CTS_CHECK(r, !time.PollTick());

    time.Step(3U);
    for (u32 i = 0U; i < 3U; ++i) {
        const TickStep st = time.PollTick();
        CTS_CHECK(r, st && !st.IsFrozen(TimeChannel::World));
    }
    CTS_CHECK(r, !time.PollTick() && Near(time.RenderView().Alpha(TimeChannel::World), 1.0f));
    return true;
}

bool DstTimeSystemLockstepReplay(CtsRunner& r) noexcept {
    TimeSystem simA;
    TimeSystem simB;
    Rng rngA(0x714E5EED2026ULL);
    Rng rngB(0x714E5EED2026ULL);

    for (u32 frame = 0U; frame < 5000U; ++frame) {
        const u32 action = rngA.Bounded(20U);
        (void)rngB.Bounded(20U);
        if (action == 0U) {
            const auto ch = static_cast<TimeChannel>(rngA.Bounded(4U));
            (void)rngB.Bounded(4U);
            const u16 ticks = static_cast<u16>(rngA.Range(1, 6));
            (void)rngB.Range(1, 6);
            simA.Hitstop(ch, ticks);
            simB.Hitstop(ch, ticks);
        } else if (action == 1U) {
            const auto ch = static_cast<TimeChannel>(rngA.Bounded(4U));
            (void)rngB.Bounded(4U);
            const f32 target = rngA.Range(0.1f, 2.5f);
            (void)rngB.Range(0.1f, 2.5f);
            simA.Ramp(ch, target, 2U, 4U);
            simB.Ramp(ch, target, 2U, 4U);
        }

        const u64 dtNs = static_cast<u64>(rngA.Range(2'000'000, 35'000'000));
        (void)rngB.Range(2'000'000, 35'000'000);
        simA.BeginFrame(dtNs);
        simB.BeginFrame(dtNs);

        for (;;) {
            const TickStep a = simA.PollTick();
            const TickStep b = simB.PollTick();
            CTS_CHECK(r, a.active == b.active);
            if (!a) break;
            CTS_CHECK(r, a.tick == b.tick && a.substep == b.substep && a.frozenMask == b.frozenMask);
            for (u8 c = 0U; c < kGameplayChannels; ++c) {
                CTS_CHECK(r, a.dt[c] == b.dt[c]);
            }
        }
        CTS_CHECK(r, simA.AccumulatorNs() == simB.AccumulatorNs());
        CTS_CHECK(r, simA.RenderView().alpha == simB.RenderView().alpha);
    }

    const u64 t0 = Clock::NowNs();
    FramePacer pacer(2000U);
    pacer.BeginFrame();
    pacer.EndFrame();
    CTS_CHECK(r, Clock::NowNs() >= t0 && pacer.TargetFps() == 2000U);

    r.RecordFuzzOps(5000U);
    r.MixDigest(simA.Stats().totalTicks ^ simA.AccumulatorNs());
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "TimeTests (CTS+DST)"};
    runner.RunCase("1440 Hz Harmonic VSync Snapping & Alpha", CtsHarmonicRefreshRatesAndInterpolation);
    runner.RunCase("Cascading Channels, Hitstop & Smooth Ramps", CtsChannelHierarchyHitstopAndRamps);
    runner.RunCase("Pause, Step Budget & Spiral Clamping", CtsPauseStepBudgetAndSpiralProtection);
    runner.RunCase("DST: 5,000-Frame Lockstep Replay", DstTimeSystemLockstepReplay);
    return runner.Finish();
}
