#pragma once

#include "input/InputTypes.h"

namespace en {

// Unified 64-byte (1 L1 cache line exact, zero padding) deterministic input forgiveness and motion buffer.
// Combines pre-input press/release buffering, simultaneous chord slop forgiveness, release-gated double-tap
// detection, priority chains, coyote jump, and an 11-entry Numpad direction ring buffer for motion/charge specials.
// Invariant: sizeof(ActionBuffer) == 64 and Feed() ages only active bitmasks (0 loop iterations when idle).
class alignas(64) ActionBuffer {
public:
    static constexpr u8 kMaxBufferTicks = 60;
    static constexpr u8 kDirRingCapacity = 11;

    constexpr void Clear() noexcept {
        m_pendingPressMask = 0U;
        m_pendingReleaseMask = 0U;
        m_armedDoubleTapMask = 0U;
        m_doubleTapMask = 0U;
        m_pressAge.fill(0xFFU);
        m_releaseAge.fill(0xFFU);
        m_dirRing.fill(5U);
        m_dirTicks.fill(0U);
        m_dirHead = 0U;
        m_dirCount = 0U;
    }

    constexpr void Feed(
        const TickInput& in,
        bool channelFrozen = false,
        u8 doubleTapMaxGapTicks = 15,
        u8 moveAxisX = 0,
        u8 moveAxisY = 1) noexcept {
        m_doubleTapMask = 0U;

        if (!channelFrozen) {
            u16 pMask = static_cast<u16>(m_pendingPressMask | m_armedDoubleTapMask);
            while (pMask != 0U) {
                const u8 idx = static_cast<u8>(std::countr_zero(pMask));
                pMask &= static_cast<u16>(pMask - 1U);
                const u16 bit = static_cast<u16>(1U << idx);
                const u8 nextAge = m_pressAge[idx] < 0xFEU ? static_cast<u8>(m_pressAge[idx] + 1U) : 0xFEU;
                m_pressAge[idx] = nextAge;
                if (nextAge > kMaxBufferTicks) {
                    m_pendingPressMask &= static_cast<u16>(~bit);
                }
                if (nextAge > doubleTapMaxGapTicks) {
                    m_armedDoubleTapMask &= static_cast<u16>(~bit);
                }
            }

            u16 rMask = m_pendingReleaseMask;
            while (rMask != 0U) {
                const u8 idx = static_cast<u8>(std::countr_zero(rMask));
                rMask &= static_cast<u16>(rMask - 1U);
                const u8 nextRelAge = m_releaseAge[idx] < 0xFEU ? static_cast<u8>(m_releaseAge[idx] + 1U) : 0xFEU;
                m_releaseAge[idx] = nextRelAge;
                if (nextRelAge > kMaxBufferTicks) {
                    m_pendingReleaseMask &= static_cast<u16>(~(1U << idx));
                }
            }
        }

        // Release edge arms the double-tap window if the initial tap was brief (<= doubleTapMaxGapTicks).
        u16 newRel = in.released;
        while (newRel != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(newRel));
            newRel &= static_cast<u16>(newRel - 1U);
            const u16 bit = static_cast<u16>(1U << idx);
            m_releaseAge[idx] = 0U;
            m_pendingReleaseMask |= bit;
            if (m_pressAge[idx] <= doubleTapMaxGapTicks) {
                m_armedDoubleTapMask |= bit;
            }
        }

        // Press edge triggers double-tap only if armed by a prior release within doubleTapMaxGapTicks.
        u16 newPress = in.pressed;
        while (newPress != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(newPress));
            newPress &= static_cast<u16>(newPress - 1U);
            const u16 bit = static_cast<u16>(1U << idx);

            if ((m_armedDoubleTapMask & bit) != 0U && m_pressAge[idx] <= doubleTapMaxGapTicks) {
                m_doubleTapMask |= bit;
                m_armedDoubleTapMask &= static_cast<u16>(~bit);
            }

            m_pressAge[idx] = 0U;
            m_pendingPressMask |= bit;
        }

