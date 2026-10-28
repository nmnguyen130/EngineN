#pragma once

#include "time/TimeTypes.h"

#include <algorithm>
#include <bit>
#include <span>

namespace en {

class TimePipeline;

// Non-owning immutable snapshot of a single 120 Hz simulation tick.
struct alignas(32) SimStepView {
    const BakedTickState* baked{nullptr};
    TimePipeline*         pipeline{nullptr};
    u64                   tickIndex{0};
    u32                   catchupIndex{0};
    u16                   substepTotal{0};
    u8                    frozenMask{0};
    bool                  valid{false};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid; }

    [[nodiscard]] constexpr bool IsFrozen(TimeChannel ch = TimeChannel::World) const noexcept {
        return (frozenMask & ChannelBit(ch)) != 0U;
    }
    [[nodiscard]] constexpr f32 Dt(TimeChannel ch = TimeChannel::World) const noexcept {
        return (IsFrozen(ch) || !baked) ? 0.0f : baked->scaledDt[ChannelIdx(ch)];
    }
    [[nodiscard]] constexpr f32 Scale(TimeChannel ch = TimeChannel::World) const noexcept {
        return (IsFrozen(ch) || !baked) ? 0.0f : baked->effectiveScale[ChannelIdx(ch)];
    }

    // Requests are deferred until next tick (keeps this view immutable).
    constexpr void RequestHitstop(TimeChannel ch, u32 ticks) const noexcept;
    constexpr void RequestMaskHitstop(u8 mask, u32 ticks) const noexcept;
    constexpr void RequestTimeRamp(TimeChannel ch, f32 target, u32 hold, u32 blend) const noexcept;
};
static_assert(sizeof(SimStepView) == 32);

// Deterministic 120 Hz simulation clock with rational harmonic VSync snapping.
class alignas(64) TimePipeline {
public:
    static constexpr u64 kMaxFrameDeltaNs   = 250'000'000ULL; // 250 ms spiral guard
    static constexpr u64 kVsyncSnapMarginNs = 150'000ULL;
    static constexpr f32 kMaxScale          = 10.0f;

    // Valid divisors of 1440 Hz for common monitors.
    static constexpr u64 kValidMask =
        (1ULL << 3) | (1ULL << 4) | (1ULL << 6) | (1ULL << 8) | (1ULL << 9) |
        (1ULL << 10) | (1ULL << 12) | (1ULL << 16) | (1ULL << 18) | (1ULL << 20) |
        (1ULL << 24) | (1ULL << 30) | (1ULL << 36) | (1ULL << 48);

    constexpr TimePipeline() noexcept {
        m_baked.effectiveScale.fill(1.0f);
        m_baked.scaledDt.fill(kFixedDtSec);
    }

    constexpr void Reset() noexcept { *this = TimePipeline{}; }

    // Call once per rendered frame with measured wall delta (ns).
    constexpr void BeginFrame(u64 rawDeltaNs) noexcept {
        Flush();
        const u64 clamped = std::min(rawDeltaNs, kMaxFrameDeltaNs);
        m_realDt = static_cast<f32>(clamped) * 1e-9f;
        ++m_tele.totalFrames;
        m_tele.ticksLastFrame = 0U;

        if (!m_paused) {
            const u64 add = Resolve(clamped);
            const u64 maxAcc = static_cast<u64>(m_tele.maxCatchup) * kTickUnits;
            m_acc += add;
            if (m_acc > maxAcc) {
                const u64 dropped = (m_acc - maxAcc + kTickUnits - 1ULL) / kTickUnits;
                m_tele.droppedTicks += static_cast<u32>(std::min<u64>(dropped, 0xFFFFFFFFULL));
                m_acc = maxAcc;
            }
        }
        m_planned = static_cast<u16>(m_acc / kTickUnits);
    }

    // Next sim step, or empty when done for this frame.
    [[nodiscard]] constexpr SimStepView PollTick() noexcept {
        Flush();
        if (m_acc < kTickUnits || m_tele.ticksLastFrame >= m_tele.maxCatchup) return {};
        if (m_paused && m_stepAllow == 0U) return {};

        m_acc -= kTickUnits;
        const u32 idx = m_tele.ticksLastFrame++;
        ++m_tele.totalSimTicks;
        if (m_stepAllow > 0U) --m_stepAllow;

        m_active = true;
        return SimStepView{
            .baked        = &m_baked,
            .pipeline     = this,
            .tickIndex    = m_tele.totalSimTicks,
            .catchupIndex = idx,
            .substepTotal = std::max(m_planned, m_tele.ticksLastFrame),
            .frozenMask   = m_frozen,
            .valid        = true,
        };
    }

