#pragma once

#include "input/InputTypes.h"

namespace en {

// Compact 32-byte SIMD-friendly input buffer for Metroidvania jump/dash pre-buffering and coyote time.
// Value type; 0xFF denotes an unbuffered slot, and aging pauses during hitstop freezes.
class ActionBuffer {
public:
    static constexpr u8 kUnbuffered = 0xFFU;

    constexpr ActionBuffer() noexcept { Clear(); }

    constexpr void Clear() noexcept {
        m_pressAge.fill(kUnbuffered);
        m_releaseAge.fill(kUnbuffered);
    }

    constexpr void Feed(const TickInput& in, bool frozen = false) noexcept {
        if (!frozen) {
            for (u8& a : m_pressAge) {
                if (a < kUnbuffered) ++a;
            }
            for (u8& a : m_releaseAge) {
                if (a < kUnbuffered) ++a;
            }
        }
        for (u16 m = in.released; m != 0U; m &= static_cast<u16>(m - 1U)) {
            m_releaseAge[std::countr_zero(m)] = 0U;
        }
        for (u16 m = in.pressed; m != 0U; m &= static_cast<u16>(m - 1U)) {
            m_pressAge[std::countr_zero(m)] = 0U;
        }
    }

    [[nodiscard]] constexpr bool Peek(u8 action, u8 windowTicks = 12U) const noexcept {
        return m_pressAge[action & 0x0FU] <= windowTicks;
    }

    [[nodiscard]] constexpr bool Consume(u8 action, u8 windowTicks = 12U) noexcept {
        const u8 i = action & 0x0FU;
        if (m_pressAge[i] > windowTicks) return false;
        m_pressAge[i] = kUnbuffered;
        return true;
    }

    [[nodiscard]] constexpr bool ConsumeRelease(u8 action, u8 windowTicks = 12U) noexcept {
        const u8 i = action & 0x0FU;
        if (m_releaseAge[i] > windowTicks) return false;
        m_releaseAge[i] = kUnbuffered;
        return true;
    }

    constexpr void Cancel(u8 action) noexcept {
        const u8 i = action & 0x0FU;
        m_pressAge[i]   = kUnbuffered;
        m_releaseAge[i] = kUnbuffered;
    }

    [[nodiscard]] constexpr bool TryJump(
        u8 jumpAction,
        bool grounded,
        u16 ticksSinceGround,
        u8 bufferTicks = 10U,
        u8 coyoteTicks = 6U) noexcept {
        if (!grounded && ticksSinceGround > coyoteTicks) return false;
        return Consume(jumpAction, bufferTicks);
    }

private:
    std::array<u8, kMaxActions> m_pressAge{};
    std::array<u8, kMaxActions> m_releaseAge{};
};
static_assert(sizeof(ActionBuffer) == 32);

} // namespace en
