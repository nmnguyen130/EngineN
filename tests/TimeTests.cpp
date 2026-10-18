#include "core/time/SdlClock.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

extern "C" en::u64 SDL_GetTicksNS(void) noexcept {
    static en::u64 s_fakeOsNs = 100'000'000ULL;
    s_fakeOsNs += en::time::kFixedDtNs;
    return s_fakeOsNs;
}

namespace {

using namespace en;
using namespace en::time;

enum class CustomChannel : u8 {
    World  = 0,
    Hero   = 1,
    Boss   = 2,
    Bullet = 3
};

// 1. Compile-time static layout, alignment, and literal verification
static_assert(6_f60 == 12, "60FPS to 120Hz tick literal conversion failed.");
static_assert(15_ticks == 15, "Tick literal failed.");
static_assert(sizeof(BakedTickState) == 64 && alignof(BakedTickState) == 64);
static_assert(sizeof(BakedRenderState) == 32 && alignof(BakedRenderState) == 32);
static_assert(sizeof(RenderTimeView) == 64 && alignof(RenderTimeView) == 64);
static_assert(sizeof(ChannelModulator::ChannelModSlot) == 16 && alignof(ChannelModulator::ChannelModSlot) == 16);
static_assert(sizeof(TimePipelineConfig) == 24);
static_assert(MakeChannelMask(TimeChannel::World, TimeChannel::Player, TimeChannel::Ui) == 0b01000011u);

// 2. Compile-time constexpr end-to-end simulation verification
consteval bool VerifyConstexprSimulation() noexcept {
    TimePipeline<ManualClock> pipe{};
    pipe.Clock().AdvanceNs(50'000'000ULL);
    pipe.BeginFrame(); // First frame anchors clock baseline (0 delta)
    if (pipe.PollTick()) {
        return false;
    }

    // 60Hz VSync frame (16,666,667 ns) -> snaps to exact 2 ticks
    pipe.Clock().AdvanceNs(16'666'667ULL);
    pipe.BeginFrame();

    u32 tickCount = 0;
    while (const auto step = pipe.PollTick()) {
        if (tickCount == 0) {
            step.RequestHitstop(CustomChannel::Hero, 2_ticks, 0.0f);
            if (!step.IsFrozen(CustomChannel::Hero) || step.Dt(CustomChannel::Hero) != 0.0f) {
                return false;
            }
        }
        ++tickCount;
    }
    if (tickCount != 2 || pipe.Telemetry().totalSimTicks != 2) {
        return false;
    }
    if (pipe.Modulator().RemainingHitstopTicks(CustomChannel::Hero) != 0 ||
        pipe.Modulator().IsFrozen(CustomChannel::Hero)) {
        return false;
    }

    // Multi-frame pause step (1_f60 == 2 ticks) while paused
    pipe.SetPaused(true);
    pipe.RequestStep(1_f60);
    pipe.BeginFrameManual(kFixedDtNs);
    u32 pausedSteps = 0;
    while (pipe.PollTick()) {
        ++pausedSteps;
    }
    if (pausedSteps != 2 || pipe.Telemetry().totalSimTicks != 4) {
        return false;
    }

    return pipe.ComputeAuditHash() != 0;
}

static_assert(VerifyConstexprSimulation(), "Compile-time constexpr TimePipeline verification failed.");

void TestVsyncHarmonicsAndDrift() {
    TimePipeline<ManualClock> pipe{};

    // 480Hz (2,083,333 ns +/- jitter): 4 frames == 1 exact tick, 0 remainder
    const u64 frames480[4] = { 2'083'333ULL, 2'100'000ULL, 2'050'000ULL, 2'083'334ULL };
    for (int i = 0; i < 4; ++i) {
        pipe.BeginFrameManual(frames480[i]);
        u32 ticks = 0;
        while (pipe.PollTick()) { ++ticks; }
        assert(ticks == (i == 3 ? 1u : 0u));
    }
    assert(pipe.Telemetry().accumulatorNs == 0);
    assert(pipe.GetRenderView().rawAlpha == 0.0f);

    // 240Hz (4,166,667 ns): 2 frames == 1 exact tick, 0 remainder, alpha == 0.5 on frame 0
    pipe.Reset();
    pipe.BeginFrameManual(4'166'667ULL);
    assert(!pipe.PollTick());
    assert(std::fabs(pipe.GetRenderView().rawAlpha - 0.5f) < 1e-6f);
    pipe.BeginFrameManual(4'166'667ULL);
    u32 ticks240 = 0;
    while (pipe.PollTick()) { ++ticks240; }
    assert(ticks240 == 1);
    assert(pipe.Telemetry().accumulatorNs == 0);

    // 120Hz (8,333,333 ns): 1 frame == 1 exact tick
    pipe.Reset();
    pipe.BeginFrameManual(kFixedDtNs);
    u32 ticks120 = 0;
    while (pipe.PollTick()) { ++ticks120; }
    assert(ticks120 == 1 && pipe.Telemetry().accumulatorNs == 0);

    // 60Hz (16,666,667 ns): 1 frame == 2 exact ticks
    pipe.Reset();
    pipe.BeginFrameManual(16'666'667ULL);
    u32 ticks60 = 0;
    while (pipe.PollTick()) { ++ticks60; }
    assert(ticks60 == 2 && pipe.Telemetry().accumulatorNs == 0);

    // 30Hz (33,333,333 ns): 1 frame == 4 exact ticks
    pipe.Reset();
    pipe.BeginFrameManual(33'333'333ULL);
    u32 ticks30 = 0;
    while (pipe.PollTick()) { ++ticks30; }
    assert(ticks30 == 4 && pipe.Telemetry().accumulatorNs == 0);

    // Non-harmonic 144Hz (6,944,444 ns) must NOT snap; preserves smooth sub-frame alpha
    pipe.Reset();
    pipe.BeginFrameManual(6'944'444ULL);
    assert(!pipe.PollTick());
    assert(pipe.GetRenderView().rawAlpha > 0.83f && pipe.GetRenderView().rawAlpha < 0.84f);
}

void TestSpiralOfDeathAndCatchupClamp() {
    TimePipeline<ManualClock> pipe{};
    pipe.BeginFrameManual(10 * kFixedDtNs);

    u32 executed = 0;
    while (const auto step = pipe.PollTick()) {
        assert(step.CatchupIndex() == executed);
        ++executed;
    }
    assert(executed == 8);
    assert(pipe.Telemetry().ticksLastFrame == 8);
    assert(pipe.Telemetry().droppedTicks == 2);
    assert(pipe.Telemetry().accumulatorNs == 0);

    // Abnormal 1-second OS spike (1,000,000,000 ns) clamps to maxFrameDeltaNs (250ms == 30 ticks)
    pipe.Reset();
    pipe.BeginFrameManual(1'000'000'000ULL);
    u32 spikeTicks = 0;
    while (pipe.PollTick()) { ++spikeTicks; }
    assert(spikeTicks == 8);
    assert(pipe.Telemetry().droppedTicks == 22); // 30 clamped ticks - 8 executed == 22 dropped
}

void TestHitstopPrecedenceAndRampFreezeInteraction() {
    ChannelModulator mod{};

    // Duration-dominant merge rule; on equal duration, lower intensity (harder freeze) wins
    mod.RequestHitstop(TimeChannel::Player, 4, 0.5f);
    assert(mod.RemainingHitstopTicks(TimeChannel::Player) == 4);
    assert(std::fabs(mod.EffectiveScale(TimeChannel::Player) - 0.5f) < 1e-6f);
    assert(!mod.IsFrozen(TimeChannel::Player));

    // Shorter duration is ignored even if harder freeze
    mod.RequestHitstop(TimeChannel::Player, 2, 0.0f);
    assert(mod.RemainingHitstopTicks(TimeChannel::Player) == 4);
    assert(std::fabs(mod.EffectiveScale(TimeChannel::Player) - 0.5f) < 1e-6f);

    // Equal duration with harder freeze (0.0f < 0.5f) wins and sets frozen bit
    mod.RequestHitstop(TimeChannel::Player, 4, 0.0f);
    assert(mod.IsFrozen(TimeChannel::Player));
    assert(mod.Dt(TimeChannel::Player) == 0.0f);

    // Time ramp on Enemy paused while hard-frozen by hitstop across ALL frozen ticks
    mod.RequestTimeRamp(TimeChannel::Enemy, 0.25f, 2, 4);
    mod.RequestHitstop(TimeChannel::Enemy, 2, 0.0f);
    assert(mod.IsFrozen(TimeChannel::Enemy));
    assert(mod. BakeRenderState(0.35f).alpha[ToChannelIndex(TimeChannel::Enemy)] == 1.0f);

    mod.AdvanceTick(); // Frozen tick 1 -> hitstop 2 -> 1, ramp hold stays 2
    assert(mod.IsFrozen(TimeChannel::Enemy));
    mod.AdvanceTick(); // Frozen tick 2 -> hitstop 1 -> 0, ramp hold STILL stays 2
    assert(!mod.IsFrozen(TimeChannel::Enemy));
    assert(std::fabs(mod.EffectiveScale(TimeChannel::Enemy) - 0.25f) < 1e-6f);

    // Now un-frozen: 2 hold ticks + 4 blend ticks
    mod.AdvanceTick(); // hold: 2 -> 1
    assert(std::fabs(mod.EffectiveScale(TimeChannel::Enemy) - 0.25f) < 1e-6f);
    mod.AdvanceTick(); // hold: 1 -> 0 (blend elapsed == 0 -> still 0.25f)
    assert(std::fabs(mod.EffectiveScale(TimeChannel::Enemy) - 0.25f) < 1e-6f);

    f32 prevScale = mod.EffectiveScale(TimeChannel::Enemy);
    for (int i = 0; i < 4; ++i) {
        mod.AdvanceTick();
        const f32 curScale = mod.EffectiveScale(TimeChannel::Enemy);
        assert(curScale > prevScale);
        prevScale = curScale;
    }
    assert(mod.ActiveRampMask() == 0);
    assert(std::fabs(mod.EffectiveScale(TimeChannel::Enemy) - 1.0f) < 1e-6f);

    // NaN / negative sanitization safety
    mod.SetBaseScale(TimeChannel::World, std::numeric_limits<f32>::quiet_NaN());
    assert(mod.GetBaseScale(TimeChannel::World) == 0.0f);
    mod.SetBaseScale(TimeChannel::World, 1.0f);
}

void TestMultiFramePauseAndGameplayFreeze() {
    TimeManager mgr{};
    mgr.BeginFrame(); // Anchor SdlClock

    // 1. Multi-frame gameplay freeze (freezes World..Camera 0x3F while Ui remains unfrozen)
    mgr.RequestGameplayFreeze(2_f60, 0.0f); // 4 ticks at 120Hz
    assert(mgr.FrozenMask() == kGameplayChannelsMask);
    assert(!mgr.Modulator().IsFrozen(TimeChannel::Ui));
    assert(mgr.Modulator().IsFrozen(TimeChannel::Player));

    for (u32 i = 0; i < 4; ++i) {
        mgr.BeginFrameManual(kFixedDtNs);
        while (const auto step = mgr.PollTick()) {
            assert(step.IsFrozen(TimeChannel::World));
            assert(!step.IsFrozen(TimeChannel::Ui));
        }
    }
    assert(mgr.FrozenMask() == 0);

    // 2. Hard clock pause + multi-frame stepping (e.g. stepping 2_f60 == 4 ticks while paused)
    mgr.SetPaused(true);
    assert(mgr.IsPaused());
    assert(mgr.FrozenMask() == kGameplayChannelsMask);
    assert(mgr.GetRenderView().Alpha(TimeChannel::World) == 1.0f);

    // Normal frames while paused do not advance simulation ticks
    const u64 ticksBefore = mgr.Telemetry().totalSimTicks;
    mgr.BeginFrameManual(kFixedDtNs * 3);
    assert(!mgr.PollTick());
    assert(mgr.Telemetry().totalSimTicks == ticksBefore);

    // Step 2_f60 (4 simulation ticks) in a single paused frame
    mgr.RequestStep(2_f60);
    mgr.BeginFrameManual(kFixedDtNs);
    u32 stepped = 0;
    while (mgr.PollTick()) {
        ++stepped;
    }
    assert(stepped == 4);
    assert(mgr.Telemetry().totalSimTicks == ticksBefore + 4);

    // Subsequent paused frame remains still
    mgr.BeginFrameManual(kFixedDtNs);
    assert(!mgr.PollTick());

    // Unpause and verify deterministic audit hash
    mgr.SetPaused(false);
    assert(!mgr.IsPaused());
    assert(mgr.ComputeAuditHash() != 0);
}

} // namespace

int main() {
    TestVsyncHarmonicsAndDrift();
    TestSpiralOfDeathAndCatchupClamp();
    TestHitstopPrecedenceAndRampFreezeInteraction();
    TestMultiFramePauseAndGameplayFreeze();
    std::printf("All EngineN Phase 1 (core/time) unit and constexpr tests passed!\n");
    return 0;
}