    [[nodiscard]] constexpr RenderTimeView GetRenderView() const noexcept {
        const f32 alpha = std::clamp(
            static_cast<f32>(m_acc) * (1.0f / static_cast<f32>(kTickUnits)), 0.0f, 1.0f);
        RenderTimeView rv{.tickIndex = m_tele.totalSimTicks, .rawAlpha = alpha, .realDt = m_realDt};
        const u8 fr = FrozenMask();
        for (u8 i = 0; i < kMaxTimeChannels; ++i)
            rv.channelAlpha[i] = (fr & (1U << i)) ? 1.0f : alpha;
        return rv;
    }

    // Time scale and channel modulation control.
    constexpr void SetMaxCatchup(u16 n) noexcept {
        m_tele.maxCatchup = std::clamp<u16>(n, 1U, 64U);
    }
    constexpr void SetMasterScale(f32 s) noexcept {
        Flush();
        m_master = Sanitize(s);
        Rebake(0xFFU);
    }
    [[nodiscard]] constexpr f32 MasterScale() const noexcept { return m_master; }

    constexpr void SetBaseScale(TimeChannel ch, f32 s) noexcept {
        Flush();
        m_slots[ChannelIdx(ch)].baseScale = Sanitize(s);
        Rebake(ChannelBit(ch));
    }
    [[nodiscard]] constexpr f32 BaseScale(TimeChannel ch) const noexcept {
        return m_slots[ChannelIdx(ch)].baseScale;
    }

    constexpr void RequestHitstop(TimeChannel ch, u32 ticks) noexcept {
        RequestMaskHitstop(ChannelBit(ch), ticks);
    }
    constexpr void RequestMaskHitstop(u8 mask, u32 ticks) noexcept {
        if (ticks == 0U || mask == 0U) return;
        const u16 c = static_cast<u16>(std::min<u32>(ticks, kMaxFreezeTicks));
        u8 dirty = 0U;
        for (u8 m = mask; m; m &= static_cast<u8>(m - 1U)) {
            const u8 i = static_cast<u8>(std::countr_zero(m));
            if (c >= m_slots[i].hitstop) {
                m_slots[i].hitstop = c;
                dirty |= static_cast<u8>(1U << i);
            }
        }
        m_hitMask |= dirty;
        if (m_active) m_pending |= dirty;
        else Rebake(dirty);
    }
    constexpr void RequestGameplayFreeze(u32 ticks) noexcept {
        RequestMaskHitstop(kGameplayMask, ticks);
    }

    constexpr void RequestTimeRamp(TimeChannel ch, f32 target, u32 hold, u32 blend) noexcept {
        const u8 i = ChannelIdx(ch);
        const u8 bit = ChannelBit(ch);
        if (hold == 0U && blend == 0U) {
            m_slots[i].ClearRamp();
            m_rampMask &= static_cast<u8>(~bit);
            if (m_active) m_pending |= bit;
            else Rebake(bit);
            return;
        }
        m_slots[i].rampTarget  = Sanitize(target);
        m_slots[i].rampHold    = static_cast<u16>(std::min<u32>(hold, 0xFFFFU));
        m_slots[i].rampBlend   = static_cast<u16>(std::min<u32>(blend, 0xFFFFU));
        m_slots[i].rampElapsed = 0U;
        m_rampMask |= bit;
        if (m_active) m_pending |= bit;
        else Rebake(bit);
    }

    constexpr void ClearChannelEffects(TimeChannel ch) noexcept {
        const u8 i = ChannelIdx(ch);
        const u8 bit = ChannelBit(ch);
        m_slots[i].Clear();
        m_hitMask  &= static_cast<u8>(~bit);
        m_rampMask &= static_cast<u8>(~bit);
        if (m_active) m_pending |= bit;
        else Rebake(bit);
    }

