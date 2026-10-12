#pragma once

#include "core/Types.h"

#include <algorithm>
#include <array>
#include <type_traits>

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
    void Request(u8 channel, u32 ticks) noexcept {
        if (channel < kMaxTimeChannels) {
            m_ticks[channel] = std::max(m_ticks[channel], ticks);
        }
    }

    template <typename E>
        requires std::is_enum_v<E>
    void Request(E channel, u32 ticks) noexcept {
        Request(static_cast<u8>(channel), ticks);
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

    [[nodiscard]] bool IsFrozen(u8 channel) const noexcept {
        return (channel < kMaxTimeChannels) && (m_ticks[channel] > 0);
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] bool IsFrozen(E channel) const noexcept {
        return IsFrozen(static_cast<u8>(channel));
    }

    [[nodiscard]] f32 GetModifier(u8 channel) const noexcept {
        return IsFrozen(channel) ? 0.0f : 1.0f;
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] f32 GetModifier(E channel) const noexcept {
        return GetModifier(static_cast<u8>(channel));
    }

    [[nodiscard]] u32 GetRemainingTicks(u8 channel) const noexcept {
        return (channel < kMaxTimeChannels) ? m_ticks[channel] : 0U;
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] u32 GetRemainingTicks(E channel) const noexcept {
        return GetRemainingTicks(static_cast<u8>(channel));
    }

private:
    std::array<u32, kMaxTimeChannels> m_ticks{};
};

} // namespace EngineN
