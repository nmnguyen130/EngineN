#pragma once

#include "time/TimeTypes.h"

namespace en {

class TimePipeline;

// Compact 32-byte non-owning simulation step view referencing Cache Line 0 of TimePipeline.
// Value type; passed to 120 Hz simulation ticks with zero 64B array copy overhead.
// Invariant: when valid == true, baked and pipeline are non-null.
struct alignas(32) SimStepView {
    const BakedTickState* baked{nullptr};
    TimePipeline* pipeline{nullptr};
    u64 tickIndex{0};
    u32 catchupIndex{0};
    u16 substepTotal{1};
    u8 frozenMask{0};
    bool valid{false};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid; }
    [[nodiscard]] constexpr f32 Dt(TimeChannel ch = TimeChannel::World) const noexcept {
        const u8 idx = static_cast<u8>(ch) & 7U;
        if ((frozenMask & (1U << idx)) != 0 || !baked) return 0.0f;
        return baked->scaledDt[idx];
    }
    [[nodiscard]] constexpr f32 EffectiveScale(TimeChannel ch = TimeChannel::World) const noexcept {
        const u8 idx = static_cast<u8>(ch) & 7U;
        if ((frozenMask & (1U << idx)) != 0 || !baked) return 0.0f;
        return baked->effectiveScale[idx];
    }
    [[nodiscard]] constexpr bool IsFrozen(TimeChannel ch = TimeChannel::World) const noexcept {
        return (frozenMask & (1U << (static_cast<u8>(ch) & 7U))) != 0;
    }

    void RequestHitstop(TimeChannel ch, u32 ticks, f32 intensity = 0.0f) const noexcept;
    void RequestMaskHitstop(u8 channelMask, u32 ticks, f32 intensity = 0.0f) const noexcept;
    void RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) const noexcept;
};
static_assert(sizeof(SimStepView) == 32);

// Deterministic 120 Hz fixed-step rational accumulator and 8-channel time modulator.
// Places BakedTickState at Cache Line 0 (offset 0..63) for zero-offset L1 access.
// Invariant: FrozenMask() automatically freezes kGameplayChannelsMask when paused unless stepping.
class alignas(64) TimePipeline {
public:
    constexpr TimePipeline() noexcept { Reset(); }

    constexpr void Reset() noexcept {
        m_masterScale = 1.0f;
        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            m_baseScales[i] = 1.0f;
            m_slots[i] = ChannelModSlot{};
            m_bakedTick.effectiveScale[i] = 1.0f;
            m_bakedTick.scaledDt[i] = kFixedDtSec;
            m_parentChannel[i] = (i >= 1U && i <= 5U) ? 0U : 0xFFU;
        }
        m_accUnits = 0;
        m_lastWallNs = 0;
        m_telemetry = TimeTelemetry{};
        m_realDtSec = kFixedDtSec;
        m_catchupCursor = 0;
        m_stepAllowanceTicks = 0;
        m_activeHitstopMask = 0;
        m_activeRampMask = 0;
        m_frozenMask = 0;
        m_paused = false;
    }

    void BeginFrame() noexcept;
    void BeginFrameManual(u64 rawDeltaNs) noexcept;
    [[nodiscard]] SimStepView PollTick() noexcept;
    [[nodiscard]] RenderTimeView GetRenderView() const noexcept;

    void SetMasterScale(f32 scale) noexcept;
    [[nodiscard]] constexpr f32 MasterScale() const noexcept { return m_masterScale; }
    void SetParentChannel(TimeChannel child, u8 parentChannelIdx) noexcept;
    [[nodiscard]] constexpr u8 ParentChannel(TimeChannel ch) const noexcept {
        return m_parentChannel[static_cast<u8>(ch) & 7U];
    }

    void SetBaseScale(TimeChannel ch, f32 scale) noexcept;
    void RequestHitstop(TimeChannel ch, u32 ticks, f32 intensity = 0.0f) noexcept;
    void RequestMaskHitstop(u8 channelMask, u32 ticks, f32 intensity = 0.0f) noexcept;
    void RequestGameplayFreeze(u32 ticks, f32 intensity = 0.0f) noexcept {
        RequestMaskHitstop(kGameplayChannelsMask, ticks, intensity);
    }
    void RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) noexcept;

    constexpr void SetPaused(bool paused) noexcept { m_paused = paused; }
    [[nodiscard]] constexpr bool IsPaused() const noexcept { return m_paused; }
    void RequestStep(u32 ticks = 1) noexcept;
    constexpr void OnWindowRefocus() noexcept { m_lastWallNs = 0; }

    [[nodiscard]] constexpr u8 FrozenMask() const noexcept {
        return (m_paused && m_stepAllowanceTicks == 0)
                   ? static_cast<u8>(m_frozenMask | kGameplayChannelsMask)
                   : m_frozenMask;
    }
    [[nodiscard]] constexpr std::span<const f32, kMaxTimeChannels> EffectiveScales() const noexcept {
        return m_bakedTick.effectiveScale;
    }
    [[nodiscard]] constexpr const TimeTelemetry& Telemetry() const noexcept { return m_telemetry; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    [[nodiscard]] static u64 NowWallNs() noexcept;

private:
    void BakeChannel(u8 idx) noexcept;
    void RebakeAllChannels() noexcept;
    void AdvanceChannelModulationTick() noexcept;

    BakedTickState m_bakedTick{};
    std::array<f32, kMaxTimeChannels> m_baseScales{};
    std::array<ChannelModSlot, kMaxTimeChannels> m_slots{};
    std::array<u8, kMaxTimeChannels> m_parentChannel{};
    u64 m_accUnits{0};
    u64 m_lastWallNs{0};
    TimeTelemetry m_telemetry{};
    f32 m_masterScale{1.0f};
    f32 m_realDtSec{kFixedDtSec};
    u32 m_catchupCursor{0};
    u32 m_stepAllowanceTicks{0};
    u8 m_activeHitstopMask{0};
    u8 m_activeRampMask{0};
    u8 m_frozenMask{0};
    bool m_paused{false};
};