    constexpr void SetPaused(bool p) noexcept {
        Flush();
        if (m_paused != p) {
            m_paused = p;
            if (!p) m_stepAllow = 0U;
            Rebake(0xFFU);
        }
    }
    [[nodiscard]] constexpr bool IsPaused() const noexcept { return m_paused; }

    // Single-step while paused (editor / debug).
    constexpr void RequestStep(u32 ticks = 1U) noexcept {
        Flush();
        if (ticks == 0U) return;
        const u32 maxC = m_tele.maxCatchup;
        m_stepAllow = static_cast<u16>(std::min<u32>(m_stepAllow + std::min(ticks, maxC), maxC));
        m_acc = std::max(m_acc, static_cast<u64>(m_stepAllow) * kTickUnits);
        m_planned = static_cast<u16>(std::min<u64>(m_acc / kTickUnits, maxC));
        Rebake(0xFFU);
    }

    // Call on focus regain / un-minimize to avoid huge catch-up.
    constexpr void OnWindowRefocus() noexcept { m_smoothed = 0ULL; }

    [[nodiscard]] constexpr u8 FrozenMask() const noexcept {
        return (m_paused && m_stepAllow == 0U)
                   ? static_cast<u8>(m_frozen | kGameplayMask)
                   : m_frozen;
    }
    [[nodiscard]] constexpr std::span<const f32, kMaxTimeChannels> EffectiveScales() const noexcept {
        return m_baked.effectiveScale;
    }
    [[nodiscard]] constexpr u64 AccumulatorNs() const noexcept { return m_acc / kHarmonicHz; }
    [[nodiscard]] constexpr const TimeTelemetry& Telemetry() const noexcept { return m_tele; }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = Mix64(m_tele.totalSimTicks ^ m_acc);
        h = HashCombine64(h, static_cast<u64>(FrozenMask()) |
                             (static_cast<u64>(m_hitMask) << 8) |
                             (static_cast<u64>(m_rampMask) << 16));
        for (u8 i = 0; i < kMaxTimeChannels; ++i)
            h = HashCombine64(h, std::bit_cast<u32>(m_baked.scaledDt[i]));
        return h;
    }

private:
    friend struct SimStepView;

    constexpr void Flush() noexcept {
        if (!m_active) return;
        m_active = false;
        AdvanceMods();
        if (m_pending) {
            Rebake(m_pending);
            m_pending = 0U;
        }
        if (m_paused && m_stepAllow == 0U) Rebake(0xFFU);
    }

    [[nodiscard]] static constexpr f32 Sanitize(f32 s) noexcept {
        return (s >= 0.0f) ? std::clamp(s, 0.0f, kMaxScale) : 0.0f;
    }
    [[nodiscard]] static constexpr u64 AbsDiff(u64 a, u64 b) noexcept {
        return (a >= b) ? (a - b) : (b - a);
    }

    // Exact snap for common refresh rates; light VRR smoothing otherwise.
    [[nodiscard]] constexpr u64 Resolve(u64 ns) noexcept {
        const u64 units = ns * kHarmonicHz;
        const u64 k = (units + (kNsPerSecond >> 1)) / kNsPerSecond;
        if (k <= 48ULL && ((kValidMask >> k) & 1ULL)) {
            const u64 target = k * kNsPerSecond;
            if (AbsDiff(units, target) <= kVsyncSnapMarginNs * kHarmonicHz) {
                m_smoothed = target / kHarmonicHz;
                return target;
            }
        }
        if (m_smoothed && AbsDiff(ns, m_smoothed) * 5ULL <= m_smoothed) {
            m_smoothed = (m_smoothed * 3ULL + ns + 2ULL) >> 2;
            return m_smoothed * kHarmonicHz;
        }
        m_smoothed = ns;
        return units;
    }

    constexpr void BakeOne(u8 i) noexcept {
        const u8 bit = static_cast<u8>(1U << i);
        if (m_paused && m_stepAllow == 0U && i <= 5U) {
            m_frozen |= bit;
            m_baked.effectiveScale[i] = 0.0f;
            m_baked.scaledDt[i] = 0.0f;
            return;
        }
        const f32 parent = (i >= 1U && i <= 5U) ? m_baked.effectiveScale[0] : m_master;
        const f32 eff = std::clamp(
            parent * m_slots[i].LocalScale((m_rampMask & bit) != 0U), 0.0f, kMaxScale);
        m_baked.effectiveScale[i] = eff;
        m_baked.scaledDt[i] = eff * kFixedDtSec;
        if (eff <= 1e-6f) m_frozen |= bit;
        else m_frozen &= static_cast<u8>(~bit);
    }

    constexpr void Rebake(u8 dirty) noexcept {
        if (dirty & 1U) dirty |= kGameplayMask;
        for (u8 m = dirty; m; m &= static_cast<u8>(m - 1U))
            BakeOne(static_cast<u8>(std::countr_zero(m)));
    }

    constexpr void AdvanceMods() noexcept {
        const u8 active = static_cast<u8>(m_hitMask | m_rampMask);
        if (!active) return;
        for (u8 m = active; m; m &= static_cast<u8>(m - 1U)) {
            const u8 i = static_cast<u8>(std::countr_zero(m));
            const u8 bit = static_cast<u8>(1U << i);
            auto& s = m_slots[i];
            if (s.hitstop > 0U) {
                if (--s.hitstop == 0U) m_hitMask &= static_cast<u8>(~bit);
            } else if (m_rampMask & bit) {
                if (s.rampHold > 0U) {
                    if (--s.rampHold == 0U && s.rampBlend == 0U) {
                        s.Clear();
                        m_rampMask &= static_cast<u8>(~bit);
                    }
                } else if (++s.rampElapsed >= s.rampBlend) {
                    s.Clear();
                    m_rampMask &= static_cast<u8>(~bit);
                }
            }
        }
        Rebake(active);
    }

    BakedTickState                     m_baked{};
    std::array<ChannelModSlot, kMaxTimeChannels> m_slots{};

    u64            m_acc{0};
    TimeTelemetry  m_tele{};
    u64            m_smoothed{0};
    f32            m_master{1.0f};
    f32            m_realDt{kFixedDtSec};
    u16            m_stepAllow{0};
    u16            m_planned{0};
    u8             m_hitMask{0};
    u8             m_rampMask{0};
    u8             m_frozen{0};
    u8             m_pending{0};
    bool           m_paused{false};
    bool           m_active{false};
};
static_assert(sizeof(TimePipeline) == 256);

