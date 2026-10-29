#include "time/Clock.h"
#include "time/TimeSystem.h"

#include <cmath>
#include <cstdio>

namespace {

using namespace en;
using namespace en::literals;

bool TestFixedTimestepAndHarmonicSnap() noexcept {
    TimeSystem time;
    time.BeginFrame(16'666'667ULL + 50'000ULL);
    u32 count = 0U;
    while (const TickStep step = time.PollTick()) {
        if (std::abs(step.Dt() - kFixedDt) > 1e-6f) return false;
        ++count;
    }
    if (count != 2U || time.AccumulatorNs() != 0ULL) return false;
    if (std::abs(time.RenderView().Alpha()) > 1e-5f) return false;

    time.BeginFrame(4'166'667ULL);
    if (time.PollTick()) return false;
    if (std::abs(time.RenderView().Alpha() - 0.5f) > 1e-4f) return false;

    return true;
}

bool TestHitstopAndRamps() noexcept {
    TimeSystem time;
    time.Hitstop(TimeChannel::Player, 2_ticks);

    time.BeginFrame(kFixedDtNs * 3ULL);
    const TickStep t1 = time.PollTick();
    if (!t1 || !t1.IsFrozen(TimeChannel::Player) || t1.IsFrozen(TimeChannel::Ui)) return false;
    if (t1.Dt(TimeChannel::Player) != 0.0f) return false;

    const TickStep t2 = time.PollTick();
    if (!t2 || !t2.IsFrozen(TimeChannel::Player)) return false;

    const TickStep t3 = time.PollTick();
    if (!t3 || t3.IsFrozen(TimeChannel::Player) || std::abs(t3.Scale(TimeChannel::Player) - 1.0f) > 1e-5f) {
        return false;
    }

    time.SetPaused(true);
    time.Step(1U);
    const TickStep pausedStep = time.PollTick();
    if (!pausedStep || pausedStep.IsFrozen(TimeChannel::World)) return false;
    if (time.PollTick()) return false;
    if (time.RenderView().Alpha(TimeChannel::World) != 1.0f) return false;

    time.SetPaused(false);
    time.Ramp(TimeChannel::Enemy, 0.25f, 1U, 2U);
    time.BeginFrame(kFixedDtNs * 4ULL);
    const TickStep r1 = time.PollTick();
    if (!r1 || std::abs(r1.Scale(TimeChannel::Enemy) - 0.25f) > 1e-5f) return false;
    (void)time.PollTick();
    (void)time.PollTick();
    const TickStep r4 = time.PollTick();
    if (!r4 || std::abs(r4.Scale(TimeChannel::Enemy) - 1.0f) > 1e-5f) return false;

    return true;
}

bool TestClockAndFramePacer() noexcept {
    const u64 t0 = Clock::NowNs();
    FramePacer pacer(1000U);
    pacer.BeginFrame();
    pacer.EndFrame();
    const u64 t1 = Clock::NowNs();
    return t1 >= t0 && pacer.TargetFps() == 1000U;
}

} // namespace

int main() {
    if (!TestFixedTimestepAndHarmonicSnap()) return 1;
    if (!TestHitstopAndRamps()) return 2;
    if (!TestClockAndFramePacer()) return 3;
    std::printf("TimeTests: ALL PASSED\n");
    return 0;
}
