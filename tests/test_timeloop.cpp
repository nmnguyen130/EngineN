#include "core/time/TimeManager.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>

namespace {

// Helper: advances a frame manually and returns the number of ticks consumed
uint32_t StepFrame(EngineN::TimeManager& tm, uint64_t dtNs) {
    tm.BeginFrameManual(dtNs);
    uint32_t ticks = 0;
    while (tm.ConsumeTick()) {
        ticks++;
        tm.EndTick();
    }
    tm.EndFrame();
    return ticks;
}

void TestTimestepInvariants() {
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    const uint64_t fixedDt = tm.GetFixedDtNs();

    // 0 ns -> 0 ticks, alpha = 0
    assert(StepFrame(tm, 0) == 0 && tm.GetAlpha() == 0.0);

    // Partial step (1ms) -> 0 ticks, accumulator stores 1ms
    assert(StepFrame(tm, 1'000'000ULL) == 0);
    assert(tm.GetAccumulatorNs() == 1'000'000ULL);

    // Exact 1 step (8.33ms) -> 1 tick
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    assert(StepFrame(tm, fixedDt) == 1);
    assert(tm.GetAccumulatorNs() == 0);

    // Exact 2 steps -> 2 ticks
    assert(StepFrame(tm, fixedDt * 2ULL) == 2);
    assert(tm.GetAccumulatorNs() == 0);

    // Sub-step fraction remainder preserved
    const uint64_t halfStep = fixedDt / 2ULL;
    assert(StepFrame(tm, fixedDt * 3ULL + halfStep) == 3);
    assert(tm.GetAccumulatorNs() == halfStep);

    // Invariants: accumulator strictly < fixedDt, alpha in [0.0, 1.0)
    assert(tm.GetAccumulatorNs() < fixedDt);
    assert(tm.GetAlpha() >= 0.0 && tm.GetAlpha() < 1.0);
}

void TestSpiralOfDeathAndClamping() {
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // Extreme delta spikes (100ms, 1s, 10s, UINT64_MAX) must strictly clamp to 8 ticks
    assert(StepFrame(tm, 100'000'000ULL) == 8);
    assert(StepFrame(tm, 1'000'000'000ULL) == 8);
    assert(StepFrame(tm, 10'000'000'000ULL) == 8);
    assert(StepFrame(tm, UINT64_MAX) == 8);
    assert(tm.GetAccumulatorNs() == 0);

    // Heavy continuous load: 50 consecutive 200ms frames never run away
    for (int i = 0; i < 50; ++i) {
        const uint32_t ticks = StepFrame(tm, 200'000'000ULL);
        assert(ticks <= 8);
        assert(tm.GetAccumulatorNs() < tm.GetFixedDtNs());
    }
}

void TestPauseResumeAndRefocus() {
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // Accumulate residual fraction
    StepFrame(tm, tm.GetFixedDtNs() / 2ULL);
    const uint64_t residualAcc = tm.GetAccumulatorNs();

    // Pause: wall-clock advance produces 0 ticks, preserves residual
    tm.SetPaused(true);
    assert(tm.IsPaused());
    assert(StepFrame(tm, 5'000'000'000ULL) == 0);
    assert(tm.GetAccumulatorNs() == residualAcc);

    // Resume: continues normally from residual
    tm.SetPaused(false);
    assert(!tm.IsPaused());
    assert(StepFrame(tm, tm.GetFixedDtNs()) == 1);

    // Refocus reset: flushes accumulator and pacer
    StepFrame(tm, tm.GetFixedDtNs() / 2ULL);
    tm.ResetClocksOnRefocus();
    assert(tm.GetAccumulatorNs() == 0);
}

void TestDeterminismAndTimeScaling() {
    // Determinism: identical frame deltas produce bitwise identical states
    const uint64_t sequence[] = { 16'200'000ULL, 8'333'333ULL, 25'000'000ULL, 4'100'000ULL, 16'666'666ULL };
    EngineN::TimeManager tmA, tmB;
    tmA.Init();
    tmB.Init();

    float posA = 0.0f;
    float posB = 0.0f;
    for (uint64_t dt : sequence) {
        tmA.BeginFrameManual(dt);
        while (tmA.ConsumeTick()) {
            posA += 100.0f * static_cast<float>(tmA.PlayerTime().scaledDt);
            tmA.EndTick();
        }
        tmA.EndFrame();

        tmB.BeginFrameManual(dt);
        while (tmB.ConsumeTick()) {
            posB += 100.0f * static_cast<float>(tmB.PlayerTime().scaledDt);
            tmB.EndTick();
        }
        tmB.EndFrame();
    }
    assert(tmA.GetSimTickCount() == tmB.GetSimTickCount());
    assert(tmA.GetAccumulatorNs() == tmB.GetAccumulatorNs());
    assert(posA == posB);

    // Channel scaling
    EngineN::TimeManager tm;
    tm.Init();
    tm.PlayerTime().SetTimeScale(0.5);
    tm.BeginFrameManual(tm.GetFixedDtNs());
    assert(tm.ConsumeTick());
    assert(std::abs(tm.PlayerTime().scaledDt - tm.GetFixedDtSec() * 0.5) < 1e-7);
    tm.EndTick();
    tm.EndFrame();

    // Invalid scales safely rejected
    tm.PlayerTime().SetTimeScale(-2.0);
    assert(tm.PlayerTime().timeScale == 0.5);
    tm.PlayerTime().SetTimeScale(NAN);
    assert(tm.PlayerTime().timeScale == 0.5);
}

void TestHitstopEngine() {
    EngineN::HitstopEngine hitstop;

    // Zero or negative requests safely ignored
    hitstop.Request({EngineN::HitstopChannel::Player, 0.0});
    hitstop.Request({EngineN::HitstopChannel::Player, -0.05});
    assert(!hitstop.IsFrozen(EngineN::HitstopChannel::Player));
    assert(hitstop.GetModifier(EngineN::HitstopChannel::Player) == 1.0);

    // Freeze request: immediate binary transition to 0.0
    hitstop.Request({EngineN::HitstopChannel::Player, 0.100});
    assert(hitstop.IsFrozen(EngineN::HitstopChannel::Player));
    assert(hitstop.GetModifier(EngineN::HitstopChannel::Player) == 0.0);

    // Non-stacking merge-by-max: retains max remaining duration
    hitstop.Request({EngineN::HitstopChannel::Player, 0.050}); // smaller: retains 0.100
    hitstop.Update(0.060);
    assert(hitstop.IsFrozen(EngineN::HitstopChannel::Player));
    hitstop.Update(0.040); // 0.060 + 0.040 = 0.100: expires
    assert(!hitstop.IsFrozen(EngineN::HitstopChannel::Player));
    assert(hitstop.GetModifier(EngineN::HitstopChannel::Player) == 1.0);

    // Reset flushes freeze
    hitstop.Request({EngineN::HitstopChannel::World, 0.500});
    hitstop.Reset();
    assert(!hitstop.IsFrozen(EngineN::HitstopChannel::World));
}

void TestFramePacerAndClock() {
    // HighResolutionClock: backward jump rejection
    EngineN::HighResolutionClock clk;
    clk.Init();
    clk.previousTicks = SDL_GetTicksNS() + 1'000'000'000ULL;
    assert(clk.Sample() == 0ULL);

    // FramePacer: target FPS configuration
    EngineN::FramePacer pacer;
    pacer.SetTargetFps(60);
    assert(pacer.GetTargetFps() == 60 && pacer.GetTargetFrameNs() == 16'666'666ULL);
    pacer.SetTargetFps(120);
    assert(pacer.GetTargetFps() == 120 && pacer.GetTargetFrameNs() == 8'333'333ULL);
    pacer.SetTargetFps(0);
    assert(pacer.GetTargetFps() == 0 && pacer.GetTargetFrameNs() == 0ULL);

    // Absolute deadline advancement: exactly targetFrameNs per step (zero drift)
    pacer.SetTargetFps(60);
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

    // 10,000 random-dt fuzzing iterations: all invariants hold
    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<uint64_t> distDt(0ULL, 2'000'000'000ULL);

    for (int i = 0; i < 10'000; ++i) {
        const uint32_t ticks = StepFrame(tm, distDt(rng));
        assert(ticks <= 8);
        assert(tm.GetAccumulatorNs() < tm.GetFixedDtNs());
        assert(tm.GetAlpha() >= 0.0 && tm.GetAlpha() < 1.0);
    }

    // 100,000 steady tick endurance check: zero accumulation drift
    EngineN::TimeManager tmEndurance;
    tmEndurance.Init();
    for (int i = 0; i < 100'000; ++i) {
        assert(StepFrame(tmEndurance, tmEndurance.GetFixedDtNs()) == 1);
        assert(tmEndurance.GetAccumulatorNs() == 0);
    }
    assert(tmEndurance.GetSimTickCount() == 100'000);
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
    std::cout << "  -> 2. Spiral-of-Death & Spike Clamping: PASSED\n";

    TestPauseResumeAndRefocus();
    std::cout << "  -> 3. Pause, Resume & Refocus Safety: PASSED\n";

    TestDeterminismAndTimeScaling();
    std::cout << "  -> 4. Simulation Determinism & Channel Scaling: PASSED\n";

    TestHitstopEngine();
    std::cout << "  -> 5. Combat Hitstop (Non-Stacking Merge): PASSED\n";

    TestFramePacerAndClock();
    std::cout << "  -> 6. Zero-Drift Frame Pacer & Clock Anomaly: PASSED\n";

    TestEnduranceAndFuzzingInvariants();
    std::cout << "  -> 7. Fuzzing & 100k-Tick Zero-Drift Endurance: PASSED\n";

    std::cout << "[TestTimeLoop] All Adversarial Suites Passed Successfully!\n";

    SDL_Quit();
    return 0;
}
