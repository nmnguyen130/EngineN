#pragma once

#include "core/time/ChannelModulator.h"

namespace en::time {

/**
 * SimStepView: Scoped view passed to simulation systems during a fixed 120Hz tick.
 * Non-owning view into ChannelModulator; valid only when operator bool() returns true.
 * Invariant: m_modulator is non-null for any active tick yielded by TimePipeline::PollTick.
 */
class SimStepView {
public:
    constexpr SimStepView() noexcept = default;

    constexpr SimStepView(u64 tickIndex, u32 subStepIndex, ChannelModulator& modulator) noexcept
        : m_tickIndex(tickIndex)
        , m_subStepIndex(subStepIndex)
        , m_modulator(&modulator) {}

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return m_modulator != nullptr;
    }

    [[nodiscard]] constexpr f32 Dt(ChannelId auto ch = TimeChannel::World) const noexcept {
        return m_modulator->BakedTick().scaledDt[ToChannelIndex(ch)];
    }

    [[nodiscard]] constexpr f32 EffectiveScale(ChannelId auto ch = TimeChannel::World) const noexcept {
        return m_modulator->BakedTick().effectiveScale[ToChannelIndex(ch)];
    }

    [[nodiscard]] constexpr bool IsFrozen(ChannelId auto ch = TimeChannel::World) const noexcept {
        return m_modulator->IsFrozen(ch);
    }

    [[nodiscard]] constexpr u64 TickIndex() const noexcept { return m_tickIndex; }
    [[nodiscard]] constexpr u32 CatchupIndex() const noexcept { return m_subStepIndex; }

    constexpr void RequestHitstop(ChannelId auto ch, u32 ticks, f32 intensity = 0.0f) const noexcept {
        m_modulator->RequestHitstop(ch, ticks, intensity);
    }

    template <ChannelId... Channels>
        requires (sizeof...(Channels) >= 2)
    constexpr void RequestMultiHitstop(u32 ticks, f32 intensity, Channels... channels) const noexcept {
        m_modulator->RequestMultiHitstop(ticks, intensity, channels...);
    }

    constexpr void RequestTimeRamp(ChannelId auto ch, f32 targetScale, u32 holdTicks, u32 blendOutTicks) const noexcept {
        m_modulator->RequestTimeRamp(ch, targetScale, holdTicks, blendOutTicks);
    }

    constexpr void SetBaseScale(ChannelId auto ch, f32 scale) const noexcept {
        m_modulator->SetBaseScale(ch, scale);
    }

private:
    u64               m_tickIndex    = 0;
    u32               m_subStepIndex = 0;
    ChannelModulator* m_modulator    = nullptr;
};

/**
 * RenderTimeView: Read-only presentation snapshot of time and per-channel interpolation alpha.
 * Passed to renderer, camera shake, and particle systems at display refresh rate.
 * Guarantees frozen channels stay pinned to alpha 1.0f to prevent sub-frame jitter.
 */
struct RenderTimeView {
    BakedRenderState channelAlpha{};
    f32              rawAlpha  = 0.0f;
    f32              realDt    = 0.0f;
    u64              tickIndex = 0;

    [[nodiscard]] constexpr f32 Alpha(ChannelId auto ch = TimeChannel::World) const noexcept {
        return channelAlpha.alpha[ToChannelIndex(ch)];
    }
};

} // namespace en::time