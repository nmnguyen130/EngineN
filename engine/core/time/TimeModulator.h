#pragma once

#include "core/time/TimeTypes.h"

#include <algorithm>
#include <bit>
#include <span>

namespace en::time {

// Evaluates per-channel time scales, hitstop freezes, and smoothstep slow-motion ramps.
// Owned by TimePipeline; hot BakedTickState sits at offset 0 (Cache Line 0) for O(1) simulation reads.
// Invariant: m_bakedTick and m_frozenMask are always synchronized with channel state in [0.0f, 8.0f].
class alignas(64) TimeModulator {
public:
    // Compact 16-byte per-channel modulation slot (4 channels per 64B cache line, zero padding).
    struct alignas(16) TimeModSlot {
        f32 hitstopIntensity = 0.0f;
        f32 rampTargetScale  = 1.0f;
        u16 hitstopTicks     = 0;
        u16 rampHoldTicks    = 0;
        u16 rampBlendTicks   = 0;
        u16 rampElapsedTicks = 0;
    };
    static_assert(sizeof(TimeModSlot) == 16);

    constexpr TimeModulator() noexcept = default;

    constexpr void Reset() noexcept {
        m_bakedTick = {};
        m_baseScales.fill(1.0f);
        m_slots.fill({});
        m_frozenMask        = 0;
        m_activeHitstopMask = 0;
        m_activeRampMask    = 0;
    }

    constexpr void SetBaseScale(ChannelId auto ch, f32 scale) noexcept {
        const u8 idx        = ToChannelIndex(ch);
        const f32 sanitized = (scale >= 0.0f) ? std::min(scale, 8.0f) : 0.0f;
        if (m_baseScales[idx] != sanitized) {
            m_baseScales[idx] = sanitized;
            BakeChannel(idx);
        }
    }

    [[nodiscard]] constexpr f32 GetBaseScale(ChannelId auto ch) const noexcept {
        return m_baseScales[ToChannelIndex(ch)];
    }

    [[nodiscard]] constexpr f32 Dt(ChannelId auto ch = TimeChannel::World) const noexcept {
        return m_bakedTick.scaledDt[ToChannelIndex(ch)];
    }

    [[nodiscard]] constexpr f32 EffectiveScale(ChannelId auto ch = TimeChannel::World) const noexcept {
        return m_bakedTick.effectiveScale[ToChannelIndex(ch)];
    }

    constexpr void RequestHitstop(ChannelId auto ch, u32 ticks, f32 intensity = 0.0f) noexcept {
        if (ticks == 0) {
            return;
        }
        const u8  idx     = ToChannelIndex(ch);
        const u16 clamped = static_cast<u16>(std::min<u32>(ticks, 0xFFFFu));
        const f32 intClmp = (intensity >= 0.0f) ? std::min(intensity, 1.0f) : 0.0f;
        auto&     slot    = m_slots[idx];

        // Longer duration wins; on ties, harder freeze (lower intensity) takes precedence.
        if (clamped > slot.hitstopTicks || (clamped == slot.hitstopTicks && intClmp < slot.hitstopIntensity)) {
            slot.hitstopTicks     = clamped;
            slot.hitstopIntensity = intClmp;
            m_activeHitstopMask  |= static_cast<u8>(1u << idx);
            BakeChannel(idx);
        }
    }

    template <ChannelId... Channels>
        requires (sizeof...(Channels) >= 2)
    constexpr void RequestMultiHitstop(u32 ticks, f32 intensity, Channels... channels) noexcept {
        RequestMaskHitstop(MakeChannelMask(channels...), ticks, intensity);
    }

    constexpr void RequestMaskHitstop(u8 channelMask, u32 ticks, f32 intensity = 0.0f) noexcept {
        if (ticks == 0 || channelMask == 0) {
            return;
        }
        const u16 clamped = static_cast<u16>(std::min<u32>(ticks, 0xFFFFu));
        const f32 intClmp = (intensity >= 0.0f) ? std::min(intensity, 1.0f) : 0.0f;
        for (u32 m = channelMask; m != 0; m &= (m - 1u)) {
            const u8 idx  = static_cast<u8>(std::countr_zero(m));
            auto&    slot = m_slots[idx];
            if (clamped > slot.hitstopTicks || (clamped == slot.hitstopTicks && intClmp < slot.hitstopIntensity)) {
                slot.hitstopTicks     = clamped;
                slot.hitstopIntensity = intClmp;
                m_activeHitstopMask  |= static_cast<u8>(1u << idx);
                BakeChannel(idx);
            }
        }
    }

