#pragma once

#include "input/InputTypes.h"

namespace en {

// Deterministic tick-counted input forgiveness buffer for responsive combat and platforming.
class alignas(4) ActionBuffer {
public:
    static constexpr u8 kMaxBufferTicks = 60;

    constexpr void Clear() noexcept {
        m_pendingPressMask = 0;
        m_pendingReleaseMask = 0;
        m_pressAge.fill(0);
        m_releaseAge.fill(0);
    }

    constexpr void Feed(const TickInput& in, bool channelFrozen = false) noexcept {
        if (!channelFrozen) {
            u16 pMask = m_pendingPressMask;
            while (pMask != 0) {
                const u8 idx = static_cast<u8>(std::countr_zero(pMask));
                pMask &= static_cast<u16>(pMask - 1U);
                if (++m_pressAge[idx] > kMaxBufferTicks) {
                    m_pendingPressMask &= static_cast<u16>(~(1U << idx));
                }
            }
            u16 rMask = m_pendingReleaseMask;
            while (rMask != 0) {
                const u8 idx = static_cast<u8>(std::countr_zero(rMask));
                rMask &= static_cast<u16>(rMask - 1U);
                if (++m_releaseAge[idx] > kMaxBufferTicks) {
                    m_pendingReleaseMask &= static_cast<u16>(~(1U << idx));
                }
            }
        }

        u16 newPress = in.pressed;
        while (newPress != 0) {
            const u8 idx = static_cast<u8>(std::countr_zero(newPress));
            newPress &= static_cast<u16>(newPress - 1U);
            m_pressAge[idx] = 0;
            m_pendingPressMask |= static_cast<u16>(1U << idx);
        }

        u16 newRel = in.released;
        while (newRel != 0) {
            const u8 idx = static_cast<u8>(std::countr_zero(newRel));
            newRel &= static_cast<u16>(newRel - 1U);
            m_releaseAge[idx] = 0;
            m_pendingReleaseMask |= static_cast<u16>(1U << idx);
        }
    }

    [[nodiscard]] constexpr u16 PendingPressMask() const noexcept { return m_pendingPressMask; }
    [[nodiscard]] constexpr u16 PendingReleaseMask() const noexcept { return m_pendingReleaseMask; }

    [[nodiscard]] constexpr bool Peek(InputId auto a, u8 windowTicks = 12) const noexcept {
        const u8 idx = ToInputIndex(a);
        return ((m_pendingPressMask & (1U << idx)) != 0) && (m_pressAge[idx] <= windowTicks);
    }

    [[nodiscard]] constexpr bool Consume(InputId auto a, u8 windowTicks = 12) noexcept {
        const u8 idx = ToInputIndex(a);
        if (!Peek(idx, windowTicks)) return false;
        m_pendingPressMask &= static_cast<u16>(~(1U << idx));
        return true;
    }

    [[nodiscard]] constexpr bool PeekRelease(InputId auto a, u8 windowTicks = 12) const noexcept {
        const u8 idx = ToInputIndex(a);
        return ((m_pendingReleaseMask & (1U << idx)) != 0) && (m_releaseAge[idx] <= windowTicks);
    }

    [[nodiscard]] constexpr bool ConsumeRelease(InputId auto a, u8 windowTicks = 12) noexcept {
        const u8 idx = ToInputIndex(a);
        if (!PeekRelease(idx, windowTicks)) return false;
        m_pendingReleaseMask &= static_cast<u16>(~(1U << idx));
        return true;
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = Mix64(static_cast<u64>(m_pendingPressMask) | (static_cast<u64>(m_pendingReleaseMask) << 16U));
        u16 mask = m_pendingPressMask;
        while (mask != 0) {
            const u8 idx = static_cast<u8>(std::countr_zero(mask));
            mask &= static_cast<u16>(mask - 1U);
            h = HashCombine64(h, (static_cast<u64>(idx) << 8U) | m_pressAge[idx]);
        }
        return h;
    }

private:
    u16 m_pendingPressMask{0};
    u16 m_pendingReleaseMask{0};
    std::array<u8, kMaxActions> m_pressAge{};
    std::array<u8, kMaxActions> m_releaseAge{};
};
static_assert(sizeof(ActionBuffer) == 36);

// Detects Double-Tap dashes and Hold-to-Charge attacks across 120 Hz simulation ticks.
class GestureDetector {
public:
    constexpr void Clear() noexcept {
        m_lastTapAge.fill(255);
        m_holdTicks.fill(0);
        m_releasedChargeTicks.fill(0);
        m_doubleTapMask = 0;
    }

    constexpr void Feed(const TickInput& in, u8 doubleTapMaxGapTicks = 18) noexcept {
        m_doubleTapMask = 0;
        for (u8 i = 0; i < kMaxActions; ++i) {
            if (m_lastTapAge[i] < 255) ++m_lastTapAge[i];
            const u16 bit = static_cast<u16>(1U << i);
            if ((in.pressed & bit) != 0) {
                if (m_lastTapAge[i] <= doubleTapMaxGapTicks) {
                    m_doubleTapMask |= bit;
                    m_lastTapAge[i] = 255;
                } else {
                    m_lastTapAge[i] = 0;
                }
            }
            if ((in.held & bit) != 0) {
                if (m_holdTicks[i] < 0xFFFFU) ++m_holdTicks[i];
            } else {
                if ((in.released & bit) != 0) {
                    m_releasedChargeTicks[i] = m_holdTicks[i];
                } else {
                    m_releasedChargeTicks[i] = 0;
                }
                m_holdTicks[i] = 0;
            }
        }
    }

    [[nodiscard]] constexpr bool WasDoubleTapped(InputId auto a) const noexcept {
        return (m_doubleTapMask & (1U << ToInputIndex(a))) != 0;
    }

    [[nodiscard]] constexpr f32 HoldChargeRatio(InputId auto a, u16 fullChargeTicks = 30) const noexcept {
        if (fullChargeTicks == 0) return 1.0f;
        const u16 held = m_holdTicks[ToInputIndex(a)];
        return std::clamp(static_cast<f32>(held) / static_cast<f32>(fullChargeTicks), 0.0f, 1.0f);
    }

    [[nodiscard]] constexpr bool ConsumeChargedRelease(InputId auto a, u16 minChargeTicks = 20) noexcept {
        const u8 idx = ToInputIndex(a);
        if (m_releasedChargeTicks[idx] >= minChargeTicks) {
            m_releasedChargeTicks[idx] = 0;
            return true;
        }
        return false;
    }

private:
    std::array<u8, kMaxActions> m_lastTapAge{};
    std::array<u16, kMaxActions> m_holdTicks{};
    std::array<u16, kMaxActions> m_releasedChargeTicks{};
    u16 m_doubleTapMask{0};
};

} // namespace en