        // Record Numpad direction [1..9] into the 11-slot circular history ring
        const u8 numpad = in.StickNumpad(moveAxisX, moveAxisY);
        if (m_dirCount > 0U && m_dirRing[m_dirHead] == numpad) {
            if (!channelFrozen && m_dirTicks[m_dirHead] < 0xFFU) {
                ++m_dirTicks[m_dirHead];
            }
        } else {
            if (m_dirCount == 0U) {
                m_dirHead = 0U;
                m_dirCount = 1U;
            } else {
                m_dirHead = static_cast<u8>((m_dirHead + 1U) % kDirRingCapacity);
                if (m_dirCount < kDirRingCapacity) ++m_dirCount;
            }
            m_dirRing[m_dirHead] = numpad;
            m_dirTicks[m_dirHead] = 1U;
        }
    }

    [[nodiscard]] constexpr u16 PendingPressMask() const noexcept { return m_pendingPressMask; }
    [[nodiscard]] constexpr u16 PendingReleaseMask() const noexcept { return m_pendingReleaseMask; }
    [[nodiscard]] constexpr u16 DoubleTapMask() const noexcept { return m_doubleTapMask; }
    [[nodiscard]] constexpr u8 CurrentNumpadDir() const noexcept {
        return m_dirCount > 0U ? m_dirRing[m_dirHead] : 5U;
    }

    [[nodiscard]] constexpr bool Peek(InputId auto a, u8 windowTicks = 12) const noexcept {
        const u8 idx = ToInputIndex(a);
        return ((m_pendingPressMask & (1U << idx)) != 0U) && (m_pressAge[idx] <= windowTicks);
    }

    [[nodiscard]] constexpr bool Consume(InputId auto a, u8 windowTicks = 12) noexcept {
        const u8 idx = ToInputIndex(a);
        if (!Peek(idx, windowTicks)) return false;
        m_pendingPressMask &= static_cast<u16>(~(1U << idx));
        return true;
    }

    [[nodiscard]] constexpr bool PeekRelease(InputId auto a, u8 windowTicks = 12) const noexcept {
        const u8 idx = ToInputIndex(a);
        return ((m_pendingReleaseMask & (1U << idx)) != 0U) && (m_releaseAge[idx] <= windowTicks);
    }

    [[nodiscard]] constexpr bool ConsumeRelease(InputId auto a, u8 windowTicks = 12) noexcept {
        const u8 idx = ToInputIndex(a);
        if (!PeekRelease(idx, windowTicks)) return false;
        m_pendingReleaseMask &= static_cast<u16>(~(1U << idx));
        return true;
    }

    [[nodiscard]] constexpr bool WasDoubleTapped(InputId auto a) const noexcept {
        return (m_doubleTapMask & (1U << ToInputIndex(a))) != 0U;
    }

    [[nodiscard]] constexpr bool ConsumeDoubleTap(InputId auto a) noexcept {
        const u8 idx = ToInputIndex(a);
        const u16 bit = static_cast<u16>(1U << idx);
        if ((m_doubleTapMask & bit) == 0U) return false;
        m_doubleTapMask &= static_cast<u16>(~bit);
        m_pendingPressMask &= static_cast<u16>(~bit);
        return true;
    }

    // Consumes a simultaneous multi-action chord if all required actions were pressed within slopWindowTicks
    // and are currently held, atomically cancelling their individual buffered presses.
    [[nodiscard]] constexpr bool ConsumeChord(
        u16 chordMask, u16 currentlyHeldMask, u8 slopWindowTicks = 4) noexcept {
        if (chordMask == 0U ||
            (currentlyHeldMask & chordMask) != chordMask ||
            (m_pendingPressMask & chordMask) != chordMask) {
            return false;
        }
        u16 req = chordMask;
        while (req != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(req));
            req &= static_cast<u16>(req - 1U);
            if (m_pressAge[idx] > slopWindowTicks) return false;
        }
        m_pendingPressMask &= static_cast<u16>(~chordMask);
        return true;
    }

    // Scans the Numpad ring backward for a directional motion (e.g. {2, 3, 6} QCF, {6, 2, 3} DP, {6, 5, 6} Dash),
    // automatically flipping horizontal directions when facingRight == false.
    [[nodiscard]] constexpr bool CheckMotion(
        std::span<const u8> motionNumpad,
        bool facingRight = true,
        u8 maxTotalTicks = 24) const noexcept {
        if (motionNumpad.empty() || m_dirCount < motionNumpad.size()) return false;
        usize targetIdx = motionNumpad.size() - 1U;
        u8 wanted = FlipNumpadHorizontal(motionNumpad[targetIdx], facingRight);
        u32 elapsedTicks = 0U;

        for (u8 step = 0U; step < m_dirCount; ++step) {
            const u8 ringSlot = static_cast<u8>((m_dirHead + kDirRingCapacity - step) % kDirRingCapacity);
            const u8 recordedDir = m_dirRing[ringSlot];
            if (recordedDir == wanted) {
                if (targetIdx == 0U) return true;
                --targetIdx;
                wanted = FlipNumpadHorizontal(motionNumpad[targetIdx], facingRight);
            }
            elapsedTicks += m_dirTicks[ringSlot];
            if (elapsedTicks > maxTotalTicks) break;
        }
        return false;
    }

    [[nodiscard]] constexpr bool ConsumeMotion(
        std::span<const u8> motionNumpad,
        InputId auto attackAction,
        bool facingRight = true,
        u8 maxMotionTicks = 24,
        u8 attackWindowTicks = 12) noexcept {
        if (!Peek(attackAction, attackWindowTicks)) return false;
        if (!CheckMotion(motionNumpad, facingRight, maxMotionTicks)) return false;
        return Consume(attackAction, attackWindowTicks);
    }

    // Checks charge motions (e.g. [4]6 Back-Forward or [2]8 Down-Up), treating Down-Back (1) as charging both 4 and 2.
    [[nodiscard]] constexpr bool CheckChargeMotion(
        u8 chargeNumpadDir,
        u8 releaseNumpadDir,
        u8 minChargeTicks = 30,
        bool facingRight = true,
        u8 maxReleaseGapTicks = 10) const noexcept {
        if (m_dirCount < 2U) return false;
        const u8 wantedRelease = FlipNumpadHorizontal(releaseNumpadDir, facingRight);
        const u8 wantedCharge = FlipNumpadHorizontal(chargeNumpadDir, facingRight);

        u32 gapTicks = 0U;
        bool seenRelease = false;
        u32 accumulatedCharge = 0U;

        for (u8 step = 0U; step < m_dirCount; ++step) {
            const u8 ringSlot = static_cast<u8>((m_dirHead + kDirRingCapacity - step) % kDirRingCapacity);
            const u8 dir = m_dirRing[ringSlot];
            const u8 ticks = m_dirTicks[ringSlot];

            if (!seenRelease) {
                if (dir == wantedRelease) {
                    seenRelease = true;
                } else {
                    gapTicks += ticks;
                    if (gapTicks > maxReleaseGapTicks) return false;
                }
            } else if (MatchesChargeDirection(dir, wantedCharge)) {
                accumulatedCharge += ticks;
                if (accumulatedCharge >= minChargeTicks) return true;
            } else if (accumulatedCharge > 0U) {
                break;
            } else {
                gapTicks += ticks;
                if (gapTicks > maxReleaseGapTicks) return false;
            }
        }
        return false;
    }

    constexpr void Cancel(InputId auto a) noexcept {
        const u8 idx = ToInputIndex(a);
        const u16 bit = static_cast<u16>(1U << idx);
        m_pendingPressMask &= static_cast<u16>(~bit);
        m_pendingReleaseMask &= static_cast<u16>(~bit);
        m_armedDoubleTapMask &= static_cast<u16>(~bit);
        m_doubleTapMask &= static_cast<u16>(~bit);
    }

    // Evaluates highest-priority buffered action in priorityOrder, consuming the winner and cancelling lower ones.
    template <InputId ActionT>
    [[nodiscard]] constexpr i32 ConsumeHighestPriority(
        std::span<const ActionT> priorityOrder, u8 windowTicks = 12) noexcept {
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
        InputId auto jumpAction,
        bool isGrounded,
        u16 ticksSinceGrounded,
        u8 jumpBufferTicks = 10,
        u8 coyoteTicks = 6) noexcept {
        if (!isGrounded && ticksSinceGrounded > coyoteTicks) return false;
        return Consume(jumpAction, jumpBufferTicks);
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        const u64 masks = static_cast<u64>(m_pendingPressMask) |
                          (static_cast<u64>(m_pendingReleaseMask) << 16U) |
                          (static_cast<u64>(m_armedDoubleTapMask) << 32U) |
                          (static_cast<u64>(m_doubleTapMask) << 48U);
        u64 h = Mix64(masks);
        for (usize i = 0U; i < kMaxActions; ++i) {
            const u64 pair = static_cast<u64>(m_pressAge[i]) |
                             (static_cast<u64>(m_releaseAge[i]) << 8U);
            h = HashCombine64(h, pair);
        }
        return HashCombine64(h, static_cast<u64>(m_dirHead) | (static_cast<u64>(m_dirCount) << 8U));
    }