constexpr void SimStepView::RequestHitstop(TimeChannel ch, u32 ticks) const noexcept {
    if (pipeline) pipeline->RequestHitstop(ch, ticks);
}
constexpr void SimStepView::RequestMaskHitstop(u8 mask, u32 ticks) const noexcept {
    if (pipeline) pipeline->RequestMaskHitstop(mask, ticks);
}
constexpr void SimStepView::RequestTimeRamp(TimeChannel ch, f32 target, u32 hold, u32 blend) const noexcept {
    if (pipeline) pipeline->RequestTimeRamp(ch, target, hold, blend);
}

// Present pacing. Implementation in SdlClock.cpp.
class alignas(8) FramePacer {
public:
    static constexpr u32 kMinMarginNs     = 50'000U;
    static constexpr u32 kDefaultMarginNs = 150'000U;
    static constexpr u32 kMaxMarginNs     = 300'000U;

    constexpr explicit FramePacer(u32 fps = 120U) noexcept { SetTargetFps(fps); }

    constexpr void SetTargetFps(u32 fps) noexcept {
        m_fps = fps;
        m_targetNs = (fps > 0U) ? (kNsPerSecond / fps) : 0ULL;
    }

    void BeginFrame() noexcept;
    void EndFrameAndPace() noexcept;

    [[nodiscard]] constexpr u32 TargetFps() const noexcept { return m_fps; }
    [[nodiscard]] constexpr u64 TargetFrameNs() const noexcept { return m_targetNs; }
    [[nodiscard]] constexpr u32 LastSleepNs() const noexcept { return m_lastSleep; }
    [[nodiscard]] constexpr u32 LastSpinNs() const noexcept { return m_lastSpin; }
    [[nodiscard]] constexpr u32 SleepMarginNs() const noexcept { return m_margin; }

private:
    u64 m_start{0};
    u64 m_targetNs{kNsPerSecond / 120ULL};
    u32 m_lastSleep{0};
    u32 m_lastSpin{0};
    u32 m_margin{kDefaultMarginNs};
    u32 m_fps{120};
};
static_assert(sizeof(FramePacer) == 32);

} // namespace en
