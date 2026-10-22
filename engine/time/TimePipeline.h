#pragma once

#include "time/SdlClock.h"
#include "time/TimeTypes.h"

#include <algorithm>

namespace en {

class TimePipeline;

// Compact 32-byte non-owning simulation step view referencing Cache Line 0 of TimePipeline.
// Value type; passed to 120 Hz simulation ticks without copying the 64B BakedTickState table.
// Invariant: when valid == true, baked and pipeline are non-null and substepTotal >= 1.
struct alignas(32) SimStepView {
    const BakedTickState* baked{nullptr};
    TimePipeline* pipeline{nullptr};
    u64 tickIndex{0};
    u32 catchupIndex{0};
    u16 substepTotal{0};
    u8 frozenMask{0};
    bool valid{false};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid; }

    [[nodiscard]] constexpr f32 Dt(TimeChannel ch = TimeChannel::World) const noexcept {
        const u8 idx = static_cast<u8>(ch) & 7U;
        return ((frozenMask & (1U << idx)) != 0U || !baked) ? 0.0f : baked->scaledDt[idx];
    }

    [[nodiscard]] constexpr f32 EffectiveScale(TimeChannel ch = TimeChannel::World) const noexcept {
        const u8 idx = static_cast<u8>(ch) & 7U;
        return ((frozenMask & (1U << idx)) != 0U || !baked) ? 0.0f : baked->effectiveScale[idx];
    }

    [[nodiscard]] constexpr bool IsFrozen(TimeChannel ch = TimeChannel::World) const noexcept {
        return (frozenMask & (1U << (static_cast<u8>(ch) & 7U))) != 0U;
    }

    constexpr void RequestHitstop(TimeChannel ch, u32 ticks) const noexcept;
    constexpr void RequestMaskHitstop(u8 channelMask, u32 ticks) const noexcept;
    constexpr void RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) const noexcept;
};
static_assert(sizeof(SimStepView) == 32);

// Layer 1 constexpr 120 Hz rational time pipeline packed into 256 bytes (4 cache lines, zero padding).
// Uses a 1440 Hz LCM harmonic accumulator for zero-drift VSync snapping across 30..480 Hz (including 90/144/180/360 Hz)
// and an EWMA micro-jitter filter for VRR displays.
// Invariant: m_accUnits <= maxCatchupTicks * kTickUnits, and Channel 0 (World) cascades to Channels 1..5.
class alignas(64) TimePipeline {
public:
    static constexpr u64 kMaxFrameDeltaNs = 250'000'000ULL;
    static constexpr u64 kVsyncSnapMarginNs = 150'000ULL;
    static constexpr f32 kMaxTimeScale = 10.0f;

    // Bitmask of valid 1440 Hz harmonic multiples k in [3..48] (k = 1440 / RefreshHz):
    // k=3(480Hz), 4(360Hz), 6(240Hz), 8(180Hz), 9(160Hz), 10(144Hz), 12(120Hz),
    // 16(90Hz), 18(80Hz), 20(72Hz), 24(60Hz), 30(48Hz), 36(40Hz), 48(30Hz).
    static constexpr u64 kValidHarmonicMask =
        (1ULL << 3U) | (1ULL << 4U) | (1ULL << 6U) | (1ULL << 8U) | (1ULL << 9U) |
        (1ULL << 10U) | (1ULL << 12U) | (1ULL << 16U) | (1ULL << 18U) | (1ULL << 20U) |
        (1ULL << 24U) | (1ULL << 30U) | (1ULL << 36U) | (1ULL << 48U);

    constexpr TimePipeline() noexcept { Reset(); }

