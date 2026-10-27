#pragma once

#include "time/SdlClock.h"
#include "time/TimeTypes.h"

#include <bit>
#include <span>

namespace en {

class TimePipeline;

// Non-owning per-tick view into TimePipeline passed to 120 Hz simulation steps.
// Value type; references the pipeline's baked channel table without copying it.
// Invariant: when valid is true, baked and pipeline are non-null and substepTotal >= 1.
struct alignas(32) SimStepView {
    const BakedTickState* baked{nullptr};
    TimePipeline* pipeline{nullptr};
    u64 tickIndex{0};
    u32 catchupIndex{0};
    u16 substepTotal{0};
    u8 frozenMask{0};
    bool valid{false};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid; }

    [[nodiscard]] constexpr bool IsFrozen(TimeChannel ch = TimeChannel::World) const noexcept {
        return (frozenMask & ChannelBit(ch)) != 0U;
    }

    [[nodiscard]] constexpr f32 Dt(TimeChannel ch = TimeChannel::World) const noexcept {
        return (IsFrozen(ch) || !baked) ? 0.0f : baked->scaledDt[ChannelIdx(ch)];
    }

    [[nodiscard]] constexpr f32 EffectiveScale(TimeChannel ch = TimeChannel::World) const noexcept {
        return (IsFrozen(ch) || !baked) ? 0.0f : baked->effectiveScale[ChannelIdx(ch)];
    }

    constexpr void RequestHitstop(TimeChannel ch, u32 ticks) const noexcept;
    constexpr void RequestMaskHitstop(u8 channelMask, u32 ticks) const noexcept;
    constexpr void RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) const noexcept;
};
static_assert(sizeof(SimStepView) == 32);

// Fixed-timestep 120 Hz simulation clock with 1440 Hz rational VSync snapping and VRR smoothing.
// Self-contained value type; owns no external resources.
// Invariant: m_accUnits <= maxCatchupTicks * kTickUnits, and Channel 0 (World) cascades to Channels 1..5.
class alignas(64) TimePipeline {
public:
    static constexpr u64 kMaxFrameDeltaNs = 250'000'000ULL;
    static constexpr u64 kVsyncSnapMarginNs = 150'000ULL;
    static constexpr f32 kMaxTimeScale = 10.0f;

    // Bitmask of valid 1440 Hz divisors k in [3..48] (k = 1440 / RefreshHz).
    static constexpr u64 kValidHarmonicMask =
        (1ULL << 3U) | (1ULL << 4U) | (1ULL << 6U) | (1ULL << 8U) | (1ULL << 9U) |
        (1ULL << 10U) | (1ULL << 12U) | (1ULL << 16U) | (1ULL << 18U) | (1ULL << 20U) |
        (1ULL << 24U) | (1ULL << 30U) | (1ULL << 36U) | (1ULL << 48U);

    constexpr TimePipeline() noexcept {
        m_bakedTick.effectiveScale.fill(1.0f);
        m_bakedTick.scaledDt.fill(kFixedDtSec);
    }

    constexpr void Reset() noexcept {
        *this = TimePipeline{};
    }

    void BeginFrame() noexcept {
        const u64 now = SdlClock::NowNs();
        const u64 delta = (m_lastWallNs != 0ULL && now > m_lastWallNs) ? (now - m_lastWallNs) : kFixedDtNs;
        m_lastWallNs = now;
        BeginFrameManual(delta);
    }

    constexpr void BeginFrameManual(u64 rawDeltaNs) noexcept {
        const u64 clampedNs = std::min(rawDeltaNs, kMaxFrameDeltaNs);
        m_realDtSec = static_cast<f32>(clampedNs) * 1e-9f;
        ++m_telemetry.totalFrames;
        m_telemetry.ticksLastFrame = 0U;

        const u32 maxCatchup = m_telemetry.maxCatchupTicks;
        if (!m_paused) {
            // Clamp accumulated time to maxCatchup ticks to prevent spiral-of-death stalls.
            m_accUnits += ResolveFrameUnits(clampedNs);
            const u64 maxAccUnits = static_cast<u64>(maxCatchup) * kTickUnits;
            if (m_accUnits > maxAccUnits) {
                const u64 dropped = (m_accUnits - maxAccUnits + (kTickUnits - 1ULL)) / kTickUnits;
                m_telemetry.droppedTicks += static_cast<u32>(dropped);
                m_accUnits = maxAccUnits;
            }
        }
        m_plannedSubsteps = static_cast<u16>(m_accUnits / kTickUnits);
    }

    [[nodiscard]] constexpr SimStepView PollTick() noexcept {
        const u32 maxCatchup = m_telemetry.maxCatchupTicks;
        if (m_accUnits < kTickUnits || m_telemetry.ticksLastFrame >= maxCatchup) return {};
        if (m_paused && m_stepAllowanceTicks == 0U) return {};

        m_accUnits -= kTickUnits;
        const u32 catchupIdx = m_telemetry.ticksLastFrame++;
        ++m_telemetry.totalSimTicks;
        if (m_stepAllowanceTicks > 0U) --m_stepAllowanceTicks;

        SimStepView view{
            .baked = &m_bakedTick,
            .pipeline = this,
            .tickIndex = m_telemetry.totalSimTicks,
            .catchupIndex = catchupIdx,
            .substepTotal = std::max<u16>(m_plannedSubsteps, m_telemetry.ticksLastFrame),
            .frozenMask = m_frozenMask,
            .valid = true,
        };

        AdvanceChannelModulationTick();
        if (m_paused && m_stepAllowanceTicks == 0U) RebakeChannels(0xFFU);
        return view;
    }