    constexpr void RequestTimeRamp(ChannelId auto ch, f32 targetScale, u32 holdTicks, u32 blendOutTicks) noexcept {
        const u8   idx  = ToChannelIndex(ch);
        const u8   bit  = static_cast<u8>(1u << idx);
        auto&      slot = m_slots[idx];
        const u16  hold = static_cast<u16>(std::min<u32>(holdTicks, 0xFFFFu));
        const u16  blnd = static_cast<u16>(std::min<u32>(blendOutTicks, 0xFFFFu));

        if (hold == 0 && blnd == 0) {
            slot.rampTargetScale  = 1.0f;
            slot.rampHoldTicks    = 0;
            slot.rampBlendTicks   = 0;
            slot.rampElapsedTicks = 0;
            m_activeRampMask     &= static_cast<u8>(~bit);
            BakeChannel(idx);
            return;
        }
        slot.rampTargetScale  = (targetScale >= 0.01f) ? std::min(targetScale, 4.0f) : 0.01f;
        slot.rampHoldTicks    = hold;
        slot.rampBlendTicks   = blnd;
        slot.rampElapsedTicks = 0;
        m_activeRampMask     |= bit;
        BakeChannel(idx);
    }

    constexpr void ClearChannelEffects(ChannelId auto ch) noexcept {
        const u8 idx = ToChannelIndex(ch);
        const u8 bit = static_cast<u8>(1u << idx);
        m_slots[idx] = {};
        m_activeHitstopMask &= static_cast<u8>(~bit);
        m_activeRampMask    &= static_cast<u8>(~bit);
        BakeChannel(idx);
    }

    constexpr void AdvanceTick() noexcept {
        u32 mask = static_cast<u32>(m_activeHitstopMask | m_activeRampMask);
        if (mask == 0) [[likely]] {
            return;
        }
        for (; mask != 0; mask &= (mask - 1u)) {
            const u8   idx       = static_cast<u8>(std::countr_zero(mask));
            const u8   bit       = static_cast<u8>(1u << idx);
            const bool wasFrozen = (m_frozenMask & bit) != 0;
            auto&      slot      = m_slots[idx];

            if ((m_activeHitstopMask & bit) != 0) {
                if (--slot.hitstopTicks == 0) {
                    slot.hitstopIntensity = 0.0f;
                    m_activeHitstopMask  &= static_cast<u8>(~bit);
                }
            }
            // Pause time-dilation ramp progression across the entire tick a channel was hard-frozen.
            if (!wasFrozen && ((m_activeRampMask & bit) != 0)) {
                if (!AdvanceRamp(slot)) {
                    m_activeRampMask &= static_cast<u8>(~bit);
                }
            }
            BakeChannel(idx);
        }
    }

    [[nodiscard]] constexpr BakedRenderState BakeRenderState(f32 rawAlpha) const noexcept {
        BakedRenderState out{};
        out.alpha.fill(rawAlpha);
        for (u32 m = m_frozenMask; m != 0; m &= (m - 1u)) {
            out.alpha[std::countr_zero(m)] = 1.0f;
        }
        return out;
    }

    [[nodiscard]] constexpr const BakedTickState& BakedTick() const noexcept { return m_bakedTick; }

    [[nodiscard]] constexpr std::span<const f32, kMaxTimeChannels> EffectiveScales() const noexcept {
        return m_bakedTick.effectiveScale;
    }

    [[nodiscard]] constexpr std::span<const f32, kMaxTimeChannels> ScaledDts() const noexcept {
        return m_bakedTick.scaledDt;
    }

    [[nodiscard]] constexpr u8 FrozenMask() const noexcept { return m_frozenMask; }
    [[nodiscard]] constexpr u8 ActiveHitstopMask() const noexcept { return m_activeHitstopMask; }
    [[nodiscard]] constexpr u8 ActiveRampMask() const noexcept { return m_activeRampMask; }

    [[nodiscard]] constexpr bool IsFrozen(ChannelId auto ch) const noexcept {
        return (m_frozenMask & static_cast<u8>(1u << ToChannelIndex(ch))) != 0;
    }

    [[nodiscard]] constexpr u32 RemainingHitstopTicks(ChannelId auto ch) const noexcept {
        return m_slots[ToChannelIndex(ch)].hitstopTicks;
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = 0x9E3779B97F4A7C15ULL;
        auto mix = [&h](u64 val) constexpr noexcept {
            h ^= val + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        };
        mix(static_cast<u64>(m_frozenMask) |
            (static_cast<u64>(m_activeHitstopMask) << 8) |
            (static_cast<u64>(m_activeRampMask) << 16));
        for (u8 i = 0; i < kMaxTimeChannels; ++i) {
            const auto& s = m_slots[i];
            mix(std::bit_cast<u32>(m_bakedTick.scaledDt[i]));
            mix(std::bit_cast<u32>(m_bakedTick.effectiveScale[i]));
            mix(std::bit_cast<u32>(m_baseScales[i]));
            mix(std::bit_cast<u32>(s.hitstopIntensity));
            mix(std::bit_cast<u32>(s.rampTargetScale));
            mix(static_cast<u64>(s.hitstopTicks) |
                (static_cast<u64>(s.rampHoldTicks) << 16) |
                (static_cast<u64>(s.rampBlendTicks) << 32) |
                (static_cast<u64>(s.rampElapsedTicks) << 48));
        }
        return h;
    }

private:
    constexpr void BakeChannel(u8 idx) noexcept {
        const u8    bit     = static_cast<u8>(1u << idx);
        const auto& slot    = m_slots[idx];
        const bool  hasHs   = (m_activeHitstopMask & bit) != 0;
        const f32   hsMod   = hasHs ? slot.hitstopIntensity : 1.0f;
        const f32   rampMod = ((m_activeRampMask & bit) != 0) ? EvaluateRamp(slot) : 1.0f;
        const f32   total   = m_baseScales[idx] * rampMod * hsMod;

        m_bakedTick.effectiveScale[idx] = total;
        m_bakedTick.scaledDt[idx]       = kFixedDtSec * total;

        if (hasHs && slot.hitstopIntensity <= 0.0f) {
            m_frozenMask |= bit;
        } else {
            m_frozenMask &= static_cast<u8>(~bit);
        }
    }

