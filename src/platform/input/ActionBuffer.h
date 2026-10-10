#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>
#include <type_traits>

namespace EngineN {

/**
 * Tick-counted intent buffer for action forgiveness (Jump/Dash buffering, hitstop freeze).
 * Decoupled from real-world wall clock, zero SDL dependency, fully deterministic.
 */
class ActionBuffer {
public:
    void Feed(const TickInput& in, bool frozen = false) noexcept;

    [[nodiscard]] bool Peek(ActionId a, u8 windowTicks) const noexcept;
    [[nodiscard]] bool Consume(ActionId a, u8 windowTicks) noexcept;
    [[nodiscard]] bool ConsumeRelease(ActionId a, u8 windowTicks) noexcept;

    void Clear() noexcept;
    void Clear(ActionId a) noexcept;

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] bool Peek(E a, u8 windowTicks) const noexcept {
        return Peek(static_cast<ActionId>(a), windowTicks);
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] bool Consume(E a, u8 windowTicks) noexcept {
        return Consume(static_cast<ActionId>(a), windowTicks);
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] bool ConsumeRelease(E a, u8 windowTicks) noexcept {
        return ConsumeRelease(static_cast<ActionId>(a), windowTicks);
    }

    template <typename E>
    requires std::is_enum_v<E>
    void Clear(E a) noexcept {
        Clear(static_cast<ActionId>(a));
    }

private:
    std::array<u8, kMaxActions> m_pressAge{};
    std::array<u8, kMaxActions> m_releaseAge{};
    u32 m_pressPending = 0;
    u32 m_releasePending = 0;
};

constexpr u8 TicksFromMs(u32 ms) noexcept {
    return static_cast<u8>((ms * 120 + 500) / 1000);
}

} // namespace EngineN
