#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>

namespace EngineN {

/**
 * ActionBuffer: Tick-counted intent buffer for action forgiveness (Jump/Dash buffering, hitstop freeze).
 * Decoupled from wall clock, zero SDL dependency, fully deterministic across replays.
 */
class ActionBuffer {
public:
    static constexpr u8  kMaxBufferTicks = 60; // 500ms at 120Hz
    static constexpr u32 kActionMask = (1u << kMaxActions) - 1u;

    void Feed(const TickInput& in, bool frozen = false) noexcept;
    void Clear() noexcept;

    template <typename A = ActionId>
    [[nodiscard]] bool Peek(A a, u8 windowTicks) const noexcept {
        return PeekImpl(m_press, static_cast<usize>(a), windowTicks);
    }

    template <typename A = ActionId>
    [[nodiscard]] bool PeekRelease(A a, u8 windowTicks) const noexcept {
        return PeekImpl(m_release, static_cast<usize>(a), windowTicks);
    }

    template <typename A = ActionId>
    [[nodiscard]] bool Consume(A a, u8 windowTicks) noexcept {
        return ConsumeImpl(m_press, static_cast<usize>(a), windowTicks);
    }

    template <typename A = ActionId>
    [[nodiscard]] bool ConsumeRelease(A a, u8 windowTicks) noexcept {
        return ConsumeImpl(m_release, static_cast<usize>(a), windowTicks);
    }

    template <typename A = ActionId>
    void Clear(A a) noexcept {
        const usize act = static_cast<usize>(a);
        ClearImpl(m_press, act);
        ClearImpl(m_release, act);
    }

private:
    struct Channel {
        u32 pending = 0;
        std::array<u8, kMaxActions> age{};
    };

    static void FeedImpl(Channel& ch, u32 edges, bool frozen) noexcept;
    [[nodiscard]] static bool PeekImpl(const Channel& ch, usize a, u8 window) noexcept;
    [[nodiscard]] static bool ConsumeImpl(Channel& ch, usize a, u8 window) noexcept;
    static void ClearImpl(Channel& ch, usize a) noexcept;

    Channel m_press;
    Channel m_release;
};

} // namespace EngineN
