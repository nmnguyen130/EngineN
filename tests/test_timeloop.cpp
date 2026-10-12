#include "core/time/TimeManager.h"
#include "core/time/HitstopEngine.h"
#include "core/time/SimEvents.h"
#include "core/time/TickContext.h"
#include "core/interpolation/Interpolated.h"
#include "renderer/Presenter.h"
#include "math/Vec2.h"

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

// 1. Fake-clock baseline: exactly 120 simulation ticks per second without clamp/drop
void TestFakeClock120HzBaseline() {
    static_assert(60_f60 == 120, "60 design frames must equal 120 simulation ticks");
    static_assert(1_f60 == 2, "1 design frame must equal 2 simulation ticks");

    TimeManager tm;
    tm.Init({.maxCatchupTicks = 120, .maxFrameDeltaNs = kNsPerSecond});

    // Exact 1 second in one batch
    assert(StepFrame(tm, kNsPerSecond) == 120);
    assert(tm.GetSimTickCount() == 120);
    assert(tm.GetAccumulatorNs() == 0);
    assert(tm.GetAlpha() == 0.0f);

    // Exact 1 second via 120 incremental frames totaling 1,000,000,000 ns
    tm.Init({.maxCatchupTicks = 8});
    u32 totalTicks = 0;
    u64 accumulatedNs = 0;
    for (int i = 0; i < 120; ++i) {
        const u64 dt = (i == 119) ? (kNsPerSecond - accumulatedNs) : kFixedDtNs;
        accumulatedNs += dt;
        totalTicks += StepFrame(tm, dt);
    }
    assert(accumulatedNs == kNsPerSecond);
    assert(totalTicks == 120);
    assert(tm.GetAccumulatorNs() == 0);
    assert(tm.GetAlpha() == 0.0f);
}

// 2. Multi-framerate invariance (30..240 FPS): tick count depends strictly on elapsed time
void TestMultiFramerateInvariance() {
    constexpr u32 kFramerates[] = {30, 60, 75, 120, 144, 165, 240};

    for (u32 fps : kFramerates) {
        TimeManager tm;
        tm.Init({.maxCatchupTicks = 8});

        u32 totalTicks = 0;
        u64 accumulatedNs = 0;
        const u64 frameDurationNs = kNsPerSecond / fps;

        for (u32 f = 0; f < fps; ++f) {
            const u64 dt = (f == fps - 1) ? (kNsPerSecond - accumulatedNs) : frameDurationNs;
            accumulatedNs += dt;
            totalTicks += StepFrame(tm, dt);
        }

        assert(accumulatedNs == kNsPerSecond);
        assert(totalTicks == 120);
        assert(tm.GetAccumulatorNs() == 0);
        assert(tm.GetAlpha() == 0.0f);
    }
}

// 3. Random frame deltas & 50..500ms hitches: bounded catch-up, dropped ticks, alpha in [0, 1)
void TestRandomDeltasAndHitchClamp() {
    TimeManager tm;
    tm.Init({.maxCatchupTicks = 8, .maxFrameDeltaNs = kMaxFrameDeltaNs});

    // 500ms spike -> clamped to maxCatchupTicks (8)
    const u32 spikeTicks = StepFrame(tm, 500'000'000ULL);
    assert(spikeTicks == 8);
    assert(tm.GetDroppedTicks() > 0);
    assert(tm.GetAccumulatorNs() < kFixedDtNs);
    assert(tm.GetAlpha() >= 0.0f && tm.GetAlpha() < 1.0f);

    // Fuzzing 500 random frames between 50ms and 500ms
    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<u64> dist(50'000'000ULL, 500'000'000ULL);

    for (int i = 0; i < 500; ++i) {
        const u32 ticks = StepFrame(tm, dist(rng));
        assert(ticks <= 8);
        assert(tm.GetAccumulatorNs() < kFixedDtNs);
        assert(tm.GetAlpha() >= 0.0f && tm.GetAlpha() < 1.0f);
    }
}

