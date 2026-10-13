#pragma once

#include "core/Types.h"
#include "math/Vec2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace EngineN {

using ActionId = u8;
using AxisId = u8;
using ContextId = u8;

inline constexpr usize kMaxActions = 16;
inline constexpr usize kMaxAxes = 4;
inline constexpr usize kMaxContexts = 4; // Context stack depth

enum class Device : u8 {
    None = 0,
    Keyboard,
    Mouse,
    Gamepad
};

enum class Edge : u8 {
    Press = 0,
    Release
};

enum class InputError : u8 {
    InvalidSource = 0,
    TableFull,
    Duplicate,
    NotFound,
    StackFull,
    StackEmpty,
    InvalidParam
};

enum class GamepadAxis : u8 {
    None = 0,
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger
};

/**
 * Validated physical input source across all device types.
 * Gamepad bindings can target a specific instance ID or kAnyGamepad.
 */
struct Source {
    static constexpr u32 kAnyGamepad = ~0u;
    static constexpr u16 kMaxScancodes = 512;
    static constexpr u16 kMaxMouseButtons = 8;
    static constexpr u16 kMaxGamepadButtons = 32;

    Device device = Device::None;
    u16    code   = 0;
    u32    gamepad = kAnyGamepad;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        switch (device) {
            case Device::Keyboard: return code > 0 && code < kMaxScancodes;
            case Device::Mouse:    return code > 0 && code < kMaxMouseButtons;
            case Device::Gamepad:  return code < kMaxGamepadButtons;
            default:               return false;
        }
    }

    [[nodiscard]] constexpr bool Matches(const Source& physical) const noexcept {
        if (device != physical.device || code != physical.code) {
            return false;
        }
        return device != Device::Gamepad || gamepad == kAnyGamepad || gamepad == physical.gamepad;
    }

    [[nodiscard]] constexpr bool operator==(const Source&) const noexcept = default;
};

[[nodiscard]] inline f32 ApplyAxialDeadzone(f32 value, f32 deadzone) noexcept {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    deadzone = std::clamp(deadzone, 0.0f, 0.99f);
    value = std::clamp(value, -1.0f, 1.0f);
    const f32 mag = std::abs(value);
    if (mag <= deadzone) {
        return 0.0f;
    }
    return std::copysign((mag - deadzone) / (1.0f - deadzone), value);
}

namespace detail {
    [[nodiscard]] constexpr bool IsBitSet(u32 mask, usize bit) noexcept {
        return bit < kMaxActions && ((mask >> bit) & 1u) != 0;
    }
}

/**
 * Immutable per-tick input state consumed by 120Hz simulation.
 * Trivially copyable POD guaranteed to fit in a single 64-byte L1 cache line.
 */
struct TickInput {
    u64 tick = 0;
    u32 held = 0;
    u32 pressed = 0;
    u32 released = 0;
    std::array<i16, kMaxAxes> axis{};
    std::array<u16, kMaxActions> heldTicks{};

    template <typename A = ActionId>
    [[nodiscard]] constexpr bool Held(A a) const noexcept {
        return detail::IsBitSet(held, static_cast<usize>(a));
    }

    template <typename A = ActionId>
    [[nodiscard]] constexpr bool Pressed(A a) const noexcept {
        return detail::IsBitSet(pressed, static_cast<usize>(a));
    }

    template <typename A = ActionId>
    [[nodiscard]] constexpr bool Released(A a) const noexcept {
        return detail::IsBitSet(released, static_cast<usize>(a));
    }

    template <typename A = ActionId>
    [[nodiscard]] constexpr u16 HeldTicks(A a) const noexcept {
        const usize idx = static_cast<usize>(a);
        return (idx < kMaxActions) ? heldTicks[idx] : 0;
    }

    template <typename A = AxisId>
    [[nodiscard]] constexpr f32 Axis(A a) const noexcept {
        const usize ax = static_cast<usize>(a);
        if (ax >= kMaxAxes) {
            return 0.0f;
        }
        return std::clamp(static_cast<f32>(axis[ax]) / 32767.0f, -1.0f, 1.0f);
    }

    template <typename X = AxisId, typename Y = AxisId>
    [[nodiscard]] constexpr Vec2 Stick(X axisX, Y axisY) const noexcept {
        return Vec2{Axis(axisX), Axis(axisY)};
    }
};

static_assert(sizeof(TickInput) <= 64, "TickInput must stay within 64 bytes");

/**
 * Diagnostic or UI action transition record within a single render frame.
 */
struct ActionEvent {
    u64 timestampNs = 0;
    ActionId action = 0;
    Edge edge = Edge::Press;
    bool synthetic = false;
};

/**
 * Immutable per-frame input snapshot consumed by UI, cursor, and dev tools.
 * Snapshot contents remain valid until the next PublishFrameInput() call.
 */
struct FrameInput {
    static constexpr usize kMaxEvents = 64;

    u64 frameIndex = 0;
    u64 timestampNs = 0;
    u32 held = 0;
    u32 pressed = 0;
    u32 released = 0;
    Vec2 mousePosition{0.0f, 0.0f};
    Vec2 mouseDelta{0.0f, 0.0f};
    Vec2 mouseWheel{0.0f, 0.0f};
    std::array<ActionEvent, kMaxEvents> events{};
    u32 eventCount = 0;
    u32 droppedEvents = 0;

    [[nodiscard]] std::span<const ActionEvent> Events() const noexcept {
        return {events.data(), std::min<usize>(eventCount, events.size())};
    }

    template <typename A = ActionId>
    [[nodiscard]] constexpr bool Held(A a) const noexcept {
        return detail::IsBitSet(held, static_cast<usize>(a));
    }

    template <typename A = ActionId>
    [[nodiscard]] constexpr bool Pressed(A a) const noexcept {
        return detail::IsBitSet(pressed, static_cast<usize>(a));
    }

    template <typename A = ActionId>
    [[nodiscard]] constexpr bool Released(A a) const noexcept {
        return detail::IsBitSet(released, static_cast<usize>(a));
    }
};

} // namespace EngineN
