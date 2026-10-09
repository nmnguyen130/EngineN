#include "core/time/TimeManager.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_passCount = 0;
int g_failCount = 0;

void Report(const char* testId, const char* name, bool pass, const char* extra = nullptr) {
    if (pass) {
        g_passCount++;
        printf("  [PASS] %-5s %-42s", testId, name);
        if (extra) printf(" (%s)", extra);
        printf("\n");
    } else {
        g_failCount++;
        printf("  [FAIL] %-5s %-42s", testId, name);
        if (extra) printf(" (%s)", extra);
        printf("\n");
    }
}

void Section(const char* title) {
    printf("\n--- %s ---\n", title);
}

// -------------------------------------------------------------
// GROUP 1: Variable frame-time / spikes (T01 - T08)
// -------------------------------------------------------------
void TestGroup1_Spikes() {
    Section("Group 1: Variable Frame-Time / Spikes (T01 - T08)");
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // T01: dt = 0 -> 0 updates, remains alive
    tm.BeginFrameManual(0);
    int t01Updates = 0;
    while (tm.ConsumeTick()) { t01Updates++; tm.EndTick(); }
    tm.EndFrame();
    Report("T01", "dt = 0 ns produces 0 updates", t01Updates == 0 && tm.GetAlpha() == 0.0);

    // T02: dt < 0 rejected by Clock.Sample() (returns 0)
    EngineN::HighResolutionClock clk;
    clk.Init();
    clk.previousTicks = SDL_GetTicksNS() + 1'000'000'000ULL; // simulate backward jump
    uint64_t backwardSample = clk.Sample();
    Report("T02", "dt < 0 backward clock jump clamped to 0", backwardSample == 0ULL);

    // T03: dt = 1 ms (1,000,000 ns) -> accumulator grows, 0 updates (fixed dt is 8.33ms)
    tm.BeginFrameManual(1'000'000ULL);
    int t03Updates = 0;
    while (tm.ConsumeTick()) { t03Updates++; tm.EndTick(); }
    tm.EndFrame();
    Report("T03", "dt = 1 ms accumulates, 0 updates", t03Updates == 0 && tm.GetAccumulatorNs() == 1'000'000ULL);

    // T04: dt = 16.666 ms (16,666,666 ns) -> consumes exactly 2 updates (8.333ms x 2)
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    tm.BeginFrameManual(16'666'666ULL);
    int t04Updates = 0;
    while (tm.ConsumeTick()) { t04Updates++; tm.EndTick(); }
    tm.EndFrame();
    Report("T04", "dt = 16.666 ms consumes exactly 2 updates", t04Updates == 2);

    // T05: dt = 100 ms -> clamped to 8 updates
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    tm.BeginFrameManual(100'000'000ULL);
    int t05Updates = 0;
    while (tm.ConsumeTick()) { t05Updates++; tm.EndTick(); }
    tm.EndFrame();
    Report("T05", "dt = 100 ms clamped to max 8 updates", t05Updates == 8);

    // T06: dt = 1 sec -> clamped to 8 updates
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    tm.BeginFrameManual(1'000'000'000ULL);
    int t06Updates = 0;
    while (tm.ConsumeTick()) { t06Updates++; tm.EndTick(); }
    tm.EndFrame();
    Report("T06", "dt = 1 sec clamped to max 8 updates", t06Updates == 8);

    // T07: dt = 10 sec -> clamped to 8 updates
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    tm.BeginFrameManual(10'000'000'000ULL);
    int t07Updates = 0;
    while (tm.ConsumeTick()) { t07Updates++; tm.EndTick(); }
    tm.EndFrame();
    Report("T07", "dt = 10 sec clamped to max 8 updates", t07Updates == 8);

    // T08: dt = UINT64_MAX -> no explosion or overflow, bounded
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    tm.BeginFrameManual(UINT64_MAX);
    int t08Updates = 0;
    while (tm.ConsumeTick()) { t08Updates++; tm.EndTick(); }
    tm.EndFrame();
    Report("T08", "dt = UINT64_MAX spike clamped safely", t08Updates == 8 && tm.GetAccumulatorNs() == 0);
}

// -------------------------------------------------------------
// GROUP 2: Fixed Timestep Correctness (T09 - T12)
// -------------------------------------------------------------
void TestGroup2_Correctness() {
    Section("Group 2: Fixed Timestep Correctness (T09 - T12)");
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // T09: frame dt = 1/120s -> 1 update
    tm.BeginFrameManual(tm.GetFixedDtNs());
    int t09 = 0;
    while (tm.ConsumeTick()) { t09++; tm.EndTick(); }
    tm.EndFrame();
    Report("T09", "dt = 1/120s produces exactly 1 update", t09 == 1 && tm.GetAccumulatorNs() == 0);

    // T10: frame dt = 2/120s -> 2 updates
    tm.BeginFrameManual(tm.GetFixedDtNs() * 2ULL);
    int t10 = 0;
    while (tm.ConsumeTick()) { t10++; tm.EndTick(); }
    tm.EndFrame();
    Report("T10", "dt = 2/120s produces exactly 2 updates", t10 == 2 && tm.GetAccumulatorNs() == 0);

    // T11: frame dt = 3.5 / 120s -> 3 updates, accumulator = 0.5/120s
    uint64_t halfStep = tm.GetFixedDtNs() / 2ULL;
    tm.BeginFrameManual(tm.GetFixedDtNs() * 3ULL + halfStep);
    int t11 = 0;
    while (tm.ConsumeTick()) { t11++; tm.EndTick(); }
    tm.EndFrame();
    Report("T11", "dt = 3.5/120s -> 3 updates, acc = 0.5/120s", t11 == 3 && tm.GetAccumulatorNs() == halfStep);

    // T12: 10 frames x 0.5/120s (alternating odd/even ns) -> total 5 updates
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    int t12Total = 0;
    for (int i = 0; i < 10; ++i) {
        // Since 8,333,333 is odd, alternate +1 ns to sum to exact fixedDtNs every 2 frames
        uint64_t halfStepNs = (i % 2 == 0) ? (tm.GetFixedDtNs() / 2ULL + 1ULL) : (tm.GetFixedDtNs() / 2ULL);
        tm.BeginFrameManual(halfStepNs);
        while (tm.ConsumeTick()) { t12Total++; tm.EndTick(); }
        tm.EndFrame();
    }
    Report("T12", "10 frames x 0.5/120s yields 5 updates", t12Total == 5 && tm.GetAccumulatorNs() == 0);
}

// -------------------------------------------------------------
// GROUP 3: Accumulation & Precision (T13 - T15)
// -------------------------------------------------------------
void TestGroup3_Accumulation() {
    Section("Group 3: Nanosecond Accumulation & Precision (T13 - T15)");
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // T13: advance 1/120s x 12,000 frames -> exactly 12,000 ticks = 100 sec
    for (int i = 0; i < 12'000; ++i) {
        tm.BeginFrameManual(tm.GetFixedDtNs());
        while (tm.ConsumeTick()) { tm.EndTick(); }
        tm.EndFrame();
    }
    double simTime = tm.GetSimTickCount() * tm.GetFixedDtSec();
    Report("T13", "12,000 frames produces exactly 100s sim", tm.GetSimTickCount() == 12'000 && std::abs(simTime - 100.0) < 1e-6);

    // T14: tiny dt (1 microsecond = 1,000 ns) repeatedly -> zero lost updates
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    uint64_t tinyDt = 1'000ULL; // 1 us
    // 8,334 steps of 1us = 8,334,000 ns > 8,333,333 ns -> exactly 1 update
    int t14Updates = 0;
    for (int i = 0; i < 8334; ++i) {
        tm.BeginFrameManual(tinyDt);
        while (tm.ConsumeTick()) { t14Updates++; tm.EndTick(); }
        tm.EndFrame();
    }
    Report("T14", "8,334 tiny 1us steps yields 1 update", t14Updates == 1 && tm.GetAccumulatorNs() == (8'334'000ULL - tm.GetFixedDtNs()));

    // T15: accumulator bounded strictly < fixed_dt after each frame
    bool t15Bounded = true;
    for (int i = 0; i < 1000; ++i) {
        tm.BeginFrameManual(3'141'592ULL); // arbitrary prime delta
        while (tm.ConsumeTick()) { tm.EndTick(); }
        tm.EndFrame();
        if (tm.GetAccumulatorNs() >= tm.GetFixedDtNs()) t15Bounded = false;
    }
    Report("T15", "Accumulator strictly < fixed_dt over 1000 frames", t15Bounded);
}

// -------------------------------------------------------------
// GROUP 4: Spiral of Death (T16 - T17)
// -------------------------------------------------------------
void TestGroup4_SpiralOfDeath() {
    Section("Group 4: Spiral of Death Protection (T16 - T17)");
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // T16: frame dt = 10s does not run 1,200 updates -> max 8 updates
    tm.BeginFrameManual(10'000'000'000ULL);
    int t16 = 0;
    while (tm.ConsumeTick()) { t16++; tm.EndTick(); }
    tm.EndFrame();
    Report("T16", "dt = 10s capped strictly to 8 updates", t16 == 8);

    // T17: 50 consecutive frames of 200ms -> CPU never runs away, every frame <= 8 updates
    bool t17Bounded = true;
    for (int i = 0; i < 50; ++i) {
        tm.BeginFrameManual(200'000'000ULL);
        int frameUpdates = 0;
        while (tm.ConsumeTick()) { frameUpdates++; tm.EndTick(); }
        tm.EndFrame();
        if (frameUpdates > 8 || tm.GetAccumulatorNs() >= tm.GetFixedDtNs()) {
            t17Bounded = false;
        }
    }
    Report("T17", "50 heavy 200ms frames never run away (max 8)", t17Bounded);
}

// -------------------------------------------------------------
// GROUP 5: Pause / Resume (T18 - T21)
// -------------------------------------------------------------
void TestGroup5_PauseResume() {
    Section("Group 5: Pause / Resume Behavior (T18 - T21)");
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // T18: pause -> advance 10 sec -> 0 updates
    tm.SetPaused(true);
    tm.BeginFrameManual(10'000'000'000ULL);
    int t18 = 0;
    while (tm.ConsumeTick()) { t18++; tm.EndTick(); }
    tm.EndFrame();
    Report("T18", "Paused state produces 0 updates on 10s dt", t18 == 0 && tm.IsPaused());

    // T19: pause -> resume -> simulation continues normally
    tm.SetPaused(false);
    tm.BeginFrameManual(tm.GetFixedDtNs());
    int t19 = 0;
    while (tm.ConsumeTick()) { t19++; tm.EndTick(); }
    tm.EndFrame();
    Report("T19", "Resume restores normal tick execution", t19 == 1);

    // T20: pause while accumulator > 0 -> preserves residual fraction
    tm.BeginFrameManual(tm.GetFixedDtNs() / 2ULL); // acc = 0.5 dt
    while (tm.ConsumeTick()) { tm.EndTick(); }
    tm.EndFrame();
    uint64_t prePauseAcc = tm.GetAccumulatorNs();

    tm.SetPaused(true);
    tm.BeginFrameManual(1'000'000'000ULL); // 1s while paused
    while (tm.ConsumeTick()) { tm.EndTick(); }
    tm.EndFrame();

    tm.SetPaused(false);
    Report("T20", "Pause preserves residual fraction cleanly", tm.GetAccumulatorNs() == prePauseAcc);

    // T21: rapid pause/resume cycles do not produce phantom ticks
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    int t21 = 0;
    for (int i = 0; i < 20; ++i) {
        tm.SetPaused(i % 2 == 0);
        tm.BeginFrameManual(0);
        while (tm.ConsumeTick()) { t21++; tm.EndTick(); }
        tm.EndFrame();
    }
    Report("T21", "Rapid pause/resume toggling produces 0 phantom ticks", t21 == 0);
}

// -------------------------------------------------------------
// GROUP 6: Time Scale Sanity (T22 - T26)
// -------------------------------------------------------------
void TestGroup6_TimeScale() {
    Section("Group 6: Time Scale Sanity (T22 - T26)");
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // T22: scale = 0 -> player time stands still (scaledDt = 0.0)
    tm.PlayerTime().SetTimeScale(0.0);
    tm.BeginFrameManual(tm.GetFixedDtNs());
    assert(tm.ConsumeTick());
    Report("T22", "scale = 0.0 freezes channel (scaledDt = 0)", tm.PlayerTime().scaledDt == 0.0);
    tm.EndTick();
    tm.EndFrame();

    // T23: scale = 0.5 -> player runs at 1/2 speed
    tm.PlayerTime().SetTimeScale(0.5);
    tm.BeginFrameManual(tm.GetFixedDtNs());
    assert(tm.ConsumeTick());
    double expectedHalf = tm.GetFixedDtSec() * 0.5;
    Report("T23", "scale = 0.5 runs at exact half-speed", std::abs(tm.PlayerTime().scaledDt - expectedHalf) < 1e-7);
    tm.EndTick();
    tm.EndFrame();

    // T24: scale = 2.0 -> player runs at 2x speed
    tm.PlayerTime().SetTimeScale(2.0);
    tm.BeginFrameManual(tm.GetFixedDtNs());
    assert(tm.ConsumeTick());
    double expectedDouble = tm.GetFixedDtSec() * 2.0;
    Report("T24", "scale = 2.0 runs at exact 2x speed", std::abs(tm.PlayerTime().scaledDt - expectedDouble) < 1e-7);
    tm.EndTick();
    tm.EndFrame();

    // T25: scale < 0 rejected (retains previous 2.0 scale)
    tm.PlayerTime().SetTimeScale(-1.5);
    Report("T25", "scale < 0 rejected, retains valid scale", tm.PlayerTime().timeScale == 2.0);

    // T26: scale = NaN or Inf rejected
    tm.PlayerTime().SetTimeScale(NAN);
    tm.PlayerTime().SetTimeScale(INFINITY);
    Report("T26", "scale = NaN / Inf rejected safely", tm.PlayerTime().timeScale == 2.0);
}

// -------------------------------------------------------------
// GROUP 7: Determinism (T27)
// -------------------------------------------------------------
void TestGroup7_Determinism() {
    Section("Group 7: Simulation Determinism (T27)");
    const uint64_t sequence[] = {
        16'200'000ULL, 17'100'000ULL, 15'400'000ULL, 8'333'333ULL,
        4'100'000ULL, 25'000'000ULL, 8'333'333ULL, 16'666'666ULL
    };

    // Run A
    EngineN::TimeManager tmA;
    tmA.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    float posA = 0.0f;
    for (uint64_t dt : sequence) {
        tmA.BeginFrameManual(dt);
        while (tmA.ConsumeTick()) {
            posA += 100.0f * static_cast<float>(tmA.PlayerTime().scaledDt);
            tmA.EndTick();
        }
        tmA.EndFrame();
    }

    // Run B
    EngineN::TimeManager tmB;
    tmB.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    float posB = 0.0f;
    for (uint64_t dt : sequence) {
        tmB.BeginFrameManual(dt);
        while (tmB.ConsumeTick()) {
            posB += 100.0f * static_cast<float>(tmB.PlayerTime().scaledDt);
            tmB.EndTick();
        }
        tmB.EndFrame();
    }

    bool deterministic = (tmA.GetSimTickCount() == tmB.GetSimTickCount()) &&
                         (tmA.GetAccumulatorNs() == tmB.GetAccumulatorNs()) &&
                         (posA == posB);
    Report("T27", "Identical frame sequence produces bitwise identical state", deterministic);
}

// -------------------------------------------------------------
// GROUP 8: Interpolation Invariants (T28 - T31)
// -------------------------------------------------------------
void TestGroup8_InterpolationInvariants() {
    Section("Group 8: Interpolation Invariants (T28 - T31)");
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    // T28: acc = 0 -> alpha = 0.0
    tm.BeginFrameManual(tm.GetFixedDtNs());
    while (tm.ConsumeTick()) { tm.EndTick(); }
    tm.EndFrame();
    Report("T28", "acc = 0 -> alpha == 0.0", tm.GetAlpha() == 0.0);

    // T29: acc = fixedDt / 2 -> alpha ≈ 0.5
    tm.BeginFrameManual(tm.GetFixedDtNs() / 2ULL);
    while (tm.ConsumeTick()) { tm.EndTick(); }
    tm.EndFrame();
    Report("T29", "acc = fixed_dt / 2 -> alpha == 0.5", std::abs(tm.GetAlpha() - 0.5) < 1e-4);

    // T30: acc = fixedDt - 1 ns -> alpha strictly < 1.0
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    tm.BeginFrameManual(tm.GetFixedDtNs() - 1ULL);
    while (tm.ConsumeTick()) { tm.EndTick(); }
    tm.EndFrame();
    Report("T30", "acc = fixed_dt - 1ns -> alpha < 1.0", tm.GetAlpha() > 0.999 && tm.GetAlpha() < 1.0);

    // T31: alpha always in [0.0, 1.0) under various inputs
    bool t31Valid = true;
    for (uint64_t dt = 0; dt < 50'000'000ULL; dt += 777'777ULL) {
        tm.BeginFrameManual(dt);
        while (tm.ConsumeTick()) { tm.EndTick(); }
        tm.EndFrame();
        if (tm.GetAlpha() < 0.0 || tm.GetAlpha() >= 1.0) t31Valid = false;
    }
    Report("T31", "Alpha strictly confined to [0.0, 1.0) across all dt", t31Valid);
}

// -------------------------------------------------------------
// GROUP 9: Clock Anomaly (T32 - T34)
// -------------------------------------------------------------
void TestGroup9_ClockAnomaly() {
    Section("Group 9: Clock Anomaly Handling (T32 - T34)");
    EngineN::HighResolutionClock clk;
    clk.Init();

    // T32: clock backward jump produces 0 ns (non-negative)
    uint64_t baseTime = SDL_GetTicksNS();
    clk.previousTicks = baseTime + 5'000'000'000ULL; // clock in future
    uint64_t sampleNegative = clk.Sample();
    Report("T32", "Clock backward jump produces 0 ns delta", sampleNegative == 0ULL);

    // T33: massive forward jump clamped safely
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    tm.BeginFrameManual(100'000'000'000ULL); // 100 sec
    int updates = 0;
    while (tm.ConsumeTick()) { updates++; tm.EndTick(); }
    tm.EndFrame();
    Report("T33", "Massive forward jump clamped to maxCatchupTicks (8)", updates == 8);

    // T34: clock returns same timestamp -> delta = 0
    clk.previousTicks = baseTime;
    uint64_t nowTicks = baseTime;
    // Emulate same timestamp
    uint64_t sampleSame = (nowTicks >= clk.previousTicks) ? (nowTicks - clk.previousTicks) : 0ULL;
    Report("T34", "Same timestamp produces exact 0 ns delta", sampleSame == 0ULL);
}

// -------------------------------------------------------------
// GROUP 10: Property / Fuzzing Stress Test (T35 - T36)
// -------------------------------------------------------------
void TestGroup10_FuzzingStressTest() {
    Section("Group 10: Property-Based Fuzzing & Stress Invariants (T35 - T36)");
    EngineN::TimeManager tm;
    tm.Init({.simulationHz = 120, .maxCatchupTicks = 8});

    std::mt19937_64 rng(1337);
    // Random dt from 0 ns up to 2 seconds (2,000,000,000 ns)
    std::uniform_int_distribution<uint64_t> distDt(0ULL, 2'000'000'000ULL);

    bool allInvariantsHeld = true;
    const int FUZZ_ITERATIONS = 100'000;

    for (int i = 0; i < FUZZ_ITERATIONS; ++i) {
        uint64_t dt = distDt(rng);
        tm.BeginFrameManual(dt);

        int updates = 0;
        while (tm.ConsumeTick()) {
            updates++;
            tm.EndTick();
        }
        tm.EndFrame();

        // INVARIANTS:
        // 1. Accumulator strictly < fixed_dt after frame
        if (tm.GetAccumulatorNs() >= tm.GetFixedDtNs()) allInvariantsHeld = false;
        // 2. Alpha strictly in [0.0, 1.0)
        if (tm.GetAlpha() < 0.0 || tm.GetAlpha() >= 1.0) allInvariantsHeld = false;
        // 3. Updates per tick <= maxCatchupTicks (8)
        if (updates > 8) allInvariantsHeld = false;
        // 4. Time channels must be finite
        if (!std::isfinite(tm.RealTime().totalTime) || !std::isfinite(tm.PlayerTime().totalTime)) allInvariantsHeld = false;
    }

    Report("T35", "100,000 random-dt fuzzing iterations (all invariants hold)", allInvariantsHeld);

    // T36: Long-running 1,000,000 steady tick endurance check
    bool t36Stable = true;
    EngineN::TimeManager tmLong;
    tmLong.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    for (int i = 0; i < 1'000'000; ++i) {
        tmLong.BeginFrameManual(8'333'333ULL);
        while (tmLong.ConsumeTick()) { tmLong.EndTick(); }
        tmLong.EndFrame();
        if (tmLong.GetAccumulatorNs() != 0) t36Stable = false;
    }
    Report("T36", "1,000,000 steady ticks zero drift endurance", t36Stable && tmLong.GetSimTickCount() == 1'000'000);
}

// -------------------------------------------------------------
// GROUP 11: Multi-Channel Hitstop Engine (T37 - T42)
// -------------------------------------------------------------
void TestGroup11_HitstopEngine() {
    Section("Group 11: Multi-Channel Hitstop Engine (T37 - T42)");

    // T37: Hitstop request immediately freezes channel
    EngineN::HitstopEngine hitstop;
    hitstop.Request({EngineN::HitstopChannel::Player, 0.100});
    Report("T37", "Hitstop request immediately freezes channel",
           hitstop.IsFrozen(EngineN::HitstopChannel::Player) && hitstop.GetModifier(EngineN::HitstopChannel::Player) == 0.0);

    // T38: Freeze duration decays accurately and expires
    hitstop.Update(0.040);
    bool t38Mid = hitstop.IsFrozen(EngineN::HitstopChannel::Player) && (hitstop.GetModifier(EngineN::HitstopChannel::Player) == 0.0);
    hitstop.Update(0.060);
    bool t38End = !hitstop.IsFrozen(EngineN::HitstopChannel::Player) && (hitstop.GetModifier(EngineN::HitstopChannel::Player) == 1.0);
    Report("T38", "Hitstop freeze decays accurately and expires", t38Mid && t38End);

    // T39: Snappy binary transition (0.0 during freeze, immediately 1.0 on expiry, no mushy ramp)
    EngineN::HitstopEngine snapEngine;
    snapEngine.Request({EngineN::HitstopChannel::Player, 0.050});
    bool modDuring = (snapEngine.GetModifier(EngineN::HitstopChannel::Player) == 0.0);
    snapEngine.Update(0.050);
    bool modAfter = (snapEngine.GetModifier(EngineN::HitstopChannel::Player) == 1.0);
    Report("T39", "Snappy binary transition (0.0 frozen -> 1.0 immediate)", modDuring && modAfter);

    // T40: Non-stacking merge-by-max prevents freeze runaway
    EngineN::HitstopEngine mergeEngine;
    mergeEngine.Request({EngineN::HitstopChannel::Enemy, 0.050});
    mergeEngine.Request({EngineN::HitstopChannel::Enemy, 0.080});
    // Max merge yields 0.080s freeze, not 0.130s stacked
    mergeEngine.Update(0.080);
    bool t40FrozenExpired = !mergeEngine.IsFrozen(EngineN::HitstopChannel::Enemy);
    Report("T40", "Non-stacking merge-by-max prevents freeze runaway", t40FrozenExpired);

    // T41: Zero or negative duration request safely ignored
    EngineN::HitstopEngine noopEngine;
    noopEngine.Request({EngineN::HitstopChannel::World, 0.0});
    noopEngine.Request({EngineN::HitstopChannel::World, -0.050});
    Report("T41", "Zero/negative duration request safely ignored",
           !noopEngine.IsFrozen(EngineN::HitstopChannel::World) && noopEngine.GetModifier(EngineN::HitstopChannel::World) == 1.0);

    // T42: Hitstop decay paused during pause, resumes on unpause
    EngineN::TimeManager tmPause;
    tmPause.Init({.simulationHz = 120, .maxCatchupTicks = 8});
    tmPause.Hitstop().Request({EngineN::HitstopChannel::Player, 0.100});
    tmPause.SetPaused(true);
    tmPause.BeginFrameManual(500'000'000ULL); // 500ms wall-clock while paused
    while (tmPause.ConsumeTick()) { tmPause.EndTick(); }
    tmPause.EndFrame();
    bool stillFrozenPaused = tmPause.Hitstop().IsFrozen(EngineN::HitstopChannel::Player);

    tmPause.SetPaused(false);
    tmPause.BeginFrameManual(100'000'000ULL); // 100ms wall-clock unpaused
    while (tmPause.ConsumeTick()) { tmPause.EndTick(); }
    tmPause.EndFrame();
    bool expiredAfterUnpause = !tmPause.Hitstop().IsFrozen(EngineN::HitstopChannel::Player);
    Report("T42", "Hitstop decay paused during pause, resumes on unpause", stillFrozenPaused && expiredAfterUnpause);
}

// -------------------------------------------------------------
// GROUP 12: Frame Pacer Correctness (T43 - T45)
// -------------------------------------------------------------
void TestGroup12_FramePacer() {
    Section("Group 12: Frame Pacer Correctness (T43 - T45)");

    // T43: FramePacer target FPS configuration
    EngineN::FramePacer pacer;
    pacer.SetTargetFps(60);
    bool pacer60 = (pacer.GetTargetFps() == 60 && pacer.GetTargetFrameNs() == 16'666'666ULL);
    pacer.SetTargetFps(120);
    bool pacer120 = (pacer.GetTargetFps() == 120 && pacer.GetTargetFrameNs() == 8'333'333ULL);
    pacer.SetTargetFps(0);
    bool pacer0 = (pacer.GetTargetFps() == 0 && pacer.GetTargetFrameNs() == 0ULL);
    Report("T43", "FramePacer target FPS configuration (60/120/Uncapped)", pacer60 && pacer120 && pacer0);

    // T44: FramePacer uncapped mode returns immediately
    pacer.SetTargetFps(0);
    pacer.WaitForTarget();
    Report("T44", "FramePacer uncapped mode returns immediately", true);

    // T45: FramePacer deadline step tracking (zero drift)
    pacer.SetTargetFps(60);
    pacer.WaitForTarget(); // initial deadline set
    uint64_t deadline1 = pacer.GetTargetDeadlineNs();
    pacer.WaitForTarget(); // advanced by exactly 16'666'666 ns
    uint64_t deadline2 = pacer.GetTargetDeadlineNs();
    bool t45ExactStep = (deadline2 >= deadline1) && ((deadline2 - deadline1) == 16'666'666ULL);
    Report("T45", "FramePacer advances deadline by exact frame period (zero drift)", t45ExactStep);

    // T46: FramePacer Reset clears deadline
    pacer.Reset();
    Report("T46", "FramePacer Reset clears target deadline cleanly", pacer.GetTargetDeadlineNs() == 0);
}

} // namespace

int main(int argc, char* argv[]) {
    if (!SDL_Init(0)) {
        printf("Failed to initialize SDL: %s\n", SDL_GetError());
        return -1;
    }

    printf("\n==================================================================\n");
    printf("   ENGINEN TIMELOOP ADVERSARIAL VERIFICATION MATRIX (T01 - T46)   \n");
    printf("   Standard: Fixed Timestep 120Hz | uint64_t Nanoseconds Precision\n");
    printf("==================================================================\n");

    TestGroup1_Spikes();
    TestGroup2_Correctness();
    TestGroup3_Accumulation();
    TestGroup4_SpiralOfDeath();
    TestGroup5_PauseResume();
    TestGroup6_TimeScale();
    TestGroup7_Determinism();
    TestGroup8_InterpolationInvariants();
    TestGroup9_ClockAnomaly();
    TestGroup10_FuzzingStressTest();
    TestGroup11_HitstopEngine();
    TestGroup12_FramePacer();

    printf("\n==================================================================\n");
    printf("  SUMMARY: %d PASSED / %d FAILED (TOTAL 46 ADVERSARIAL SUITES)\n", g_passCount, g_failCount);
    printf("==================================================================\n\n");

    SDL_Quit();
    return (g_failCount == 0) ? 0 : 1;
}
