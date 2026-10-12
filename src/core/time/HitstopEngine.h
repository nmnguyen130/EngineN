#pragma once

#include "core/Types.h"

#include <array>

namespace EngineN {

inline constexpr u8 kMaxTimeChannels = 8;
inline constexpr u8 kDefaultTimeChannel = 0;

/**
 * HitstopEngine: Channel-agnostic impact freeze coordinator.
 * Ticks continue to advance while freezing delta times for designated channels.
 * Uses merge-by-max to prevent micro-stuttering or cancellation.
 */
class HitstopEngine {
public:
    template <typename C = u8>
    void Request(C channel, u32 ticks) noexcept {
        const u8 ch = static_cast<u8>(channel);
        if (ch < kMaxTimeChannels) {
            m_ticks[ch] = (m_ticks[ch] > ticks) ? m_ticks[ch] : ticks;
        }
    }

    void AdvanceTick() noexcept {
        for (u32& t : m_ticks) {
            if (t > 0) {
                --t;
            }
        }
    }

    void Reset() noexcept {
        m_ticks.fill(0);
    }

    template <typename C = u8>
    [[nodiscard]] bool IsFrozen(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        return (ch < kMaxTimeChannels) && (m_ticks[ch] > 0);
    }

    template <typename C = u8>
    [[nodiscard]] f32 GetModifier(C channel = kDefaultTimeChannel) const noexcept {
        return IsFrozen(channel) ? 0.0f : 1.0f;
    }

    template <typename C = u8>
    [[nodiscard]] u32 GetRemainingTicks(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        return (ch < kMaxTimeChannels) ? m_ticks[ch] : 0U;
    }

private:
    std::array<u32, kMaxTimeChannels> m_ticks{};
};

} // namespace EngineN