    [[nodiscard]] static constexpr f32 EvaluateRamp(const TimeModSlot& s) noexcept {
        if (s.rampHoldTicks > 0) {
            return s.rampTargetScale;
        }
        if (s.rampBlendTicks > 0 && s.rampElapsedTicks < s.rampBlendTicks) {
            const f32 t       = static_cast<f32>(s.rampElapsedTicks) / static_cast<f32>(s.rampBlendTicks);
            const f32 smoothT = t * t * (3.0f - 2.0f * t);
            return s.rampTargetScale + (1.0f - s.rampTargetScale) * smoothT;
        }
        return 1.0f;
    }

    [[nodiscard]] static constexpr bool AdvanceRamp(TimeModSlot& s) noexcept {
        if (s.rampHoldTicks > 0) {
            --s.rampHoldTicks;
            return (s.rampHoldTicks > 0) || (s.rampBlendTicks > 0);
        }
        if (s.rampBlendTicks > 0 && s.rampElapsedTicks < s.rampBlendTicks) {
            if (++s.rampElapsedTicks < s.rampBlendTicks) {
                return true;
            }
        }
        s.rampTargetScale  = 1.0f;
        s.rampHoldTicks    = 0;
        s.rampBlendTicks   = 0;
        s.rampElapsedTicks = 0;
        return false;
    }

    // Cache Line 0 (64B): Hot per-tick lookup arrays read by every simulation subsystem.
    BakedTickState m_bakedTick{};

    // Cache Line 1 (32B): Per-channel base time scales.
    std::array<f32, kMaxTimeChannels> m_baseScales{
        1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f
    };

    // Contiguous 16B modulation slots (128B total, zero internal padding).
    std::array<TimeModSlot, kMaxTimeChannels> m_slots{};

    u8 m_frozenMask        = 0;
    u8 m_activeHitstopMask = 0;
    u8 m_activeRampMask    = 0;
};

// Scoped view passed by value to simulation systems during a fixed 120Hz tick.
// Non-owning view into TimeModulator; valid only when operator bool() returns true.
class SimStepView {
public:
    constexpr SimStepView() noexcept = default;

    constexpr SimStepView(u64 tickIndex, u32 subStepIndex, TimeModulator& modulator) noexcept
        : m_modulator(&modulator)
        , m_tickIndex(tickIndex)
        , m_subStepIndex(subStepIndex) {}

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return m_modulator != nullptr; }

    [[nodiscard]] constexpr f32 Dt(ChannelId auto ch = TimeChannel::World) const noexcept {
        return m_modulator->Dt(ch);
    }

    [[nodiscard]] constexpr f32 EffectiveScale(ChannelId auto ch = TimeChannel::World) const noexcept {
        return m_modulator->EffectiveScale(ch);
    }

    [[nodiscard]] constexpr bool IsFrozen(ChannelId auto ch = TimeChannel::World) const noexcept {
        return m_modulator->IsFrozen(ch);
    }

    [[nodiscard]] constexpr u8 FrozenMask() const noexcept { return m_modulator->FrozenMask(); }
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

    constexpr void RequestMaskHitstop(u8 channelMask, u32 ticks, f32 intensity = 0.0f) const noexcept {
        m_modulator->RequestMaskHitstop(channelMask, ticks, intensity);
    }

    constexpr void RequestTimeRamp(ChannelId auto ch, f32 targetScale, u32 holdTicks, u32 blendOutTicks) const noexcept {
        m_modulator->RequestTimeRamp(ch, targetScale, holdTicks, blendOutTicks);
    }

    constexpr void SetBaseScale(ChannelId auto ch, f32 scale) const noexcept {
        m_modulator->SetBaseScale(ch, scale);
    }

private:
    TimeModulator* m_modulator    = nullptr;
    u64            m_tickIndex    = 0;
    u32            m_subStepIndex = 0;
};

} // namespace en::time