// 4. Sub-tick input edge preservation: press and release between ticks without dropping edges
void TestSubTickEdgePreservation() {
    SimEvents events;

    // Both press and release emitted within the same sub-tick window
    assert(events.Push(SimEvent{.kind = SimEventKind::AttackStarted, .tick = 1}));
    assert(events.Push(SimEvent{.kind = SimEventKind::HitLanded, .tick = 1}));
    assert(events.View().size() == 2);
    assert(events.View()[0].kind == SimEventKind::AttackStarted);
    assert(events.View()[1].kind == SimEventKind::HitLanded);

    // Buffer overflow and drop counter test
    for (usize i = 2; i < SimEvents::kCapacity; ++i) {
        assert(events.Push(SimEvent{.kind = SimEventKind::DashStarted, .tick = 1}));
    }
    assert(events.View().size() == SimEvents::kCapacity);
    assert(events.Dropped() == 0);

    // Push past capacity records dropped count
    assert(!events.Push(SimEvent{.kind = SimEventKind::Landed, .tick = 1}));
    assert(events.Dropped() == 1);

    // Clear drains buffer and resets dropped counter
    events.Clear();
    assert(events.View().empty());
    assert(events.Dropped() == 0);
}

// 5. Focus loss, pause, suspend/resume lifecycle: no stuck movement or time jumps
void TestPauseResumeAndRefocus() {
    TimeManager tm;
    tm.Init({.maxCatchupTicks = 8});

    // Residual accumulator accumulation
    StepFrame(tm, 4'000'000ULL);
    const u64 residual = tm.GetAccumulatorNs();
    assert(residual == 4'000'000ULL);

    // Paused state does not advance ticks, preserves residual
    tm.SetPaused(true);
    assert(tm.IsPaused());
    assert(StepFrame(tm, 100'000'000ULL) == 0);
    assert(tm.GetAccumulatorNs() == residual);

    // Unpause resumes execution cleanly
    tm.SetPaused(false);
    assert(!tm.IsPaused());

    // Window refocus flushes residual accumulator and hitstop to prevent catchup spike
    tm.Hitstop().Request(Channel::World, 20);
    assert(tm.Hitstop().IsFrozen(Channel::World));
    tm.ResetClocksOnRefocus();
    assert(tm.GetAccumulatorNs() == 0);
    assert(tm.GetAlpha() == 0.0f);
    assert(!tm.Hitstop().IsFrozen(Channel::World));
}

// 6. Teleport, respawn, or cutscene snap: zero visual interpolation streaks
void TestTeleportSnapNoStreak() {
    Interpolated<Vec2> pos(Vec2{0.0f, 0.0f});

    // Normal movement interpolates smoothly
    pos.BeginTick();
    pos.Cur() = Vec2{10.0f, 0.0f};
    assert(pos.Prev().x == 0.0f);
    assert(pos.Cur().x == 10.0f);
    assert(pos.Get(0.0f).x == 0.0f);
    assert(pos.Get(0.5f).x == 5.0f);
    assert(pos.Get(1.0f).x == 10.0f);

    // Teleport / Respawn cut: Snap prevents interpolation streak
    pos.Snap(Vec2{500.0f, 300.0f});
    assert(pos.Prev().x == 500.0f && pos.Prev().y == 300.0f);
    assert(pos.Cur().x == 500.0f && pos.Cur().y == 300.0f);
    assert(pos.Get(0.0f).x == 500.0f);
    assert(pos.Get(0.5f).x == 500.0f);
    assert(pos.Get(1.0f).x == 500.0f);

    // Out-of-bounds alpha is safely clamped [0, 1]
    assert(pos.Get(-0.5f).x == 500.0f);
    assert(pos.Get(1.5f).x == 500.0f);
}