private:
    [[nodiscard]] static constexpr u8 FlipNumpadHorizontal(u8 dir, bool facingRight) noexcept {
        if (facingRight) return dir;
        switch (dir) {
            case 1U: return 3U;
            case 3U: return 1U;
            case 4U: return 6U;
            case 6U: return 4U;
            case 7U: return 9U;
            case 9U: return 7U;
            default: return dir;
        }
    }

    [[nodiscard]] static constexpr bool MatchesChargeDirection(u8 actualDir, u8 wantedCharge) noexcept {
        if (actualDir == wantedCharge) return true;
        if (wantedCharge == 2U) return actualDir == 1U || actualDir == 3U;
        if (wantedCharge == 4U) return actualDir == 1U || actualDir == 7U;
        if (wantedCharge == 6U) return actualDir == 3U || actualDir == 9U;
        return false;
    }

    // Offset 0..39 (40B action press/release/double-tap buffer)
    u16 m_pendingPressMask{0};
    u16 m_pendingReleaseMask{0};
    u16 m_armedDoubleTapMask{0};
    u16 m_doubleTapMask{0};
    std::array<u8, kMaxActions> m_pressAge{
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    std::array<u8, kMaxActions> m_releaseAge{
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};

    // Offset 40..63 (24B 11-entry Numpad direction & charge ring buffer)
    std::array<u8, kDirRingCapacity> m_dirRing{5U, 5U, 5U, 5U, 5U, 5U, 5U, 5U, 5U, 5U, 5U};
    std::array<u8, kDirRingCapacity> m_dirTicks{};
    u8 m_dirHead{0};
    u8 m_dirCount{0};
};
static_assert(sizeof(ActionBuffer) == 64);

} // namespace en
