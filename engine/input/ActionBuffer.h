#pragma once

#include "input/InputTypes.h"

namespace en {

// Compact 36-byte bitmask-indexed input buffer for jump/dash pre-buffering and coyote time.
// Iterates only active buffered bits via std::countr_zero; pauses aging during hitstop freezes.
class ActionBuffer {
public:
    static constexpr u8 kMaxAge = 60U;

    constexpr ActionBuffer() noexcept { Clear(); }

    constexpr void Clear() noexcept {
        m_pressMask = 0U;
        m_releaseMask = 0U;
        m_pressAge.fill(0xFFU);
        m_releaseAge.fill(0xFFU);
    }

    constexpr void Feed(const TickInput& in, bool frozen = false) noexcept {
        if (!frozen) {
            Age(m_pressMask, m_pressAge);
            Age(m_releaseMask, m_releaseAge);
        }
        Enqueue(in.released, m_releaseMask, m_releaseAge);
        Enqueue(in.pressed, m_pressMask, m_pressAge);
    }

    [[nodiscard]] constexpr bool Peek(u8 action, u8 windowTicks = 12U) const noexcept {
        const u8 i = action & 0x0FU;
        return (m_pressMask & (1U << i)) != 0U && m_pressAge[i] <= windowTicks;
    }

    [[nodiscard]] constexpr bool Consume(u8 action, u8 windowTicks = 12U) noexcept {
        const u8 i = action & 0x0FU;
        if ((m_pressMask & (1U << i)) == 0U || m_pressAge[i] > windowTicks) return false;
        m_pressMask &= static_cast<u16>(~(1U << i));
        return true;
    }

    [[nodiscard]] constexpr bool ConsumeRelease(u8 action, u8 windowTicks = 12U) noexcept {
        const u8 i = action & 0x0FU;
        if ((m_releaseMask & (1U << i)) == 0U || m_releaseAge[i] > windowTicks) return false;
        m_releaseMask &= static_cast<u16>(~(1U << i));
        return true;
    }

    constexpr void Cancel(u8 action) noexcept {
        const u16 m = static_cast<u16>(~(1U << (action & 0x0FU)));
        m_pressMask &= m;
        m_releaseMask &= m;
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
    static constexpr void Age(u16& mask, std::array<u8, kMaxActions>& age) noexcept {
        for (u16 m = mask; m != 0U; m &= static_cast<u16>(m - 1U)) {
            const u8 i = static_cast<u8>(std::countr_zero(m));
            const u8 next = age[i] < 0xFEU ? static_cast<u8>(age[i] + 1U) : 0xFEU;
            age[i] = next;
            if (next > kMaxAge) mask &= static_cast<u16>(~(1U << i));
        }
    }

    static constexpr void Enqueue(u16 bits, u16& mask, std::array<u8, kMaxActions>& age) noexcept {
        for (; bits != 0U; bits &= static_cast<u16>(bits - 1U)) {
            const u8 i = static_cast<u8>(std::countr_zero(bits));
            age[i] = 0U;
            mask |= static_cast<u16>(1U << i);
        }
    }

    u16                         m_pressMask{0};
    u16                         m_releaseMask{0};
    std::array<u8, kMaxActions> m_pressAge{};
    std::array<u8, kMaxActions> m_releaseAge{};
};
static_assert(sizeof(ActionBuffer) == 36);

} // namespace en
