#pragma once

#include "input/InputTypes.h"

namespace en {

// Input forgiveness buffer for pre-input presses, releases (variable jump cut), and coyote jumps.
// Value type owning a 16-action age table with zero double-tap or motion-ring bloat.
// Invariant: sizeof(ActionBuffer) == 36 and Feed() ages only active bitmasks.
class alignas(4) ActionBuffer {
public:
    static constexpr u8 kMaxBufferTicks = 60;

    constexpr ActionBuffer() noexcept { Clear(); }

    constexpr void Clear() noexcept {
        m_pendingPressMask = 0U;
        m_pendingReleaseMask = 0U;
        m_pressAge.fill(0xFFU);
        m_releaseAge.fill(0xFFU);
    }

    constexpr void Feed(const TickInput& in, bool channelFrozen = false) noexcept {
        if (!channelFrozen) {
            AgeMask(m_pendingPressMask, m_pressAge);
            AgeMask(m_pendingReleaseMask, m_releaseAge);
        }
        EnqueueActions(in.released, m_pendingReleaseMask, m_releaseAge);
        EnqueueActions(in.pressed, m_pendingPressMask, m_pressAge);
    }

    [[nodiscard]] constexpr u16 PendingPressMask() const noexcept { return m_pendingPressMask; }
    [[nodiscard]] constexpr u16 PendingReleaseMask() const noexcept { return m_pendingReleaseMask; }

    [[nodiscard]] constexpr bool Peek(u8 a, u8 windowTicks = 12) const noexcept {
        const u8 idx = a & 0x0FU;
        return ((m_pendingPressMask & (1U << idx)) != 0U) && (m_pressAge[idx] <= windowTicks);
    }

    [[nodiscard]] constexpr bool Consume(u8 a, u8 windowTicks = 12) noexcept {
        const u8 idx = a & 0x0FU;
        if (!Peek(idx, windowTicks)) return false;
        m_pendingPressMask &= static_cast<u16>(~(1U << idx));
        return true;
    }

    [[nodiscard]] constexpr bool PeekRelease(u8 a, u8 windowTicks = 12) const noexcept {
        const u8 idx = a & 0x0FU;
        return ((m_pendingReleaseMask & (1U << idx)) != 0U) && (m_releaseAge[idx] <= windowTicks);
    }

    [[nodiscard]] constexpr bool ConsumeRelease(u8 a, u8 windowTicks = 12) noexcept {
        const u8 idx = a & 0x0FU;
        if (!PeekRelease(idx, windowTicks)) return false;
        m_pendingReleaseMask &= static_cast<u16>(~(1U << idx));
        return true;
    }

    constexpr void Cancel(u8 a) noexcept {
        const u16 clearMask = static_cast<u16>(~(1U << (a & 0x0FU)));
        m_pendingPressMask &= clearMask;
        m_pendingReleaseMask &= clearMask;
    }

    [[nodiscard]] constexpr i32 ConsumeHighestPriority(
        std::span<const u8> priorityOrder, u8 windowTicks = 12) noexcept {
        i32 winningSlot = -1;
        for (usize i = 0U; i < priorityOrder.size(); ++i) {
            if (winningSlot < 0) {
                if (Consume(priorityOrder[i], windowTicks)) {
                    winningSlot = static_cast<i32>(i);
                }
            } else {
                Cancel(priorityOrder[i]);
            }
        }
        return winningSlot;
    }

    [[nodiscard]] constexpr bool TryConsumeJump(
        u8 jumpAction,
        bool isGrounded,
        u16 ticksSinceGrounded,
        u8 jumpBufferTicks = 10,
        u8 coyoteTicks = 6) noexcept {
        if (!isGrounded && ticksSinceGrounded > coyoteTicks) return false;
        return Consume(jumpAction, jumpBufferTicks);
    }

private:
    static constexpr void AgeMask(u16& mask, std::array<u8, kMaxActions>& age) noexcept {
        u16 m = mask;
        while (m != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(m));
            m &= static_cast<u16>(m - 1U);
            const u8 nextAge = age[idx] < 0xFEU ? static_cast<u8>(age[idx] + 1U) : 0xFEU;
            age[idx] = nextAge;
            if (nextAge > kMaxBufferTicks) {
                mask &= static_cast<u16>(~(1U << idx));
            }
        }
    }

    static constexpr void EnqueueActions(u16 newBits, u16& mask, std::array<u8, kMaxActions>& age) noexcept {
        u16 b = newBits;
        while (b != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(b));
            b &= static_cast<u16>(b - 1U);
            age[idx] = 0U;
            mask |= static_cast<u16>(1U << idx);
        }
    }

    u16 m_pendingPressMask{0};
    u16 m_pendingReleaseMask{0};
    std::array<u8, kMaxActions> m_pressAge{};
    std::array<u8, kMaxActions> m_releaseAge{};
};
static_assert(sizeof(ActionBuffer) == 36);

} // namespace en