    constexpr void Reset() noexcept {
        m_accUnits = 0ULL;
        m_lastWallNs = 0ULL;
        m_telemetry = TimeTelemetry{};
        m_smoothedDeltaNs = 0ULL;
        m_masterScale = 1.0f;
        m_realDtSec = kFixedDtSec;
        m_stepAllowanceTicks = 0U;
        m_plannedSubsteps = 0U;
        m_activeHitstopMask = 0U;
        m_activeRampMask = 0U;
        m_frozenMask = 0U;
        m_paused = false;

        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            m_slots[i] = ChannelModSlot{};
            m_bakedTick.effectiveScale[i] = 1.0f;
            m_bakedTick.scaledDt[i] = kFixedDtSec;
        }
    }

    void BeginFrame() noexcept {
        const u64 now = SdlClock::NowNs();
        if (m_lastWallNs == 0ULL || now <= m_lastWallNs) {
            m_lastWallNs = now;
            BeginFrameManual(kFixedDtNs);
            return;
        }
        const u64 deltaNs = now - m_lastWallNs;
        m_lastWallNs = now;
        BeginFrameManual(deltaNs);
    }

    constexpr void BeginFrameManual(u64 rawDeltaNs) noexcept {
        const u64 clampedNs = std::min(rawDeltaNs, kMaxFrameDeltaNs);
        m_realDtSec = static_cast<f32>(clampedNs) * 1e-9f;
        ++m_telemetry.totalFrames;
        m_telemetry.ticksLastFrame = 0U;

        const u32 maxCatchup = std::max<u32>(1U, m_telemetry.maxCatchupTicks);
        if (m_paused) {
            m_plannedSubsteps = static_cast<u16>(std::min<u64>(m_accUnits / kTickUnits, maxCatchup));
            return;
        }

        // Stage 1: O(1) bitmask-driven harmonic snapping in 1440 Hz rational units.
        u64 units = clampedNs * kHarmonicHz;
        constexpr u64 kSnapMarginUnits = kVsyncSnapMarginNs * kHarmonicHz;
        const u64 nearestK = (units + (kNsPerSecond >> 1U)) / kNsPerSecond;
        if (nearestK <= 48ULL && ((kValidHarmonicMask >> nearestK) & 1ULL) != 0ULL) {
            const u64 targetUnits = nearestK * kNsPerSecond;
            const u64 diff = (units >= targetUnits) ? (units - targetUnits) : (targetUnits - units);
            if (diff <= kSnapMarginUnits) {
                units = targetUnits;
                m_smoothedDeltaNs = targetUnits / kHarmonicHz;
            } else {
                units = FilterVrrDeltaUnits(clampedNs);
            }
        } else {
            units = FilterVrrDeltaUnits(clampedNs);
        }

        // Stage 2: Accumulate and enforce spiral-of-death ceiling.
        m_accUnits += units;
        const u64 maxAccUnits = static_cast<u64>(maxCatchup) * kTickUnits;
        if (m_accUnits > maxAccUnits) {
            const u64 dropped = (m_accUnits - maxAccUnits + (kTickUnits - 1ULL)) / kTickUnits;
            m_telemetry.droppedTicks += static_cast<u32>(dropped);
            m_accUnits = maxAccUnits;
        }
        m_plannedSubsteps = static_cast<u16>(std::min<u64>(m_accUnits / kTickUnits, maxCatchup));
    }

    [[nodiscard]] constexpr SimStepView PollTick() noexcept {
        const u32 maxCatchup = std::max<u32>(1U, m_telemetry.maxCatchupTicks);
        if (m_accUnits < kTickUnits || m_telemetry.ticksLastFrame >= maxCatchup) {
            return SimStepView{};
        }
        if (m_paused && m_stepAllowanceTicks == 0U) {
            return SimStepView{};
        }

        m_accUnits -= kTickUnits;
        const u32 catchupIdx = m_telemetry.ticksLastFrame++;
        ++m_telemetry.totalSimTicks;

        if (m_stepAllowanceTicks > 0U) {
            --m_stepAllowanceTicks;
        }

        SimStepView view{};
        view.baked = &m_bakedTick;
        view.pipeline = this;
        view.tickIndex = m_telemetry.totalSimTicks;
        view.catchupIndex = catchupIdx;
        view.substepTotal = std::max<u16>(m_plannedSubsteps, m_telemetry.ticksLastFrame);
        view.frozenMask = m_frozenMask;
        view.valid = true;

        AdvanceChannelModulationTick();
        if (m_paused && m_stepAllowanceTicks == 0U) {
            RebakeAllChannels();
        }
        return view;
    }

    [[nodiscard]] constexpr RenderTimeView GetRenderView() const noexcept {
        RenderTimeView rv{};
        rv.rawAlpha = std::clamp(
            static_cast<f32>(m_accUnits) * (1.0f / static_cast<f32>(kTickUnits)), 0.0f, 1.0f);
        rv.realDt = m_realDtSec;
        rv.tickIndex = m_telemetry.totalSimTicks;
        const u8 frozen = FrozenMask();
        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            rv.channelAlpha[i] = ((frozen & (1U << i)) != 0U) ? 1.0f : rv.rawAlpha;
        }
        return rv;
    }

    constexpr void SetMasterScale(f32 scale) noexcept {
        if (!(scale >= 0.0f)) scale = 0.0f;
        m_masterScale = std::clamp(scale, 0.0f, kMaxTimeScale);
        RebakeAllChannels();
    }

    [[nodiscard]] constexpr f32 MasterScale() const noexcept { return m_masterScale; }

    constexpr void SetBaseScale(TimeChannel ch, f32 scale) noexcept {
        if (!(scale >= 0.0f)) scale = 0.0f;
        const u8 idx = static_cast<u8>(ch) & 7U;
        m_slots[idx].baseScale = std::clamp(scale, 0.0f, kMaxTimeScale);
        if (idx == 0U) {
            RebakeAllChannels();
        } else {
            BakeSingleChannel(idx);
        }
    }

    [[nodiscard]] constexpr f32 BaseScale(TimeChannel ch) const noexcept {
        return m_slots[static_cast<u8>(ch) & 7U].baseScale;
    }

    constexpr void RequestHitstop(TimeChannel ch, u32 ticks) noexcept {
        if (ticks == 0U) return;
        const u8 idx = static_cast<u8>(ch) & 7U;
        const u16 clamped = static_cast<u16>(std::min<u32>(ticks, 0xFFFFU));
        if (clamped >= m_slots[idx].hitstopTicks) {
            m_slots[idx].hitstopTicks = clamped;
            m_activeHitstopMask |= static_cast<u8>(1U << idx);
            if (idx == 0U) {
                RebakeAllChannels();
            } else {
                BakeSingleChannel(idx);
            }
        }
    }

    constexpr void RequestMaskHitstop(u8 channelMask, u32 ticks) noexcept {
        if (ticks == 0U || channelMask == 0U) return;
        const u16 clamped = static_cast<u16>(std::min<u32>(ticks, 0xFFFFU));
        u8 m = channelMask;
        while (m != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(m));
            m &= static_cast<u8>(m - 1U);
            if (clamped >= m_slots[idx].hitstopTicks) {
                m_slots[idx].hitstopTicks = clamped;
                m_activeHitstopMask |= static_cast<u8>(1U << idx);
            }
        }
        RebakeAllChannels();
    }

    constexpr void RequestGameplayFreeze(u32 ticks) noexcept {
        RequestMaskHitstop(kGameplayChannelsMask, ticks);
    }

    constexpr void RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) noexcept {
        const u8 idx = static_cast<u8>(ch) & 7U;
        if (holdTicks == 0U && blendTicks == 0U) {
            ClearChannelEffects(ch);
            return;
        }
        if (!(targetScale >= 0.0f)) targetScale = 0.0f;
        ChannelModSlot& s = m_slots[idx];
        s.rampTargetScale = std::clamp(targetScale, 0.0f, kMaxTimeScale);
        s.rampHoldTicks = static_cast<u16>(std::min<u32>(holdTicks, 0xFFFFU));
        s.rampBlendTicks = static_cast<u16>(std::min<u32>(blendTicks, 0xFFFFU));
        s.rampElapsedTicks = 0U;
        m_activeRampMask |= static_cast<u8>(1U << idx);
        if (idx == 0U) {
            RebakeAllChannels();
        } else {
            BakeSingleChannel(idx);
        }
    }

    constexpr void ClearChannelEffects(TimeChannel ch) noexcept {
        const u8 idx = static_cast<u8>(ch) & 7U;
        const u8 clearBit = static_cast<u8>(~(1U << idx));
        const f32 savedBase = m_slots[idx].baseScale;
        m_slots[idx] = ChannelModSlot{savedBase, 1.0f, 0U, 0U, 0U, 0U};
        m_activeHitstopMask &= clearBit;
        m_activeRampMask &= clearBit;
        if (idx == 0U) {
            RebakeAllChannels();
        } else {
            BakeSingleChannel(idx);
        }
    }

    constexpr void SetPaused(bool paused) noexcept {
        if (m_paused != paused) {
            m_paused = paused;
            if (!paused) {
                m_stepAllowanceTicks = 0U;
            }
            RebakeAllChannels();
        }
    }

    [[nodiscard]] constexpr bool IsPaused() const noexcept { return m_paused; }

    constexpr void RequestStep(u32 ticks = 1U) noexcept {
        if (ticks == 0U) return;
        const u32 maxCatchup = std::max<u32>(1U, m_telemetry.maxCatchupTicks);
        const u32 clamped = std::min(ticks, maxCatchup);
        m_stepAllowanceTicks = static_cast<u16>(std::min<u32>(m_stepAllowanceTicks + clamped, maxCatchup));
        const u64 requiredUnits = static_cast<u64>(m_stepAllowanceTicks) * kTickUnits;
        if (m_accUnits < requiredUnits) {
            m_accUnits = requiredUnits;
        }
        m_plannedSubsteps = static_cast<u16>(std::min<u64>(m_accUnits / kTickUnits, maxCatchup));
        RebakeAllChannels();
    }

    constexpr void OnWindowRefocus() noexcept {
        m_lastWallNs = 0ULL;
        m_smoothedDeltaNs = 0ULL;
    }

    [[nodiscard]] constexpr u8 FrozenMask() const noexcept {
        return (m_paused && m_stepAllowanceTicks == 0U)
                   ? static_cast<u8>(m_frozenMask | kGameplayChannelsMask)
                   : m_frozenMask;
    }

    [[nodiscard]] constexpr std::span<const f32, kMaxTimeChannels> EffectiveScales() const noexcept {
        return m_bakedTick.effectiveScale;
    }

    [[nodiscard]] constexpr u64 AccumulatorNs() const noexcept {
        return m_accUnits / kHarmonicHz;
    }

    [[nodiscard]] constexpr const TimeTelemetry& Telemetry() const noexcept { return m_telemetry; }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = Mix64(m_telemetry.totalSimTicks ^ m_accUnits);
        h = HashCombine64(h, static_cast<u64>(FrozenMask()) |
                             (static_cast<u64>(m_activeHitstopMask) << 8U) |
                             (static_cast<u64>(m_activeRampMask) << 16U));
        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            h = HashCombine64(h, std::bit_cast<u32>(m_bakedTick.scaledDt[i]));
        }
        return h;
    }

