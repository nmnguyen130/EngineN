#pragma once

#include "time/TimeTypes.h"

namespace en {

// Deterministic 120 Hz fixed-timestep scheduler with 1440 Hz rational harmonic VSync snapping.
// Pure state machine with zero OS/SDL dependency; World channel scales cascade to gameplay channels.
class TimeSystem {
public:
    // Bitmask of exact integer divisors k of 1440 Hz (30, 36, 40, 48, 60, 72, 80, 90, 120, 144, 160, 180, 240, 360, 480 Hz).
    static constexpr u64 kValidHarmonicMask =
        (1ULL << 3) | (1ULL << 4) | (1ULL << 6) | (1ULL << 8) | (1ULL << 9) |
        (1ULL << 10) | (1ULL << 12) | (1ULL << 16) | (1ULL << 18) | (1ULL << 20) |
        (1ULL << 24) | (1ULL << 30) | (1ULL << 36) | (1ULL << 48);

    constexpr TimeSystem() noexcept { Reset(); }

    constexpr void Reset() noexcept {
        m_channels = {};
        m_baked = {};
        m_render = {};
        m_stats = {};
        m_accumUnits = 0ULL;
        m_smoothedNs = 0ULL;
        m_masterScale = 1.0f;
        m_stepBudget = 0U;
        m_maxCatchup = 8U;
        m_hitMask = 0U;
        m_rampMask = 0U;
        m_paused = false;
        Rebake(kGameplayMask);
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
                return TickStep{};
            }
            fromBudget = true;
        } else {
            if (m_accumUnits < kTickUnits || m_stats.lastFrameTicks >= m_maxCatchup) {
                SyncRenderStep();
                return TickStep{};
            }
            m_accumUnits -= kTickUnits;
        }

        Rebake(kGameplayMask);
        if (fromBudget) --m_stepBudget;

        const u16 sub = m_stats.lastFrameTicks++;
        const u32 tickIdx = static_cast<u32>(++m_stats.totalTicks);

        TickStep step = m_baked;
        step.tick = tickIdx;
        step.substep = sub;
        step.active = 1U;

