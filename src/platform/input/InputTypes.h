#pragma once

#include "core/Types.h"
#include "math/Vec2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <type_traits>

namespace EngineN {

using ActionId = u8;
using AxisId = u8;
using ContextId = u8;

constexpr usize kMaxActions = 16;
constexpr usize kMaxAxes = 4;
constexpr usize kMaxContexts = 4;
constexpr u16 kMaxKeyboardKeys = 512;
constexpr u16 kMaxMouseButtons = 8;
constexpr u16 kMaxGamepadButtons = 32;
constexpr u32 kMaxFrameLogEvents = 64;
constexpr u64 kDefaultTickDurationNs = 8'333'333ULL;

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
    ContextStackFull,
    ContextStackEmpty,
    NotFound
};

/**
 * Validated physical input source across all device types.
 * Strictly range-checked to prevent out-of-bounds reads on state arrays.
 */
struct Source {
    Device device = Device::None;
    u16 code = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        switch (device) {
            case Device::Keyboard:
                return code > 0 && code < kMaxKeyboardKeys;
            case Device::Mouse:
                return code >= 1 && code < kMaxMouseButtons;
            case Device::Gamepad:
                return code < kMaxGamepadButtons;
            default:
                return false;
        }
    }

    [[nodiscard]] constexpr bool operator==(const Source& other) const noexcept = default;
};

inline constexpr f32 ApplyAxialDeadzone(f32 value, f32 deadzone) noexcept {
    const f32 absVal = std::abs(value);
    if (absVal <= deadzone) {
        return 0.0f;
    }
    const f32 sign = (value > 0.0f) ? 1.0f : -1.0f;
    return sign * (absVal - deadzone) / (1.0f - deadzone);
}

inline Vec2 ApplyRadialDeadzone(Vec2 input, f32 deadzone) noexcept {
    const f32 mag = input.Length();
    if (mag <= deadzone) {
        return Vec2{0.0f, 0.0f};
    }
    const f32 normMag = std::min(1.0f, (mag - deadzone) / (1.0f - deadzone));
    return (input / mag) * normMag;
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
    i16 axis[kMaxAxes]{};
    u16 heldTicks[kMaxActions]{};

    [[nodiscard]] constexpr bool Held(ActionId a) const noexcept {
        return a < kMaxActions && ((held & (1u << a)) != 0);
    }
    [[nodiscard]] constexpr bool Pressed(ActionId a) const noexcept {
        return a < kMaxActions && ((pressed & (1u << a)) != 0);
    }
    [[nodiscard]] constexpr bool Released(ActionId a) const noexcept {
        return a < kMaxActions && ((released & (1u << a)) != 0);
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Held(E a) const noexcept {
        return Held(static_cast<ActionId>(a));
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Pressed(E a) const noexcept {
        return Pressed(static_cast<ActionId>(a));
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Released(E a) const noexcept {
        return Released(static_cast<ActionId>(a));
    }

    [[nodiscard]] constexpr f32 Axis(AxisId a) const noexcept {
        return (a < kMaxAxes) ? (static_cast<f32>(axis[a]) / 32767.0f) : 0.0f;
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] constexpr f32 Axis(E a) const noexcept {
        return Axis(static_cast<AxisId>(a));
    }

    [[nodiscard]] Vec2 Stick(AxisId axisX, AxisId axisY) const noexcept {
        return Vec2{Axis(axisX), Axis(axisY)};
    }
};

static_assert(std::is_trivially_copyable_v<TickInput>);
static_assert(sizeof(TickInput) <= 64, "TickInput must fit in a 64-byte cache line");

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
 * Populated at the end of Platform::Pump without interfering with simulation queues.
 */
struct FrameInput {
    u64 frameIndex = 0;
    u64 timestampNs = 0;
    u32 held = 0;
    u32 pressed = 0;
    u32 released = 0;
    Vec2 mousePosition{0.0f, 0.0f};
    Vec2 mouseDelta{0.0f, 0.0f};
    Vec2 mouseWheel{0.0f, 0.0f};
    std::array<ActionEvent, kMaxFrameLogEvents> events{};
    u32 eventCount = 0;
    u32 droppedEvents = 0;

    [[nodiscard]] std::span<const ActionEvent> Events() const noexcept {
        return std::span<const ActionEvent>(events.data(), eventCount);
    }

    [[nodiscard]] constexpr bool Held(ActionId a) const noexcept {
        return a < kMaxActions && ((held & (1u << a)) != 0);
    }
    [[nodiscard]] constexpr bool Pressed(ActionId a) const noexcept {
        return a < kMaxActions && ((pressed & (1u << a)) != 0);
    }
    [[nodiscard]] constexpr bool Released(ActionId a) const noexcept {
        return a < kMaxActions && ((released & (1u << a)) != 0);
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Held(E a) const noexcept {
        return Held(static_cast<ActionId>(a));
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Pressed(E a) const noexcept {
        return Pressed(static_cast<ActionId>(a));
    }

    template <typename E>
    requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Released(E a) const noexcept {
        return Released(static_cast<ActionId>(a));
    }
};

} // namespace EngineN