private:
    // VRR micro-jitter filter: smooths frame-to-frame fluctuations within +-20% while passing step changes immediately.
    [[nodiscard]] constexpr u64 FilterVrrDeltaUnits(u64 clampedNs) noexcept {
        if (m_smoothedDeltaNs == 0ULL) {
            m_smoothedDeltaNs = clampedNs;
            return clampedNs * kHarmonicHz;
        }
        const u64 diff = (clampedNs >= m_smoothedDeltaNs)
                             ? (clampedNs - m_smoothedDeltaNs)
                             : (m_smoothedDeltaNs - clampedNs);
        if (diff * 5ULL <= m_smoothedDeltaNs) {
            m_smoothedDeltaNs = (m_smoothedDeltaNs * 3ULL + clampedNs + 2ULL) >> 2U;
            return m_smoothedDeltaNs * kHarmonicHz;
        }
        m_smoothedDeltaNs = clampedNs;
        return clampedNs * kHarmonicHz;
    }

    [[nodiscard]] constexpr f32 EvaluateChannelLocalScale(u8 idx) const noexcept {
        const ChannelModSlot& s = m_slots[idx];
        if (s.hitstopTicks > 0U) return 0.0f;
        f32 rampScale = 1.0f;
        if ((m_activeRampMask & (1U << idx)) != 0U) {
            if (s.rampElapsedTicks < s.rampHoldTicks) {
                rampScale = s.rampTargetScale;
            } else if (s.rampBlendTicks > 0U) {
                const u32 blendElapsed = static_cast<u32>(s.rampElapsedTicks - s.rampHoldTicks);
                const f32 rawT = std::clamp(
                    static_cast<f32>(blendElapsed) / static_cast<f32>(s.rampBlendTicks), 0.0f, 1.0f);
                const f32 smoothT = rawT * rawT * (3.0f - 2.0f * rawT);
                rampScale = s.rampTargetScale + (1.0f - s.rampTargetScale) * smoothT;
            }
        }
        return s.baseScale * rampScale;
    }

    constexpr void BakeSingleChannel(u8 idx) noexcept {
        const u8 bit = static_cast<u8>(1U << idx);
        const bool pausedGameplay = (m_paused && m_stepAllowanceTicks == 0U && idx <= 5U);
        if (pausedGameplay) {
            m_frozenMask |= bit;
            m_bakedTick.effectiveScale[idx] = 0.0f;
            m_bakedTick.scaledDt[idx] = 0.0f;
            return;
        }

        const f32 parentScale = (idx >= 1U && idx <= 5U) ? m_bakedTick.effectiveScale[0] : m_masterScale;
        const f32 eff = std::clamp(parentScale * EvaluateChannelLocalScale(idx), 0.0f, kMaxTimeScale);
        m_bakedTick.effectiveScale[idx] = eff;
        m_bakedTick.scaledDt[idx] = eff * kFixedDtSec;
        if (eff <= 1e-6f) {
            m_frozenMask |= bit;
        } else {
            m_frozenMask &= static_cast<u8>(~bit);
        }
    }

    constexpr void RebakeAllChannels() noexcept {
        for (u8 idx = 0; idx < kMaxTimeChannels; ++idx) {
            BakeSingleChannel(idx);
        }
    }

    constexpr void AdvanceChannelModulationTick() noexcept {
        u8 activeMask = static_cast<u8>(m_activeHitstopMask | m_activeRampMask);
        if (activeMask == 0U) return;

        bool worldChanged = false;
        while (activeMask != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(activeMask));
            activeMask &= static_cast<u8>(activeMask - 1U);
            const u8 bit = static_cast<u8>(1U << idx);
            ChannelModSlot& s = m_slots[idx];

            if (s.hitstopTicks > 0U) {
                --s.hitstopTicks;
                if (s.hitstopTicks == 0U) {
                    m_activeHitstopMask &= static_cast<u8>(~bit);
                }
            } else if ((m_activeRampMask & bit) != 0U) {
                ++s.rampElapsedTicks;
                const u32 totalRamp = static_cast<u32>(s.rampHoldTicks) + static_cast<u32>(s.rampBlendTicks);
                if (s.rampElapsedTicks >= totalRamp) {
                    s.rampTargetScale = 1.0f;
                    s.rampHoldTicks = 0U;
                    s.rampBlendTicks = 0U;
                    s.rampElapsedTicks = 0U;
                    m_activeRampMask &= static_cast<u8>(~bit);
                }
            }

            if (idx == 0U) {
                worldChanged = true;
            } else if (!worldChanged) {
                BakeSingleChannel(idx);
            }
        }

        if (worldChanged) {
            RebakeAllChannels();
        }
    }

    // Cache Line 0 (offset 0..63): Baked per-channel dt and effective scale lookup table.
    BakedTickState m_bakedTick{};

    // Cache Lines 1 & 2 (offset 64..191): 8 x 16B SIMD-aligned channel modulation slots.
    std::array<ChannelModSlot, kMaxTimeChannels> m_slots{};

    // Cache Line 3 (offset 192..255): Rational accumulator, telemetry, VRR filter, and control flags (64B exact).
    u64 m_accUnits{0};
    u64 m_lastWallNs{0};
    TimeTelemetry m_telemetry{};
    u64 m_smoothedDeltaNs{0};
    f32 m_masterScale{1.0f};
    f32 m_realDtSec{kFixedDtSec};
    u16 m_stepAllowanceTicks{0};
    u16 m_plannedSubsteps{0};
    u8 m_activeHitstopMask{0};
    u8 m_activeRampMask{0};
    u8 m_frozenMask{0};
    bool m_paused{false};
};
static_assert(sizeof(TimePipeline) == 256);