    [[nodiscard]] constexpr RenderTimeView GetRenderView() const noexcept {
        const f32 alpha = std::clamp(
            static_cast<f32>(m_accUnits) * (1.0f / static_cast<f32>(kTickUnits)), 0.0f, 1.0f);
        RenderTimeView rv{
            .tickIndex = m_telemetry.totalSimTicks,
            .rawAlpha = alpha,
            .realDt = m_realDtSec,
        };
        const u8 frozen = FrozenMask();
        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            rv.channelAlpha[i] = ((frozen & (1U << i)) != 0U) ? 1.0f : alpha;
        }
        return rv;
    }

    constexpr void SetMaxCatchupTicks(u16 maxCatchup) noexcept {
        m_telemetry.maxCatchupTicks = std::clamp<u16>(maxCatchup, 1U, 64U);
    }

    constexpr void SetMasterScale(f32 scale) noexcept {
        m_masterScale = SanitizeScale(scale);
        RebakeChannels(0xFFU);
    }

    [[nodiscard]] constexpr f32 MasterScale() const noexcept { return m_masterScale; }

    constexpr void SetBaseScale(TimeChannel ch, f32 scale) noexcept {
        m_slots[ChannelIdx(ch)].baseScale = SanitizeScale(scale);
        RebakeChannels(ChannelBit(ch));
    }

    [[nodiscard]] constexpr f32 BaseScale(TimeChannel ch) const noexcept {
        return m_slots[ChannelIdx(ch)].baseScale;
    }

    constexpr void RequestHitstop(TimeChannel ch, u32 ticks) noexcept {
        RequestMaskHitstop(ChannelBit(ch), ticks);
    }

    constexpr void RequestMaskHitstop(u8 channelMask, u32 ticks) noexcept {
        if (ticks == 0U || channelMask == 0U) return;
        const u16 clamped = static_cast<u16>(std::min<u32>(ticks, kMaxFreezeTicks));
        u8 dirtyMask = 0U;
        for (u8 m = channelMask; m != 0U; m &= static_cast<u8>(m - 1U)) {
            const u8 idx = static_cast<u8>(std::countr_zero(m));
            if (clamped >= m_slots[idx].hitstopTicks) {
                m_slots[idx].hitstopTicks = clamped;
                dirtyMask |= static_cast<u8>(1U << idx);
            }
        }
        m_activeHitstopMask |= dirtyMask;
        RebakeChannels(dirtyMask);
    }

    constexpr void RequestGameplayFreeze(u32 ticks) noexcept {
        RequestMaskHitstop(kGameplayChannelsMask, ticks);
    }

    constexpr void RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) noexcept {
        const u8 idx = ChannelIdx(ch);
        const u8 bit = ChannelBit(ch);
        if (holdTicks == 0U && blendTicks == 0U) {
            m_slots[idx].ClearRamp();
            m_activeRampMask &= static_cast<u8>(~bit);
            RebakeChannels(bit);
            return;
        }
        m_slots[idx].rampTargetScale = SanitizeScale(targetScale);
        m_slots[idx].rampHoldTicks = static_cast<u16>(std::min<u32>(holdTicks, 0xFFFFU));
        m_slots[idx].rampBlendTicks = static_cast<u16>(std::min<u32>(blendTicks, 0xFFFFU));
        m_slots[idx].rampElapsedTicks = 0U;
        m_activeRampMask |= bit;
        RebakeChannels(bit);
    }

    constexpr void ClearChannelEffects(TimeChannel ch) noexcept {
        const u8 idx = ChannelIdx(ch);
        const u8 bit = ChannelBit(ch);
        m_slots[idx].ClearEffects();
        m_activeHitstopMask &= static_cast<u8>(~bit);
        m_activeRampMask &= static_cast<u8>(~bit);
        RebakeChannels(bit);
    }

    constexpr void SetPaused(bool paused) noexcept {
        if (m_paused != paused) {
            m_paused = paused;
            if (!paused) m_stepAllowanceTicks = 0U;
            RebakeChannels(0xFFU);
        }
    }

    [[nodiscard]] constexpr bool IsPaused() const noexcept { return m_paused; }

    constexpr void RequestStep(u32 ticks = 1U) noexcept {
        if (ticks == 0U) return;
        const u32 maxCatchup = m_telemetry.maxCatchupTicks;
        m_stepAllowanceTicks = static_cast<u16>(
            std::min<u32>(m_stepAllowanceTicks + std::min(ticks, maxCatchup), maxCatchup));
        m_accUnits = std::max(m_accUnits, static_cast<u64>(m_stepAllowanceTicks) * kTickUnits);
        m_plannedSubsteps = static_cast<u16>(std::min<u64>(m_accUnits / kTickUnits, maxCatchup));
        RebakeChannels(0xFFU);
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

    [[nodiscard]] constexpr u64 AccumulatorNs() const noexcept { return m_accUnits / kHarmonicHz; }
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
    [[nodiscard]] static constexpr f32 SanitizeScale(f32 scale) noexcept {
        return (scale >= 0.0f) ? std::clamp(scale, 0.0f, kMaxTimeScale) : 0.0f;
    }

    [[nodiscard]] static constexpr u64 AbsDiff(u64 a, u64 b) noexcept {
        return (a >= b) ? (a - b) : (b - a);
    }

    // Snaps standard VSync frame intervals within +-150us, or smooths VRR micro-jitter within +-20%.
    [[nodiscard]] constexpr u64 ResolveFrameUnits(u64 clampedNs) noexcept {
        const u64 units = clampedNs * kHarmonicHz;
        const u64 nearestK = (units + (kNsPerSecond >> 1U)) / kNsPerSecond;
        if (nearestK <= 48ULL && ((kValidHarmonicMask >> nearestK) & 1ULL) != 0ULL) {
            const u64 targetUnits = nearestK * kNsPerSecond;
            if (AbsDiff(units, targetUnits) <= kVsyncSnapMarginNs * kHarmonicHz) {
                m_smoothedDeltaNs = targetUnits / kHarmonicHz;
                return targetUnits;
            }
        }
        if (m_smoothedDeltaNs != 0ULL && AbsDiff(clampedNs, m_smoothedDeltaNs) * 5ULL <= m_smoothedDeltaNs) {
            m_smoothedDeltaNs = (m_smoothedDeltaNs * 3ULL + clampedNs + 2ULL) >> 2U;
            return m_smoothedDeltaNs * kHarmonicHz;
        }
        m_smoothedDeltaNs = clampedNs;
        return units;
    }

    constexpr void BakeSingleChannel(u8 idx) noexcept {
        const u8 bit = static_cast<u8>(1U << idx);
        if (m_paused && m_stepAllowanceTicks == 0U && idx <= 5U) {
            m_frozenMask |= bit;
            m_bakedTick.effectiveScale[idx] = 0.0f;
            m_bakedTick.scaledDt[idx] = 0.0f;
            return;
        }

        const f32 parentScale = (idx >= 1U && idx <= 5U) ? m_bakedTick.effectiveScale[0] : m_masterScale;
        const f32 eff = std::clamp(
            parentScale * m_slots[idx].EvaluateLocalScale((m_activeRampMask & bit) != 0U), 0.0f, kMaxTimeScale);
        m_bakedTick.effectiveScale[idx] = eff;
        m_bakedTick.scaledDt[idx] = eff * kFixedDtSec;
        if (eff <= 1e-6f) m_frozenMask |= bit;
        else m_frozenMask &= static_cast<u8>(~bit);
    }

    // Rebakes dirty channels; if World (bit 0) is dirty, cascades to gameplay channels 0..5.
    constexpr void RebakeChannels(u8 dirtyMask) noexcept {
        if ((dirtyMask & 1U) != 0U) dirtyMask |= kGameplayChannelsMask;
        for (u8 m = dirtyMask; m != 0U; m &= static_cast<u8>(m - 1U)) {
            BakeSingleChannel(static_cast<u8>(std::countr_zero(m)));
        }
    }

    constexpr void AdvanceChannelModulationTick() noexcept {
        const u8 activeMask = static_cast<u8>(m_activeHitstopMask | m_activeRampMask);
        if (activeMask == 0U) return;

        for (u8 m = activeMask; m != 0U; m &= static_cast<u8>(m - 1U)) {
            const u8 idx = static_cast<u8>(std::countr_zero(m));
            const u8 bit = static_cast<u8>(1U << idx);
            ChannelModSlot& s = m_slots[idx];

            if (s.hitstopTicks > 0U) {
                if (--s.hitstopTicks == 0U) m_activeHitstopMask &= static_cast<u8>(~bit);
            } else if ((m_activeRampMask & bit) != 0U) {
                if (++s.rampElapsedTicks >= static_cast<u32>(s.rampHoldTicks) + static_cast<u32>(s.rampBlendTicks)) {
                    s.ClearEffects();
                    m_activeRampMask &= static_cast<u8>(~bit);
                }
            }
        }
        RebakeChannels(activeMask);
    }

    BakedTickState m_bakedTick{};
    std::array<ChannelModSlot, kMaxTimeChannels> m_slots{};

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

// Hybrid sleep and sub-millisecond spin-wait frame limiter.
// Tracks an exponentially weighted moving average of OS sleep overshoot to meet frame deadlines.
// Invariant: m_sleepMarginNs stays within [kMinSpinMarginNs, kMaxSpinMarginNs] (<= 0.3 ms).
class alignas(8) FramePacer {
public:
    static constexpr u32 kMinSpinMarginNs = 50'000U;
    static constexpr u32 kDefaultSpinMarginNs = 150'000U;
    static constexpr u32 kMaxSpinMarginNs = 300'000U;

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
