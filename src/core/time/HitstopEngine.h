#pragma once

#include "core/Types.h"

#include <array>

namespace EngineN {

inline constexpr u8 kMaxTimeChannels = 8;
inline constexpr u8 kDefaultTimeChannel = 0;

/**
 * HitstopEngine: Channel-agnostic impact freeze and slow-motion coordinator.
 * Supports both hard freeze (intensity 0.0f) and soft hitstop friction (intensity 0.1f..0.3f).
 * Uses duration-dominant merge to prevent combat micro-stuttering or cancellation.
 */
class HitstopEngine {
public:
    struct State {
        u32 ticks     = 0;
        f32 intensity = 0.0f; // 0.0f = hard freeze, 0.1f..0.3f = soft hitstop friction
    };

    template <typename C = u8>
    void Request(C channel, u32 ticks, f32 intensity = 0.0f) noexcept {
        const u8 ch = static_cast<u8>(channel);
        if (ch >= kMaxTimeChannels || ticks == 0) {
            return;
        }

        const f32 clampedIntensity = (intensity < 0.0f) ? 0.0f : ((intensity > 1.0f) ? 1.0f : intensity);
        State& s = m_states[ch];

        // Merge policy: Longer duration wins. If equal duration, harder freeze (lower intensity) wins.
        if (ticks > s.ticks || (ticks == s.ticks && clampedIntensity < s.intensity)) {
            s.ticks = ticks;
            s.intensity = clampedIntensity;
        }
    }

    template <typename C = u8>
    void RequestHard(C channel, u32 ticks) noexcept {
        Request(channel, ticks, 0.0f);
    }

    void AdvanceTick() noexcept {
        for (State& s : m_states) {
            if (s.ticks > 0) {
                --s.ticks;
                if (s.ticks == 0) {
                    s.intensity = 0.0f;
                }
            }
        }
    }

    void Reset() noexcept {
        m_states.fill({});
    }

    template <typename C = u8>
    [[nodiscard]] bool IsFrozen(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        return (ch < kMaxTimeChannels) && (m_states[ch].ticks > 0) && (m_states[ch].intensity <= 0.0f);
    }

    template <typename C = u8>
    [[nodiscard]] bool IsActive(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        return (ch < kMaxTimeChannels) && (m_states[ch].ticks > 0);
    }

    template <typename C = u8>
    [[nodiscard]] f32 GetModifier(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        if (ch >= kMaxTimeChannels || m_states[ch].ticks == 0) {
            return 1.0f;
        }
        return m_states[ch].intensity;
    }

    template <typename C = u8>
    [[nodiscard]] u32 GetRemainingTicks(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        return (ch < kMaxTimeChannels) ? m_states[ch].ticks : 0U;
    }

    template <typename C = u8>
    [[nodiscard]] f32 GetIntensity(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        return (ch < kMaxTimeChannels) ? m_states[ch].intensity : 0.0f;
    }

private:
    std::array<State, kMaxTimeChannels> m_states{};
};

} // namespace EngineN