constexpr void SimStepView::RequestHitstop(TimeChannel ch, u32 ticks) const noexcept {
    if (pipeline) pipeline->RequestHitstop(ch, ticks);
}

constexpr void SimStepView::RequestMaskHitstop(u8 channelMask, u32 ticks) const noexcept {
    if (pipeline) pipeline->RequestMaskHitstop(channelMask, ticks);
}

constexpr void SimStepView::RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) const noexcept {
    if (pipeline) pipeline->RequestTimeRamp(ch, targetScale, holdTicks, blendTicks);
}

// Adaptive hybrid SDL3-sleep + sub-millisecond spin-wait frame pacer packed into 32 bytes.
// Tracks an EWMA of OS sleep overshoot (m_sleepMarginNs) to hit frame deadlines without 100% CPU burn.
// Invariant: m_sleepMarginNs stays clamped within [kMinSpinMarginNs, kMaxSpinMarginNs].
class alignas(8) FramePacer {
public:
    static constexpr u32 kMinSpinMarginNs = 250'000U;
    static constexpr u32 kDefaultSpinMarginNs = 1'500'000U;
    static constexpr u32 kMaxSpinMarginNs = 4'000'000U;

    constexpr explicit FramePacer(u32 targetFps = 120U) noexcept { SetTargetFps(targetFps); }

