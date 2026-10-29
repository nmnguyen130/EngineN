#pragma once

#include "time/TimeTypes.h"

namespace en {

// Deterministic 120 Hz fixed-timestep scheduler with 1440 Hz rational harmonic VSync snapping.
// Pure state machine with zero OS/SDL dependency; World channel scales cascade to gameplay channels.
class TimeSystem {
public:
    static constexpr u64 kValidHarmonicMask =
        (1ULL << 3) | (1ULL << 4) | (1ULL << 6) | (1ULL << 8) | (1ULL << 9) |
        (1ULL << 10) | (1ULL << 12) | (1ULL << 16) | (1ULL << 18) | (1ULL << 20) |
        (1ULL << 24) | (1ULL << 30) | (1ULL << 36) | (1ULL << 48);

    constexpr TimeSystem() noexcept { Reset(); }

    constexpr void Reset() noexcept {
        m_channels    = {};
        m_baked       = {};
        m_render      = {};
        m_stats       = {};
        m_accumUnits  = 0ULL;
        m_smoothedNs  = 0ULL;
        m_masterScale = 1.0f;
        m_stepBudget  = 0U;
        m_maxCatchup  = 8U;
        m_paused      = false;
        Rebake();
    }

    constexpr void BeginFrame(u64 rawFrameNs) noexcept {
        ++m_stats.totalFrames;
        m_stats.lastFrameTicks = 0U;

        u64 clampedNs = rawFrameNs;
        if (clampedNs > kMaxFrameNs) {
            clampedNs = kMaxFrameNs;
            ++m_stats.clampedFrames;
        }

        const f32 rawSec = static_cast<f32>(clampedNs) * 1e-9f;
        m_render.frameDt = rawSec;
        if (rawSec > 1e-6f) {
            m_render.smoothDt = (m_stats.totalFrames == 1ULL)
                ? rawSec
                : m_render.smoothDt + (rawSec - m_render.smoothDt) * 0.1f;
        }

        if (!m_paused) {
            m_accumUnits += ResolveDeltaUnits(clampedNs);
            const u64 maxUnits = static_cast<u64>(m_maxCatchup) * kTickUnits;
            if (m_accumUnits > maxUnits) {
                const u64 dropped = (m_accumUnits - maxUnits + kTickUnits - 1ULL) / kTickUnits;
                m_stats.droppedTicks += static_cast<u32>(dropped);
                m_accumUnits = maxUnits;
            }
        }
        SyncRenderStep();
    }

    [[nodiscard]] constexpr TickStep PollTick() noexcept {
        bool fromBudget = false;
        if (m_paused) {
            if (m_stepBudget == 0U) {
                SyncRenderStep();
                return {};
            }
            fromBudget = true;
            --m_stepBudget;
        } else {
            if (m_accumUnits < kTickUnits || m_stats.lastFrameTicks >= m_maxCatchup) {
                SyncRenderStep();
                return {};
            }
            m_accumUnits -= kTickUnits;
        }

        // m_baked is already up to date from the prior state transition or tick end.
        TickStep step = m_baked;
        step.tick    = static_cast<u32>(++m_stats.totalTicks);
        step.substep = m_stats.lastFrameTicks++;
        step.active  = 1U;

        bool changed = fromBudget && (m_stepBudget == 0U);
        for (ChannelMod& mod : m_channels) {
            changed |= mod.StepTick();
        }
        if (changed) Rebake();
        SyncRenderStep();
        return step;
    }

    constexpr void Hitstop(TimeChannel ch, u16 ticks) noexcept {
        MaskHitstop(ChannelBit(ch), ticks);
    }

    constexpr void MaskHitstop(u8 channelMask, u16 ticks) noexcept {
        const u8 mask = static_cast<u8>(channelMask & kGameplayMask);
        if (mask == 0U || ticks == 0U) return;
        for (u8 i = 0U; i < kGameplayChannels; ++i) {
            if ((mask & (1U << i)) != 0U && ticks > m_channels[i].hitstop) {
                m_channels[i].hitstop = ticks;
            }
        }
        Rebake();
        SyncRenderStep();
    }

    constexpr void ClearHitstop() noexcept {
        for (auto& ch : m_channels) ch.hitstop = 0U;
        Rebake();
        SyncRenderStep();
    }

    constexpr void Ramp(TimeChannel ch, f32 targetScale, u16 holdTicks, u16 recoverTicks) noexcept {
        const u8 i = static_cast<u8>(ch);
        if (i >= kGameplayChannels) return;
        ChannelMod& s = m_channels[i];
        if (holdTicks == 0U && recoverTicks == 0U) {
            s.ClearRamp();
        } else {
            s.rampTarget  = std::clamp(targetScale, 0.0f, kMaxTimeScale);
            s.rampHold    = holdTicks;
            s.rampRecover = recoverTicks;
            s.rampElapsed = 0U;
        }
        Rebake();
        SyncRenderStep();
    }

