#include "core/time/TimeManager.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>

namespace {

// StepFrame: advances dtNs -> returns ticks consumed
uint32_t StepFrame(EngineN::TimeManager& tm, uint64_t dtNs) {
    tm.BeginFrameManual(dtNs);
    uint32_t ticks = 0;
    while (tm.ConsumeTick()) {
        ++ticks;
        tm.EndTick();
    }
    tm.EndFrame();
    return ticks;
}

void TestTimestepInvariants() {
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // dt = 0 -> 0 ticks, alpha = 0.0
    assert(StepFrame(tm, 0) == 0 && tm.GetAlpha() == 0.0);

    // dt = 1ms (< 8.33ms fixed dt) -> 0 ticks, acc = 1ms
    assert(StepFrame(tm, 1'000'000ULL) == 0);
    assert(tm.GetAccumulatorNs() == 1'000'000ULL);

    // dt = 24ms -> acc = 1ms + 24ms = 25ms @ 120Hz -> 3 ticks, rem = 0
    assert(StepFrame(tm, 24'000'000ULL) == 3);
    assert(tm.GetAccumulatorNsHz() == 0);
    assert(tm.GetAccumulatorNs() == 0);
    assert(tm.GetAlpha() == 0.0);

    // dt = 10ms -> 1 tick (8.33ms), rem = 1.66ms -> alpha in (0, 1)
    assert(StepFrame(tm, 10'000'000ULL) == 1);
    assert(tm.GetAccumulatorNs() < tm.GetFixedDtNs());
    assert(tm.GetAlpha() > 0.0 && tm.GetAlpha() < 1.0);

    // 60 frames @ 60Hz (~16.66ms) -> total dt = 1s @ 120Hz -> 120 ticks, rem = 0
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    uint32_t totalSecTicks = 0;
    uint64_t simTimeNs = 0;
    for (int i = 0; i < 60; ++i) {
        const uint64_t frameDt = (i % 3 == 0) ? 16'666'668ULL : 16'666'666ULL;
        simTimeNs += frameDt;
        totalSecTicks += StepFrame(tm, frameDt);
    }
    assert(simTimeNs == 1'000'000'000ULL);
    assert(totalSecTicks == 120);
    assert(tm.GetAccumulatorNsHz() == 0);
    assert(tm.GetAccumulatorNs() < tm.GetFixedDtNs());
    assert(tm.GetAlpha() >= 0.0 && tm.GetAlpha() < 1.0);
}

void TestSpiralOfDeathAndClamping() {
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // dt = 100ms (12 ticks requested) -> clamp to maxCatchupTicks = 8 ticks
    assert(StepFrame(tm, 100'000'000ULL) == 8);
    assert(tm.WasTimeDropped());

    // dt = 1s (> maxFrameNs = 250ms) -> clamp to 8 ticks
    assert(StepFrame(tm, 1'000'000'000ULL) == 8);

    // dt = UINT64_MAX -> clamp to 8 ticks, no overflow
    assert(StepFrame(tm, UINT64_MAX) == 8);

    // Backlog dropped -> accNsHz < 1s (phase preserved)
    assert(tm.GetAccumulatorNsHz() < EngineN::kNsPerSec);

    // 50 x 200ms consecutive spikes -> each <= 8 ticks, acc < fixedDt
    for (int i = 0; i < 50; ++i) {
        const uint32_t ticks = StepFrame(tm, 200'000'000ULL);
        assert(ticks <= 8);
        assert(tm.GetAccumulatorNs() < tm.GetFixedDtNs());
    }
}

void TestPauseResumeAndRefocus() {
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // dt = 4ms -> acc residual > 0
    StepFrame(tm, 4'000'000ULL);
    const uint64_t residualNsHz = tm.GetAccumulatorNsHz();
    assert(residualNsHz > 0);

    // Paused: dt = 5s -> 0 ticks, residual preserved
    tm.SetPaused(true);
    assert(tm.IsPaused());
    assert(StepFrame(tm, 5'000'000'000ULL) == 0);
    assert(tm.GetAccumulatorNsHz() == residualNsHz);

    // Resume: dt = 21ms (+ 4ms residual = 25ms @ 120Hz) -> 3 ticks, rem = 0
    tm.SetPaused(false);
    assert(!tm.IsPaused());
    assert(StepFrame(tm, 21'000'000ULL) == 3);
    assert(tm.GetAccumulatorNsHz() == 0);

    // Refocus reset -> acc = 0, alpha = 0.0
    StepFrame(tm, 4'000'000ULL);
    tm.ResetClocksOnRefocus();
    assert(tm.GetAccumulatorNsHz() == 0);
    assert(tm.GetAlpha() == 0.0);
}

void TestDeterminismAndTimeScaling() {
    // Determinism: seq(dt) -> tmA == tmB (ticks, accNsHz, pos)
    const uint64_t sequence[] = { 16'200'000ULL, 8'333'333ULL, 25'000'000ULL, 4'100'000ULL, 16'666'666ULL };
    EngineN::TimeManager tmA, tmB;
    tmA.Init();
    tmB.Init();

    auto runFrame = [](EngineN::TimeManager& tm, float& pos, uint64_t dt) {
        tm.BeginFrameManual(dt);
        while (tm.ConsumeTick()) {
            pos += 100.0f * static_cast<float>(tm.PlayerTime().GetScaledDt());
            tm.EndTick();
        }
        tm.EndFrame();
    };

    float posA = 0.0f;
    float posB = 0.0f;
    for (uint64_t dt : sequence) {
        runFrame(tmA, posA, dt);
        runFrame(tmB, posB, dt);
    }
    assert(tmA.GetSimTickCount() == tmB.GetSimTickCount());
    assert(tmA.GetAccumulatorNsHz() == tmB.GetAccumulatorNsHz());
    assert(posA == posB);

    // scale = 0.5 -> scaleQ16 = 32768, scaledDt = fixedDt * 0.5
    EngineN::TimeManager tm;
    tm.Init();
    tm.PlayerTime().SetTimeScale(0.5);
    assert(tm.PlayerTime().GetScaleQ16() == EngineN::ToQ16(0.5f));

    tm.BeginFrameManual(25'000'000ULL); // 25ms @ 120Hz -> 3 ticks
    assert(tm.ConsumeTick());
    assert(std::abs(tm.PlayerTime().GetScaledDt() - tm.GetFixedDtSec() * 0.5) < 1e-7);
    tm.EndTick();
    while (tm.ConsumeTick()) {
        tm.EndTick();
    }
    tm.EndFrame();

    // Domain alpha in [0.0, 1.0]
    const double domAlpha = tm.GetDomainAlpha(EngineN::ChannelId::Player);
    assert(domAlpha >= 0.0 && domAlpha <= 1.0);

    // RealTime -> unscaled wall clock, scaledDt > 0
    assert(tm.RealTime().GetScaledDt() > 0.0);

    // Invalid scale (scale < 0, NaN) -> reject, keep 0.5
    tm.PlayerTime().SetTimeScale(-2.0);
    assert(tm.PlayerTime().GetTimeScale() == 0.5);
    tm.PlayerTime().SetTimeScale(NAN);
    assert(tm.PlayerTime().GetTimeScale() == 0.5);
}

void TestHitstopEngine() {
    EngineN::HitstopEngine hitstop;

    // dur <= 0 (0.0, -0.05) -> ignored, modifier = 1.0
    hitstop.Request({.channel = EngineN::HitstopChannel::Player, .durationSec = 0.0});
    hitstop.Request({.channel = EngineN::HitstopChannel::Player, .durationSec = -0.05});
    assert(!hitstop.IsFrozen(EngineN::HitstopChannel::Player));
    assert(hitstop.GetModifier(EngineN::HitstopChannel::Player) == 1.0);

    // Freeze req = 0.1s -> modifier = 0.0, rem = 0.1s
    hitstop.Request({.channel = EngineN::HitstopChannel::Player, .durationSec = 0.100});
    assert(hitstop.IsFrozen(EngineN::HitstopChannel::Player));
    assert(hitstop.GetModifier(EngineN::HitstopChannel::Player) == 0.0);
    assert(hitstop.GetRemainingSec(EngineN::HitstopChannel::Player) == 0.100);

    // Merge-by-max: req = 0.05s (< 0.1s) -> keep 0.1s
    hitstop.Request({.channel = EngineN::HitstopChannel::Player, .durationSec = 0.050});
    hitstop.Update(0.060);
    assert(hitstop.IsFrozen(EngineN::HitstopChannel::Player));
    hitstop.Update(0.040); // 0.06s + 0.04s = 0.10s -> expires, modifier = 1.0
    assert(!hitstop.IsFrozen(EngineN::HitstopChannel::Player));
    assert(hitstop.GetModifier(EngineN::HitstopChannel::Player) == 1.0);

    // Reset -> flush freeze, modifier = 1.0
    hitstop.Request({.channel = EngineN::HitstopChannel::World, .durationSec = 0.500});
    hitstop.Reset();
    assert(!hitstop.IsFrozen(EngineN::HitstopChannel::World));
}

void TestFramePacerAndClock() {
    // Clock: prev > now (backward jump) -> delta = 0
    EngineN::HighResolutionClock clk;
    clk.Init();
    clk.SetPreviousTicksNs(SDL_GetTicksNS() + 1'000'000'000ULL);
    assert(clk.Sample() == 0ULL);

    // Pacer FPS: 0 -> 0ns, 120 -> 8.33ms, 60 -> 16.66ms
    EngineN::FramePacer pacer;
    pacer.SetTargetFps(0);
    assert(pacer.GetTargetFps() == 0 && pacer.GetTargetFrameNs() == 0ULL);
    pacer.SetTargetFps(120);
    assert(pacer.GetTargetFps() == 120 && pacer.GetTargetFrameNs() == 8'333'333ULL);
    pacer.SetTargetFps(60);
    assert(pacer.GetTargetFps() == 60 && pacer.GetTargetFrameNs() == 16'666'666ULL);

    // Deadline: d2 - d1 == targetFrameNs (zero drift)
    pacer.WaitForTarget();
    const uint64_t d1 = pacer.GetTargetDeadlineNs();
    pacer.WaitForTarget();
    const uint64_t d2 = pacer.GetTargetDeadlineNs();
    assert(d2 - d1 == 16'666'666ULL);

    pacer.Reset();
    assert(pacer.GetTargetDeadlineNs() == 0);
}

void TestEnduranceAndFuzzingInvariants() {
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // 10k random dt in [0, 2s] -> ticks <= 8, acc < fixedDt, alpha in [0, 1)
    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<uint64_t> distDt(0ULL, 2'000'000'000ULL);

    for (int i = 0; i < 10'000; ++i) {
        const uint32_t ticks = StepFrame(tm, distDt(rng));
        assert(ticks <= 8);
        assert(tm.GetAccumulatorNs() < tm.GetFixedDtNs());
        assert(tm.GetAlpha() >= 0.0 && tm.GetAlpha() < 1.0);
    }

    // 100k frames 144Hz jitter (6.944444ms / 6.944445ms) -> test zero cumulative drift
    EngineN::TimeManager tmJitter;
    tmJitter.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    uint64_t totalNs = 0;
    uint64_t totalTicks = 0;
    for (int i = 0; i < 100'000; ++i) {
        const uint64_t dt = (i % 2 == 0) ? 6'944'444ULL : 6'944'445ULL;
        totalNs += dt;
        totalTicks += StepFrame(tmJitter, dt);
    }

    // totalTicks == floor(totalNs * 120 / 1e9)
    const uint64_t expectedTicks = (totalNs * 120ULL) / EngineN::kNsPerSec;
    assert(totalTicks == expectedTicks);
    assert(tmJitter.GetSimTickCount() == expectedTicks);

    // accNsHz == (totalNs * 120) % 1e9
    const uint64_t expectedRemNsHz = (totalNs * 120ULL) % EngineN::kNsPerSec;
    assert(tmJitter.GetAccumulatorNsHz() == expectedRemNsHz);
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << "\n";
        return -1;
    }

    std::cout << "[TestTimeLoop] Running Deterministic Metroidvania TimeLoop Test Matrix...\n";

    TestTimestepInvariants();
    std::cout << "  -> 1. Fixed Timestep & Accumulator Invariants: PASSED\n";

    TestSpiralOfDeathAndClamping();
    std::cout << "  -> 2. Spiral-of-Death & Clamping: PASSED\n";

    TestPauseResumeAndRefocus();
    std::cout << "  -> 3. Pause, Resume & Refocus Safety: PASSED\n";

    TestDeterminismAndTimeScaling();
    std::cout << "  -> 4. Simulation Determinism & Channel Scaling: PASSED\n";

    TestHitstopEngine();
    std::cout << "  -> 5. Combat Hitstop (Non-Stacking Merge): PASSED\n";

    TestFramePacerAndClock();
    std::cout << "  -> 6. Frame Pacer & Clock Anomaly: PASSED\n";

    TestEnduranceAndFuzzingInvariants();
    std::cout << "  -> 7. Fuzzing & Long-Run Zero-Drift Invariants: PASSED\n";

    std::cout << "[TestTimeLoop] All TimeLoop Invariant Suites Passed Successfully!\n";

    SDL_Quit();
    return 0;
}
