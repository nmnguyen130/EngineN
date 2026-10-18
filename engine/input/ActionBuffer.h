#pragma once

#include "input/InputTypes.h"

#include <array>
#include <bit>

namespace en::input {

// Deterministic tick-counted intent buffer for jump/dash forgiveness and hitstop freeze retention.
// Self-contained 36-byte value type; pairs with SimStepView::IsFrozen(ch).
// Invariant: Ages are defined for set bits in pending and never exceed kMaxBufferTicks.
class ActionBuffer {
public:
    static constexpr u8 kMaxBufferTicks = 60;

    constexpr void Feed(const TickInput& in, bool channelFrozen = false) noexcept {
        StepChannel(m_press, in.pressed, channelFrozen);
        StepChannel(m_release, in.released, channelFrozen);
    }

    constexpr void Clear() noexcept {
        m_press   = {};
        m_release = {};
    }

    [[nodiscard]] constexpr u16 PendingPressMask() const noexcept { return m_press.pending; }
    [[nodiscard]] constexpr u16 PendingReleaseMask() const noexcept { return m_release.pending; }

    [[nodiscard]] constexpr bool Peek(InputId auto a, u8 windowTicks) const noexcept {
        return Check(m_press, ToInputIndex(a), windowTicks);
    }

    [[nodiscard]] constexpr bool PeekRelease(InputId auto a, u8 windowTicks) const noexcept {
        return Check(m_release, ToInputIndex(a), windowTicks);
    }

    [[nodiscard]] constexpr bool Consume(InputId auto a, u8 windowTicks) noexcept {
        return Take(m_press, ToInputIndex(a), windowTicks);
    }

    [[nodiscard]] constexpr bool ConsumeRelease(InputId auto a, u8 windowTicks) noexcept {
        return Take(m_release, ToInputIndex(a), windowTicks);
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = 14695981039346656037ull;
        auto mix = [&](u64 v) constexpr noexcept {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(static_cast<u64>(m_press.pending) | (static_cast<u64>(m_release.pending) << 16));
        for (u32 m = m_press.pending; m != 0; m &= m - 1) {
            mix(m_press.age[std::countr_zero(m)]);
        }
        for (u32 m = m_release.pending; m != 0; m &= m - 1) {
            mix(m_release.age[std::countr_zero(m)]);
        }
        return h;
    }

private:
    struct Channel {
        u16 pending = 0;
        std::array<u8, kMaxActions> age{};
    };

    static constexpr void StepChannel(Channel& ch, u16 edges, bool frozen) noexcept {
        if ((ch.pending | edges) == 0) {
            return;
        }

        for (u32 m = edges; m != 0; m &= m - 1) {
            ch.age[std::countr_zero(m)] = 0;
        }
        ch.pending |= edges;

        if (frozen) {
            return;
        }

        for (u32 m = static_cast<u32>(ch.pending & ~edges); m != 0; m &= m - 1) {
            const int i = std::countr_zero(m);
            if (++ch.age[i] > kMaxBufferTicks) {
                ch.pending &= static_cast<u16>(~(1u << i));
            }
        }
    }

    [[nodiscard]] static constexpr bool Check(const Channel& ch, u8 i, u8 window) noexcept {
        return i < kMaxActions && ((ch.pending >> i) & 1u) != 0 && ch.age[i] <= window;
    }

    [[nodiscard]] static constexpr bool Take(Channel& ch, u8 i, u8 window) noexcept {
        if (!Check(ch, i, window)) {
            return false;
        }
        ch.pending &= static_cast<u16>(~(1u << i));
        return true;
    }

    Channel m_press{};
    Channel m_release{};
};

static_assert(sizeof(ActionBuffer) == 36);

} // namespace en::input