// Hybrid OS-sleep + sub-millisecond spin-lock frame pacer preventing 100% CPU burn when VSync is disabled.
class FramePacer {
public:
    static constexpr u64 kSpinMarginNs = 1'500'000ULL;

    constexpr explicit FramePacer(u32 targetFps = 120) noexcept { SetTargetFps(targetFps); }

    constexpr void SetTargetFps(u32 targetFps) noexcept {
        m_targetFps = targetFps;
        m_targetFrameNs = (targetFps > 0) ? (kNsPerSecond / targetFps) : 0ULL;
    }

    void BeginFrame() noexcept { m_frameStartNs = TimePipeline::NowWallNs(); }
    void EndFrameAndPace() noexcept;

    [[nodiscard]] constexpr u32 TargetFps() const noexcept { return m_targetFps; }
    [[nodiscard]] constexpr u64 TargetFrameNs() const noexcept { return m_targetFrameNs; }
    [[nodiscard]] constexpr u64 LastSleepNs() const noexcept { return m_lastSleepNs; }
    [[nodiscard]] constexpr u64 LastSpinNs() const noexcept { return m_lastSpinNs; }

private:
    u64 m_frameStartNs{0};
    u64 m_targetFrameNs{kNsPerSecond / 120ULL};
    u64 m_lastSleepNs{0};
    u64 m_lastSpinNs{0};
    u32 m_targetFps{120};
};

// Fixed-capacity deterministic tick-based timer wheel for gameplay cooldowns and scheduled triggers.
class TimerWheel {
public:
    static constexpr usize kMaxTimers = 64;
    struct Entry {
        StringId tag{};
        u32 remainingTicks{0};
        u32 periodTicks{0};
        bool repeating{false};
        bool active{false};
    };

    constexpr void Clear() noexcept { m_entries.fill(Entry{}); }

    constexpr bool Schedule(StringId tag, u32 delayTicks, bool repeating = false) noexcept {
        if (!tag.IsValid() || delayTicks == 0) return false;
        for (auto& e : m_entries) {
            if (!e.active || e.tag == tag) {
                e = Entry{tag, delayTicks, delayTicks, repeating, true};
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr u32 TickAndCollectFired(std::span<StringId> outFired) noexcept {
        u32 fired = 0;
        for (auto& e : m_entries) {
            if (!e.active) continue;
            if (e.remainingTicks > 0) --e.remainingTicks;
            if (e.remainingTicks == 0) {
                if (fired < outFired.size()) outFired[fired] = e.tag;
                ++fired;
                if (e.repeating) {
                    e.remainingTicks = e.periodTicks;
                } else {
                    e.active = false;
                }
            }
        }
        return fired;
    }

private:
    std::array<Entry, kMaxTimers> m_entries{};
};

// Data-oriented float property tweening manager with SmoothStep and EaseOutBack curves.
class TweenManager {
public:
    enum class Curve : u8 { Linear = 0, SmoothStep = 1, EaseOutBack = 2 };
    struct Slot {
        f32* target{nullptr};
        f32 startVal{0.0f};
        f32 endVal{0.0f};
        f32 elapsed{0.0f};
        f32 duration{1.0f};
        Curve curve{Curve::SmoothStep};
        bool active{false};
    };

    constexpr void Clear() noexcept { m_slots.fill(Slot{}); }

    constexpr bool StartTween(f32* target, f32 startVal, f32 endVal, f32 durationSec, Curve curve = Curve::SmoothStep) noexcept {
        if (!target || durationSec <= 0.0f) return false;
        for (Slot& s : m_slots) {
            if (!s.active || s.target == target) {
                s = Slot{target, startVal, endVal, 0.0f, durationSec, curve, true};
                *target = startVal;
                return true;
            }
        }
        return false;
    }

    constexpr void Step(f32 dt) noexcept {
        if (dt <= 0.0f) return;
        for (Slot& s : m_slots) {
            if (!s.active || !s.target) continue;
            s.elapsed += dt;
            const f32 rawT = std::clamp(s.elapsed / s.duration, 0.0f, 1.0f);
            f32 shapedT = rawT;
            if (s.curve == Curve::SmoothStep) shapedT = SmoothStep(0.0f, 1.0f, rawT);
            else if (s.curve == Curve::EaseOutBack) shapedT = EaseOutBack(rawT);
            *s.target = s.startVal + (s.endVal - s.startVal) * shapedT;
            if (rawT >= 1.0f) s.active = false;
        }
    }

private:
    std::array<Slot, 64> m_slots{};
};

} // namespace en