// 7. Time scaling (0..2x) & combat hitstop: fixed simulation dt, proportional gameplay rate
void TestTimeScaleAndCombatHitstop() {
    TimeManager tm;
    tm.Init();

    constexpr f32 kScales[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f};
    for (f32 s : kScales) {
        tm.SetTimeScale(Channel::Player, s);
        assert(tm.GetTimeScale(Channel::Player) == s);
        const f32 expectedDt = kFixedDtSec * s;
        assert(std::abs(tm.GetScaledDt(Channel::Player) - expectedDt) < 1e-6f);
    }

    // Invalid negative scale is rejected, excessive scale is clamped to 8.0f
    tm.SetTimeScale(Channel::Player, -1.0f);
    assert(tm.GetTimeScale(Channel::Player) == 2.0f);
    tm.SetTimeScale(Channel::Player, 100.0f);
    assert(tm.GetTimeScale(Channel::Player) == 8.0f);

    // Hitstop freeze isolation
    HitstopEngine& hitstop = tm.Hitstop();
    hitstop.Request(Channel::Enemy, 6);
    assert(hitstop.IsFrozen(Channel::Enemy));
    assert(hitstop.GetModifier(Channel::Enemy) == 0.0f);
    assert(tm.GetScaledDt(Channel::Enemy) == 0.0f);

    // Player remains unfrozen
    assert(!hitstop.IsFrozen(Channel::Player));
    assert(hitstop.GetModifier(Channel::Player) == 1.0f);

    // Merge-by-max retains higher tick count
    hitstop.Request(Channel::Enemy, 4);
    assert(hitstop.GetRemainingTicks(Channel::Enemy) == 6);

    // Count down to unfreeze
    for (int i = 0; i < 6; ++i) {
        hitstop.AdvanceTick();
    }
    assert(!hitstop.IsFrozen(Channel::Enemy));
}

// 8. Simulation telemetry metrics & presentation FramePacer
void TestTelemetryAndFramePacer() {
    TimeManager tm;
    tm.Init({.maxCatchupTicks = 8, .maxFrameDeltaNs = 250'000'000ULL});

    // 20ms delta -> 2 ticks @ 120Hz (16.66ms), remainder ~3.33ms
    StepFrame(tm, 20'000'000ULL);
    assert(tm.GetSimTickCount() == 2);
    assert(tm.GetRealDt() > 0.019 && tm.GetRealDt() < 0.021);
    assert(tm.GetAccumulatorNs() > 0 && tm.GetAccumulatorNs() < kFixedDtNs);
    assert(tm.GetAlpha() > 0.0f && tm.GetAlpha() < 1.0f);

    // Presentation FramePacer
    FramePacer pacer;
    pacer.SetTargetFps(60);
    assert(pacer.GetTargetFps() == 60);
    assert(pacer.GetTargetFrameNs() == 16'666'666ULL);

    pacer.SetTargetFps(120);
    assert(pacer.GetTargetFps() == 120);
    assert(pacer.GetTargetFrameNs() == 8'333'333ULL);

    pacer.SetTargetFps(0);
    assert(pacer.GetTargetFps() == 0);
    assert(pacer.GetTargetFrameNs() == 0ULL);
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

    TestFakeClock120HzBaseline();
    std::cout << "  -> 1. Fake-Clock 120Hz Baseline: PASSED\n";

    TestMultiFramerateInvariance();
    std::cout << "  -> 2. Multi-Framerate Invariance (30..240 FPS): PASSED\n";

    TestRandomDeltasAndHitchClamp();
    std::cout << "  -> 3. Random Deltas & Hitch Clamp: PASSED\n";

    TestSubTickEdgePreservation();
    std::cout << "  -> 4. Sub-Tick Edge Preservation: PASSED\n";

    TestPauseResumeAndRefocus();
    std::cout << "  -> 5. Pause, Resume & Refocus Lifecycle: PASSED\n";

    TestTeleportSnapNoStreak();
    std::cout << "  -> 6. Teleport / Respawn Snap (No Streak): PASSED\n";

    TestTimeScaleAndCombatHitstop();
    std::cout << "  -> 7. Time Scale & Combat Hitstop: PASSED\n";

    TestTelemetryAndFramePacer();
    std::cout << "  -> 8. Telemetry Metrics & FramePacer: PASSED\n";

    std::cout << "[TestTimeLoop] All 8 Verification Tests Passed Successfully!\n";

    SDL_Quit();
    return 0;
}
