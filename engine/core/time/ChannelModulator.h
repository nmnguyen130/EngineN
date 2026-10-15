#pragma once

#include "core/time/TimeTypes.h"

#include <algorithm>

namespace en::time {

/**
 * ChannelModulator: Evaluates per-channel time scales, hitstop freezes, and slow-motion ramps.
 * Precomputes per-tick channel deltas into a single 64-byte L1 cache line.
 * Owned by TimePipeline; all effective channel scales are non-negative and clamped to [0.0f, 8.0f].
 */
class ChannelModulator {
public:
    struct HitstopState {
        u32 remainingTicks = 0;
        f32 intensity      = 0.0f;
    };

    struct RampState {
        f32 targetScale  = 1.0f;
        u32 holdTicks    = 0;
        u32 blendTicks   = 0;
        u32 elapsedBlend = 0;
    };

    constexpr ChannelModulator() noexcept { Reset(); }

    constexpr void Reset() noexcept {
        m_baseScales.fill(1.0f);
        m_hitstops.fill({});
        m_ramps.fill({});
        BakeTickState();
    }

    constexpr void SetBaseScale(ChannelId auto ch, f32 scale) noexcept {
        m_baseScales[ToChannelIndex(ch)] = std::clamp(scale, 0.0f, 8.0f);
    }

    [[nodiscard]] constexpr f32 GetBaseScale(ChannelId auto ch) const noexcept {
        return m_baseScales[ToChannelIndex(ch)];
    }

    constexpr void RequestHitstop(ChannelId auto ch, u32 ticks, f32 intensity = 0.0f) noexcept {
        if (ticks == 0) {
            return;
        }
        const u8 idx      = ToChannelIndex(ch);
        const f32 clamped = std::clamp(intensity, 0.0f, 1.0f);
        HitstopState& hs  = m_hitstops[idx];

        // Longer duration wins; on ties, harder freeze (lower intensity) takes precedence
        // so weak ambient shakes never overwrite heavy combat impacts.
        if (ticks > hs.remainingTicks || (ticks == hs.remainingTicks && clamped < hs.intensity)) {
            hs.remainingTicks = ticks;
            hs.intensity      = clamped;
        }
    }

    template <ChannelId... Channels>
        requires (sizeof...(Channels) >= 2)
    constexpr void RequestMultiHitstop(u32 ticks, f32 intensity, Channels... channels) noexcept {
        (RequestHitstop(channels, ticks, intensity), ...);
    }

    constexpr void RequestTimeRamp(ChannelId auto ch, f32 targetScale, u32 holdTicks, u32 blendOutTicks) noexcept {
        m_ramps[ToChannelIndex(ch)] = RampState{
            .targetScale  = std::clamp(targetScale, 0.01f, 4.0f),
            .holdTicks    = holdTicks,
            .blendTicks   = blendOutTicks,
            .elapsedBlend = 0
        };
    }

    constexpr void BakeTickState() noexcept {
        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            const f32 hsMod   = (m_hitstops[i].remainingTicks > 0) ? m_hitstops[i].intensity : 1.0f;
            const f32 rampMod = EvaluateRamp(m_ramps[i]);
            const f32 total   = m_baseScales[i] * rampMod * hsMod;

            m_bakedTick.effectiveScale[i] = total;
            m_bakedTick.scaledDt[i]       = kFixedDtSec * total;
        }
    }

    constexpr void AdvanceTick() noexcept {
        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            if (m_hitstops[i].remainingTicks > 0) {
                if (--m_hitstops[i].remainingTicks == 0) {
                    m_hitstops[i].intensity = 0.0f;
                }
            }
            // Pause time-dilation ramp progression while the channel is hard-frozen by hitstop.
            if (!IsFrozen(i)) {
                AdvanceRamp(m_ramps[i]);
            }
        }
    }

    [[nodiscard]] constexpr BakedRenderState BakeRenderState(f32 rawAlpha) const noexcept {
        BakedRenderState out{};
        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            // Lock interpolation alpha to 1.0f during hard hitstop so frozen sprites do not
            // visually drift between their pre-freeze and freeze-frame transforms.
            out.alpha[i] = IsFrozen(i) ? 1.0f : rawAlpha;
        }
        return out;
    }

    [[nodiscard]] constexpr const BakedTickState& BakedTick() const noexcept { return m_bakedTick; }

    [[nodiscard]] constexpr bool IsFrozen(ChannelId auto ch) const noexcept {
        const u8 idx = ToChannelIndex(ch);
        return (m_hitstops[idx].remainingTicks > 0) && (m_hitstops[idx].intensity <= 0.0f);
    }

    [[nodiscard]] constexpr u32 RemainingHitstopTicks(ChannelId auto ch) const noexcept {
        return m_hitstops[ToChannelIndex(ch)].remainingTicks;
    }

private:
    [[nodiscard]] static constexpr f32 EvaluateRamp(const RampState& r) noexcept {
        if (r.holdTicks > 0) {
            return r.targetScale;
        }
        if (r.blendTicks > 0 && r.elapsedBlend < r.blendTicks) {
            const f32 t       = static_cast<f32>(r.elapsedBlend) / static_cast<f32>(r.blendTicks);
            const f32 smoothT = t * t * (3.0f - 2.0f * t);
            return r.targetScale + (1.0f - r.targetScale) * smoothT;
        }
        return 1.0f;
    }

    static constexpr void AdvanceRamp(RampState& r) noexcept {
        if (r.holdTicks > 0) {
            --r.holdTicks;
        } else if (r.blendTicks > 0 && r.elapsedBlend < r.blendTicks) {
            if (++r.elapsedBlend >= r.blendTicks) {
                r = RampState{};
            }
        }
    }

    BakedTickState                             m_bakedTick{};
    std::array<f32, kMaxTimeChannels>          m_baseScales{};
    std::array<HitstopState, kMaxTimeChannels> m_hitstops{};
    std::array<RampState, kMaxTimeChannels>    m_ramps{};
};

} // namespace en::time