    constexpr void SetScale(TimeChannel ch, f32 scale) noexcept {
        const u8 i = static_cast<u8>(ch);
        if (i >= kGameplayChannels) return;
        m_channels[i].baseScale = std::clamp(scale, 0.0f, kMaxTimeScale);
        Rebake();
        SyncRenderStep();
    }

    constexpr void SetMasterScale(f32 scale) noexcept {
        m_masterScale = std::clamp(scale, 0.0f, kMaxTimeScale);
        Rebake();
        SyncRenderStep();
    }

    constexpr void SetPaused(bool paused) noexcept {
        if (m_paused != paused) {
            m_paused = paused;
            if (!paused) m_stepBudget = 0U;
            Rebake();
            SyncRenderStep();
        }
    }

    constexpr void Step(u16 ticks = 1U) noexcept {
        if (m_paused && ticks > 0U) {
            m_stepBudget = static_cast<u16>(std::min<u32>(m_stepBudget + ticks, m_maxCatchup));
            Rebake();
            SyncRenderStep();
        }
    }

    constexpr void OnWindowRefocus() noexcept { m_smoothedNs = 0ULL; }

    [[nodiscard]] constexpr bool IsPaused() const noexcept { return m_paused; }
    [[nodiscard]] constexpr u64 AccumulatorNs() const noexcept { return m_accumUnits / kHarmonicHz; }
    [[nodiscard]] constexpr const RenderStep& RenderView() const noexcept { return m_render; }
    [[nodiscard]] constexpr const TimeStats& Stats() const noexcept { return m_stats; }

private:
    [[nodiscard]] static constexpr u64 AbsDiff(u64 a, u64 b) noexcept {
        return (a >= b) ? (a - b) : (b - a);
    }

    [[nodiscard]] constexpr u64 ResolveDeltaUnits(u64 ns) noexcept {
        const u64 units = ns * kHarmonicHz;
        const u64 k = (units + (kNsPerSec >> 1U)) / kNsPerSec;
        const bool isHarmonic = (k > 0ULL) &&
            (((k % kHarmonicSubdiv) == 0ULL) ||
             (k <= 48ULL && ((kValidHarmonicMask >> k) & 1ULL) != 0ULL));
        if (isHarmonic) {
            const u64 target = k * kNsPerSec;
            if (AbsDiff(units, target) <= kHarmonicSnapNs * kHarmonicHz) {
                m_smoothedNs = target / kHarmonicHz;
                return target;
            }
        }
        if (m_smoothedNs != 0ULL && AbsDiff(ns, m_smoothedNs) * 5ULL <= m_smoothedNs) {
            m_smoothedNs = (m_smoothedNs * 3ULL + ns + 2ULL) >> 2U;
            return m_smoothedNs * kHarmonicHz;
        }
        m_smoothedNs = ns;
        return units;
    }

    constexpr void Rebake() noexcept {
        const bool worldPaused = m_paused && (m_stepBudget == 0U);
        const f32 worldScale = worldPaused
            ? 0.0f
            : std::clamp(m_masterScale * m_channels[0].LocalScale(), 0.0f, kMaxTimeScale);

        u8 frozen = 0U;
        for (u8 i = 0U; i < kGameplayChannels; ++i) {
            const f32 eff = (i == 0U)
                ? worldScale
                : (worldPaused ? 0.0f : std::clamp(worldScale * m_channels[i].LocalScale(), 0.0f, kMaxTimeScale));
            m_baked.dt[i] = eff * kFixedDt;
            if (eff <= 1e-6f) frozen |= static_cast<u8>(1U << i);
        }
        m_baked.frozenMask = frozen;
    }

    constexpr void SyncRenderStep() noexcept {
        m_render.alpha = std::clamp(
            static_cast<f32>(m_accumUnits) * (1.0f / static_cast<f32>(kTickUnits)), 0.0f, 1.0f);
        m_render.tick       = static_cast<u32>(m_stats.totalTicks);
        m_render.frozenMask = m_baked.frozenMask;
        m_render.paused     = (m_paused && m_stepBudget == 0U) ? 1U : 0U;
        m_render.frame      = static_cast<u16>(m_stats.totalFrames & 0xFFFFULL);
    }

    u64        m_accumUnits{0};
    u64        m_smoothedNs{0};
    TimeStats  m_stats{};
    std::array<ChannelMod, kGameplayChannels> m_channels{};
    TickStep   m_baked{};
    RenderStep m_render{};
    f32        m_masterScale{1.0f};
    u16        m_stepBudget{0};
    u8         m_maxCatchup{8};
    bool       m_paused{false};
};
static_assert(sizeof(TimeSystem) == 160);

} // namespace en