    constexpr void SetTargetFps(u32 targetFps) noexcept {
        m_targetFps = targetFps;
        m_targetFrameNs = (targetFps > 0U) ? (kNsPerSecond / targetFps) : 0ULL;
    }

    void BeginFrame() noexcept { m_frameStartNs = SdlClock::NowNs(); }
    void EndFrameAndPace() noexcept;

    [[nodiscard]] constexpr u32 TargetFps() const noexcept { return m_targetFps; }
    [[nodiscard]] constexpr u64 TargetFrameNs() const noexcept { return m_targetFrameNs; }
    [[nodiscard]] constexpr u32 LastSleepNs() const noexcept { return m_lastSleepNs; }
    [[nodiscard]] constexpr u32 LastSpinNs() const noexcept { return m_lastSpinNs; }
    [[nodiscard]] constexpr u32 SleepMarginNs() const noexcept { return m_sleepMarginNs; }

private:
    u64 m_frameStartNs{0};
    u64 m_targetFrameNs{kNsPerSecond / 120ULL};
    u32 m_lastSleepNs{0};
    u32 m_lastSpinNs{0};
    u32 m_sleepMarginNs{kDefaultSpinMarginNs};
    u32 m_targetFps{120};
};
static_assert(sizeof(FramePacer) == 32);

} // namespace en
