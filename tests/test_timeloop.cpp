#include "core/time/TimeManager.h"
#include "core/time/HitstopEngine.h"
#include "core/time/SimEvents.h"
#include "core/time/TickContext.h"
#include "renderer/Presenter.h"

#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>

using namespace EngineN;

namespace {

enum class Channel : u8 {
    World = 0,
    Player = 1,
    Enemy = 2
};

// Helper stepping outer frame dtNs -> returns ticks consumed
u32 StepFrame(TimeManager& tm, u64 dtNs) {
    tm.BeginFrameManual(dtNs);
    u32 ticks = 0;
    while (tm.ConsumeTick()) {
        ++ticks;
        tm.EndTick();
    }
    tm.EndFrame();
    return ticks;
}

void TestFixedTimestepInvariants() {
    TimeManager tm;
    tm.Init({.maxCatchupTicks = 8});

    // Zero delta produces zero ticks and zero alpha
    assert(StepFrame(tm, 0) == 0);
    assert(tm.GetAlpha() == 0.0f);

    // Delta below one tick (1ms < 8.33ms) accumulates with 0 ticks
    assert(StepFrame(tm, 1'000'000ULL) == 0);
    assert(tm.GetAccumulatorNs() == 1'000'000ULL);

    // 24ms added to 1ms = 25ms @ 120Hz -> exact 3 ticks, 0 remainder
    assert(StepFrame(tm, 24'000'000ULL) == 3);
    assert(tm.GetAccumulatorNs() == 0);
    assert(tm.GetAlpha() == 0.0f);

    // 10ms -> 1 tick consumed, remainder ~1.66ms -> alpha strictly in (0, 1)
    assert(StepFrame(tm, 10'000'000ULL) == 1);
    assert(tm.GetAccumulatorNs() < kFixedDtNs);
    assert(tm.GetAlpha() > 0.0f && tm.GetAlpha() < 1.0f);

    // 60 standard 60Hz frames (total 1s) at 120Hz sim -> exactly 120 ticks, zero drift
    tm.Init({.maxCatchupTicks = 8});
    u32 totalTicks = 0;
    u64 simTimeNs = 0;
    for (int i = 0; i < 60; ++i) {
        const u64 frameDt = (i % 3 == 0) ? 16'666'668ULL : 16'666'666ULL;
        simTimeNs += frameDt;
        totalTicks += StepFrame(tm, frameDt);
    }
    assert(simTimeNs == kNsPerSecond);
    assert(totalTicks == 120);
    assert(tm.GetAccumulatorNs() == 0);
    assert(tm.GetAlpha() == 0.0f);
}

void TestSpiralOfDeathAndBacklogCap() {
    TimeManager tm;
    tm.Init({.maxCatchupTicks = 8, .maxFrameDeltaNs = kMaxFrameDeltaNs});

    // 100ms delta requests 12 ticks -> capped to maxCatchupTicks = 8
    assert(StepFrame(tm, 100'000'000ULL) == 8);
    assert(tm.GetDroppedTicks() > 0);

    // Extreme delta (1s and UINT64_MAX) clamps to maxFrameDeltaNs and never overflows
    assert(StepFrame(tm, 1000'000'000ULL) == 8);
    assert(StepFrame(tm, UINT64_MAX) == 8);

    // Backlog dropped while sub-tick remainder is strictly preserved
    assert(tm.GetAccumulatorNs() < kFixedDtNs);
}

void TestPauseResumeAndRefocus() {
    TimeManager tm;
    tm.Init({.maxCatchupTicks = 8});

    // Sub-tick accumulator state
    StepFrame(tm, 4'000'000ULL);
    const u64 residualNs = tm.GetAccumulatorNs();
    assert(residualNs > 0);

    // Paused state preserves sub-tick residual without consuming ticks
    tm.SetPaused(true);
    assert(tm.IsPaused());
    assert(StepFrame(tm, 50'000'000ULL) == 0);
    assert(tm.GetAccumulatorNs() == residualNs);

    // Unpause resumes execution cleanly
    tm.SetPaused(false);
    assert(!tm.IsPaused());

    // Window refocus flushes residual accumulator and hitstop to prevent catch-up spike
    tm.Hitstop().Request(Channel::World, 10);
    tm.ResetClocksOnRefocus();
    assert(tm.GetAccumulatorNs() == 0);
    assert(tm.GetAlpha() == 0.0f);
    assert(!tm.Hitstop().IsFrozen(Channel::World));
}

void TestSimulationDeterminismAndScaling() {
    TimeManager tmA;
    TimeManager tmB;
    tmA.Init();
    tmB.Init();

    f32 posA = 0.0f;
    f32 posB = 0.0f;

    // Simulation A: 10 frames @ 20ms = 200ms -> exactly 24 ticks
    for (int i = 0; i < 10; ++i) {
        tmA.BeginFrameManual(20'000'000ULL);
        while (tmA.ConsumeTick()) {
            posA += 100.0f * tmA.GetScaledDt(Channel::World);
            tmA.EndTick();
        }
        tmA.EndFrame();
    }

    // Simulation B: 24 frames with exact total 200ms -> exactly 24 ticks
    for (int i = 0; i < 24; ++i) {
        const u64 dt = (i < 8) ? 8'333'334ULL : 8'333'333ULL;
        tmB.BeginFrameManual(dt);
        while (tmB.ConsumeTick()) {
            posB += 100.0f * tmB.GetScaledDt(Channel::World);
            tmB.EndTick();
        }
        tmB.EndFrame();
    }

    // Both simulations must produce identical state (zero bitwise divergence)
    assert(posA == posB);

    // Channel time scaling
    TimeManager tm;
    tm.Init();
    tm.SetTimeScale(Channel::Player, 0.5f);
    assert(tm.GetTimeScale(Channel::Player) == 0.5f);

    tm.BeginFrameManual(25'000'000ULL);
    assert(tm.ConsumeTick());
    assert(std::abs(tm.GetScaledDt(Channel::Player) - (kFixedDtSec * 0.5f)) < 1e-6f);
    tm.EndTick();
    while (tm.ConsumeTick()) {
        tm.EndTick();
    }
    tm.EndFrame();
}

void TestHitstopEngine() {
    HitstopEngine hitstop;

    // Zero ticks does not freeze
    hitstop.Request(Channel::Player, 0);
    assert(!hitstop.IsFrozen(Channel::Player));
    assert(hitstop.GetModifier(Channel::Player) == 1.0f);

    // 6 ticks freeze request -> modifier = 0.0
    hitstop.Request(Channel::Player, 6);
    assert(hitstop.IsFrozen(Channel::Player));
    assert(hitstop.GetModifier(Channel::Player) == 0.0f);
    assert(hitstop.GetRemainingTicks(Channel::Player) == 6);

    // Non-stacking merge-by-max: request 4 ticks (< 6) retains 6
    hitstop.Request(Channel::Player, 4);
    assert(hitstop.GetRemainingTicks(Channel::Player) == 6);

    // Advance 3 ticks
    hitstop.AdvanceTick();
    hitstop.AdvanceTick();
    hitstop.AdvanceTick();
    assert(hitstop.GetRemainingTicks(Channel::Player) == 3);
    assert(hitstop.IsFrozen(Channel::Player));

    // Advance remaining 3 ticks -> unfreezes
    hitstop.AdvanceTick();
    hitstop.AdvanceTick();
    hitstop.AdvanceTick();
    assert(!hitstop.IsFrozen(Channel::Player));
    assert(hitstop.GetModifier(Channel::Player) == 1.0f);

    // Reset clears freeze state immediately
    hitstop.Request(Channel::World, 60);
    hitstop.Reset();
    assert(!hitstop.IsFrozen(Channel::World));
}

void TestTickContextAndSimEvents() {
    TimeManager tm;
    SimEvents events;
    tm.Init();

    TickContext ctx(tm, events);

    assert(ctx.TickIndex() == 0);

    // Dt is active inside an open simulation tick
    tm.BeginFrameManual(20'000'000ULL);
    assert(tm.ConsumeTick());
    assert(ctx.Dt() == kFixedDtSec);

    // Request hitstop via context
    ctx.RequestHitstop(Channel::Enemy, 4);
    assert(tm.Hitstop().IsFrozen(Channel::Enemy));

    // Emit simulation event
    assert(events.View().empty());
    ctx.Emit(SimEvent{
        .kind = SimEventKind::HitLanded,
        .tick = static_cast<u32>(ctx.TickIndex()),
        .pos = {10.0f, 20.0f},
        .magnitude = 5.0f
    });

    assert(events.View().size() == 1);
    assert(events.View()[0].kind == SimEventKind::HitLanded);
    assert(events.View()[0].magnitude == 5.0f);

    events.Clear();
    assert(events.View().empty());
}

void TestFramePacer() {
    FramePacer pacer;

    pacer.SetTargetFps(0);
    assert(pacer.GetTargetFps() == 0);
    assert(pacer.GetTargetFrameNs() == 0ULL);

    pacer.SetTargetFps(120);
    assert(pacer.GetTargetFps() == 120);
    assert(pacer.GetTargetFrameNs() == 8'333'333ULL);

    pacer.SetTargetFps(60);
    assert(pacer.GetTargetFps() == 60);
    assert(pacer.GetTargetFrameNs() == 16'666'666ULL);

    pacer.WaitForTarget();
    const u64 d1 = pacer.GetTargetDeadlineNs();
    pacer.WaitForTarget();
    const u64 d2 = pacer.GetTargetDeadlineNs();
    assert(d2 - d1 == 16'666'666ULL);

    pacer.Reset();
    assert(pacer.GetTargetDeadlineNs() == 0);
}

void TestFuzzingAndEnduranceInvariants() {
    TimeManager tm;
    tm.Init({.maxCatchupTicks = 8});

    // 1,000 random frame spikes -> ticks <= 8, accumulator residual < 1 tick
    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<u64> distDt(0ULL, 2'000'000'000ULL);

    for (int i = 0; i < 1'000; ++i) {
        const u32 ticks = StepFrame(tm, distDt(rng));
        assert(ticks <= 8);
        assert(tm.GetAccumulatorNs() < kFixedDtNs);
        assert(tm.GetAlpha() >= 0.0f && tm.GetAlpha() < 1.0f);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << "\n";
        return -1;
    }

    std::cout << "[TestTimeLoop] Running Clean C++20 TimeLoop Verification Suite...\n";

    TestFixedTimestepInvariants();
    std::cout << "  -> 1. Fixed Timestep & Zero-Drift Invariants: PASSED\n";

    TestSpiralOfDeathAndBacklogCap();
    std::cout << "  -> 2. Spiral-of-Death & Backlog Cap: PASSED\n";

    TestPauseResumeAndRefocus();
    std::cout << "  -> 3. Pause, Resume & Refocus Lifecycle: PASSED\n";

    TestSimulationDeterminismAndScaling();
    std::cout << "  -> 4. Simulation Determinism & Channel Scaling: PASSED\n";

    TestHitstopEngine();
    std::cout << "  -> 5. Combat Hitstop (Non-Stacking Merge): PASSED\n";

    TestTickContextAndSimEvents();
    std::cout << "  -> 6. TickContext & SimEvents Buffer: PASSED\n";

    TestFramePacer();
    std::cout << "  -> 7. Presentation FramePacer: PASSED\n";

    TestFuzzingAndEnduranceInvariants();
    std::cout << "  -> 8. Fuzzing & Endurance Invariants: PASSED\n";

    std::cout << "[TestTimeLoop] All Suites Passed Successfully!\n";

    SDL_Quit();
    return 0;
}