        AdvanceMods();
        SyncRenderStep();
        return step;
    }

    constexpr void Hitstop(TimeChannel ch, u16 ticks) noexcept {
        MaskHitstop(ChannelBit(ch), ticks);
    }

    constexpr void MaskHitstop(u8 channelMask, u16 ticks) noexcept {
        const u8 mask = static_cast<u8>(channelMask & kGameplayMask);
        if (mask == 0U || ticks == 0U) return;
        u8 dirty = 0U;
        for (u8 m = mask; m != 0U; m &= static_cast<u8>(m - 1U)) {
            const u8 i = static_cast<u8>(std::countr_zero(m));
            if (ticks > m_channels[i].hitstop) {
                m_channels[i].hitstop = ticks;
                dirty |= static_cast<u8>(1U << i);
            }
        }
        m_hitMask |= dirty;
        if (dirty != 0U) {
            Rebake(dirty);
            SyncRenderStep();
        }
    }

    constexpr void ClearHitstop() noexcept {
        for (auto& ch : m_channels) ch.hitstop = 0U;
        m_hitMask = 0U;
        Rebake(kGameplayMask);
        SyncRenderStep();
    }

    constexpr void Ramp(TimeChannel ch, f32 targetScale, u16 holdTicks, u16 recoverTicks) noexcept {
        const u8 bit = ChannelBit(ch);
        if (bit == 0U) return;
        const u8 i = static_cast<u8>(ch);
        ChannelMod& s = m_channels[i];
        if (holdTicks == 0U && recoverTicks == 0U) {
            s.ClearRamp();
            m_rampMask &= static_cast<u8>(~bit);
        } else {
            s.rampTarget = std::clamp(targetScale, 0.0f, kMaxTimeScale);
            s.rampHold = holdTicks;
            s.rampRecover = recoverTicks;
            s.rampElapsed = 0U;
            m_rampMask |= bit;
        }
        Rebake(bit);
        SyncRenderStep();
    }

    constexpr void SetScale(TimeChannel ch, f32 scale) noexcept {
        const u8 bit = ChannelBit(ch);
        if (bit == 0U) return;
        m_channels[static_cast<u8>(ch)].baseScale = std::clamp(scale, 0.0f, kMaxTimeScale);
        Rebake(bit);
        SyncRenderStep();
    }

    constexpr void SetMasterScale(f32 scale) noexcept {
        m_masterScale = std::clamp(scale, 0.0f, kMaxTimeScale);
        Rebake(kGameplayMask);
        SyncRenderStep();
    }

    constexpr void SetPaused(bool paused) noexcept {
        if (m_paused != paused) {
            m_paused = paused;
            if (!paused) m_stepBudget = 0U;
            Rebake(kGameplayMask);
            SyncRenderStep();
        }
    }

    constexpr void Step(u16 ticks = 1U) noexcept {
        if (m_paused && ticks > 0U) {
            m_stepBudget = static_cast<u16>(std::min<u32>(m_stepBudget + ticks, m_maxCatchup));
            Rebake(kGameplayMask);
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

    // O(1) bitmask rational snap for 1440 Hz divisors with VRR EMA fallback.
    [[nodiscard]] constexpr u64 ResolveDeltaUnits(u64 ns) noexcept {
        const u64 units = ns * kHarmonicHz;
        const u64 k = (units + (kNsPerSec >> 1U)) / kNsPerSec;
        if (k <= 48ULL && ((kValidHarmonicMask >> k) & 1ULL) != 0ULL) {
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

    constexpr void Rebake(u8 dirtyMask) noexcept {
        if ((dirtyMask & 1U) != 0U) dirtyMask = kGameplayMask;
        const bool worldPaused = m_paused && (m_stepBudget == 0U);
        const f32 worldScale = worldPaused
            ? 0.0f
            : std::clamp(m_masterScale * m_channels[0].LocalScale((m_rampMask & 1U) != 0U),
                         0.0f, kMaxTimeScale);

        for (u8 m = static_cast<u8>(dirtyMask & kGameplayMask); m != 0U; m &= static_cast<u8>(m - 1U)) {
            const u8 i = static_cast<u8>(std::countr_zero(m));
            const u8 bit = static_cast<u8>(1U << i);
            const f32 eff = (i == 0U)
                ? worldScale
                : (worldPaused
                       ? 0.0f
                       : std::clamp(worldScale * m_channels[i].LocalScale((m_rampMask & bit) != 0U),
                                    0.0f, kMaxTimeScale));
            m_baked.dt[i] = eff * kFixedDt;
            if (eff <= 1e-6f) {
                m_baked.frozenMask |= bit;
            } else {
                m_baked.frozenMask &= static_cast<u8>(~bit);
            }
        }
    }

    constexpr void AdvanceMods() noexcept {
        const u8 active = static_cast<u8>(m_hitMask | m_rampMask);
        if (active == 0U) return;
        for (u8 m = active; m != 0U; m &= static_cast<u8>(m - 1U)) {
            const u8 i = static_cast<u8>(std::countr_zero(m));
            const u8 bit = static_cast<u8>(1U << i);
            ChannelMod& s = m_channels[i];
            if (s.hitstop > 0U) {
                if (--s.hitstop == 0U) m_hitMask &= static_cast<u8>(~bit);
            } else if ((m_rampMask & bit) != 0U) {
                if (s.rampHold > 0U) {
                    if (--s.rampHold == 0U && s.rampRecover == 0U) {
                        s.ClearRamp();
                        m_rampMask &= static_cast<u8>(~bit);
                    }
                } else if (++s.rampElapsed >= s.rampRecover) {
                    s.ClearRamp();
                    m_rampMask &= static_cast<u8>(~bit);
                }
            }
        }
        Rebake(active);
    }

    constexpr void SyncRenderStep() noexcept {
        m_render.alpha = std::clamp(
            static_cast<f32>(m_accumUnits) * (1.0f / static_cast<f32>(kTickUnits)), 0.0f, 1.0f);
        m_render.tick = static_cast<u32>(m_stats.totalTicks);
        m_render.frozenMask = m_baked.frozenMask;
        m_render.paused = (m_paused && m_stepBudget == 0U) ? 1U : 0U;
        m_render.frame = static_cast<u16>(m_stats.totalFrames & 0xFFFFULL);
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
    u8         m_hitMask{0};
    u8         m_rampMask{0};
    bool       m_paused{false};
};
static_assert(sizeof(TimeSystem) == 160);

} // namespace en
