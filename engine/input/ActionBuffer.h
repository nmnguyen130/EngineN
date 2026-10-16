#pragma once

#include "input/InputTypes.h"

#include <array>
#include <bit>

namespace en::input {

// Deterministic tick-counted intent buffer for jump/dash forgiveness and hitstop freeze retention.
// Self-contained 40-byte value type; pairs with SimStepView::IsFrozen(ch).
// Invariant: Ages never exceed kMaxBufferTicks; expired bits are automatically cleared.
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
        mix(m_press.pending);
        for (const u8 a : m_press.age) mix(a);
        mix(m_release.pending);
        for (const u8 a : m_release.age) mix(a);
        return h;
    }

private:
    struct Channel {
        u16 pending = 0;
        std::array<u8, kMaxActions> age{};
    };

    static constexpr void StepChannel(Channel& ch, u16 edges, bool frozen) noexcept {
        for (u32 m = edges; m != 0; m &= m - 1) {
            ch.age[static_cast<usize>(std::countr_zero(m))] = 0;
        }
        ch.pending |= edges;

        if (frozen) {
            return;
        }

        for (u32 m = static_cast<u32>(ch.pending & ~edges); m != 0; m &= m - 1) {
            const usize i = static_cast<usize>(std::countr_zero(m));
            if (++ch.age[i] > kMaxBufferTicks) {
                ch.pending &= static_cast<u16>(~(1u << i));
                ch.age[i]   = 0;
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
        ch.age[i]   = 0;
        return true;
    }

    Channel m_press{};
    Channel m_release{};
};

} // namespace en::input