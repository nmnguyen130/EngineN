#pragma once

#include "core/Types.h"
#include "math/Vec2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <type_traits>

namespace EngineN {

inline constexpr usize kMaxActions         = 16;
inline constexpr usize kMaxAxes            = 4;
inline constexpr usize kMaxBindingsPerSlot = 4;

template <typename T>
concept InputId = (std::unsigned_integral<T> || std::is_enum_v<T>) && sizeof(T) <= sizeof(u32);

[[nodiscard]] constexpr u8 ToInputIndex(InputId auto id) noexcept {
    return static_cast<u8>(id);
}

[[nodiscard]] constexpr u16 MakeActionMask(InputId auto... actions) noexcept {
    return static_cast<u16>(((1u << ToInputIndex(actions)) | ... | 0u));
}

enum class Device : u8 {
    None = 0,
    Keyboard,
    Mouse,
    Gamepad
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

enum class SocdPolicy : u8 {
    LastInputWins = 0,
    Neutral
};

// Packed 4-byte physical control token (Keyboard scancode, Mouse button, or Gamepad button).
// Value type; gamepadIndex == kAnyGamepad matches any connected controller slot [0..3].
// Invariant: sizeof(Source) == 4.
struct Source {
    static constexpr u8  kAnyGamepad        = 0xFF;
    static constexpr u16 kMaxScancodes      = 512;
    static constexpr u16 kMaxMouseButtons   = 8;
    static constexpr u16 kMaxGamepadButtons = 32;

    Device device       = Device::None;
    u8     gamepadIndex = kAnyGamepad;
    u16    code         = 0;

    [[nodiscard]] static constexpr Source Key(u16 scancode) noexcept {
        return {.device = Device::Keyboard, .gamepadIndex = kAnyGamepad, .code = scancode};
    }

    [[nodiscard]] static constexpr Source Mouse(u16 button) noexcept {
        return {.device = Device::Mouse, .gamepadIndex = kAnyGamepad, .code = button};
    }

    [[nodiscard]] static constexpr Source Pad(u16 button, u8 padSlot = kAnyGamepad) noexcept {
        return {.device = Device::Gamepad, .gamepadIndex = padSlot, .code = button};
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        switch (device) {
            case Device::Keyboard: return code > 0 && code < kMaxScancodes;
            case Device::Mouse:    return code > 0 && code < kMaxMouseButtons;
            case Device::Gamepad:  return code < kMaxGamepadButtons;
            default:               return false;
        }
    }

    [[nodiscard]] constexpr bool operator==(const Source&) const noexcept = default;
};

static_assert(sizeof(Source) == 4);

struct AxisBinding {
    Source      negative{};
    Source      positive{};
    GamepadAxis gamepadAxis  = GamepadAxis::None;
    u8          gamepadIndex = Source::kAnyGamepad;
    SocdPolicy  socd         = SocdPolicy::LastInputWins;
    bool        inverted     = false;
    f32         deadzone     = 0.15f;
    f32         scale        = 1.0f;
};

// Direct-indexed binding table eliminating dynamic search and runtime capacity errors.
// Pure data aggregate; can be swapped, serialized, or mutated at zero cost.
// Invariant: Every action owns exactly kMaxBindingsPerSlot physical source slots.
struct InputMap {
    std::array<std::array<Source, kMaxBindingsPerSlot>, kMaxActions> actions{};
    std::array<AxisBinding, kMaxAxes>                                axes{};

    constexpr InputMap& Bind(InputId auto action, std::initializer_list<Source> sources) noexcept {
        const u8 idx = ToInputIndex(action);
        if (idx < kMaxActions) {
            actions[idx].fill(Source{});
            usize slot = 0;
            for (const Source s : sources) {
                if (s.IsValid() && slot < kMaxBindingsPerSlot) {
                    actions[idx][slot++] = s;
                }
            }
        }
        return *this;
    }

    constexpr InputMap& BindSlot(InputId auto action, usize slot, Source src) noexcept {
        const u8 idx = ToInputIndex(action);
        if (idx < kMaxActions && slot < kMaxBindingsPerSlot) {
            actions[idx][slot] = src.IsValid() ? src : Source{};
        }
        return *this;
    }

    constexpr InputMap& BindAxis(InputId auto axis, const AxisBinding& cfg) noexcept {
        const u8 idx = ToInputIndex(axis);
        if (idx < kMaxAxes) {
            axes[idx] = cfg;
            axes[idx].deadzone = std::clamp(cfg.deadzone, 0.0f, 0.99f);
        }
        return *this;
    }
};

[[nodiscard]] constexpr f32 ApplyAxialDeadzone(f32 value, f32 deadzone) noexcept {
    deadzone = std::clamp(deadzone, 0.0f, 0.99f);
    value    = std::clamp(value, -1.0f, 1.0f);
    const f32 mag = (value < 0.0f) ? -value : value;
    if (mag <= deadzone) {
        return 0.0f;
    }
    const f32 norm = (mag - deadzone) / (1.0f - deadzone);
    return (value < 0.0f) ? -norm : norm;
}

[[nodiscard]] inline Vec2 ApplyRadialDeadzone(Vec2 stick, f32 deadzone) noexcept {
    deadzone = std::clamp(deadzone, 0.0f, 0.99f);
    const f32 lenSq = stick.x * stick.x + stick.y * stick.y;
    if (!std::isfinite(lenSq) || lenSq <= deadzone * deadzone) {
        return Vec2{0.0f, 0.0f};
    }
    const f32 len    = std::sqrt(lenSq);
    const f32 scaled = (std::min(len, 1.0f) - deadzone) / (1.0f - deadzone);
    return Vec2{(stick.x / len) * scaled, (stick.y / len) * scaled};
}

// L1 cache-aligned immutable 120Hz simulation input snapshot.
// Trivially copyable; suitable for deterministic lockstep and replay recording.
// Invariant: (pressed & released) == 0 on every tick; sizeof(TickInput) == 64.
struct alignas(64) TickInput {
    u64 tick     = 0;
    u16 held     = 0;
    u16 pressed  = 0;
    u16 released = 0;
    u16 pad0     = 0;
    std::array<i16, kMaxAxes>    axis{};
    std::array<u16, kMaxActions> heldTicks{};
    u64 reserved = 0;

    [[nodiscard]] constexpr bool Held(InputId auto a) const noexcept {
        return ((held >> ToInputIndex(a)) & 1u) != 0;
    }

    [[nodiscard]] constexpr bool Pressed(InputId auto a) const noexcept {
        return ((pressed >> ToInputIndex(a)) & 1u) != 0;
    }

    [[nodiscard]] constexpr bool Released(InputId auto a) const noexcept {
        return ((released >> ToInputIndex(a)) & 1u) != 0;
    }

    [[nodiscard]] constexpr u16 HeldTicks(InputId auto a) const noexcept {
        const u8 idx = ToInputIndex(a);
        return (idx < kMaxActions) ? heldTicks[idx] : 0;
    }

    [[nodiscard]] constexpr f32 Axis(InputId auto ax) const noexcept {
        const u8 idx = ToInputIndex(ax);
        return (idx < kMaxAxes)
            ? std::clamp(static_cast<f32>(axis[idx]) / 32767.0f, -1.0f, 1.0f)
            : 0.0f;
    }

    [[nodiscard]] Vec2 Stick(InputId auto axX, InputId auto axY, f32 radialDeadzone = 0.0f) const noexcept {
        const Vec2 raw{Axis(axX), Axis(axY)};
        return (radialDeadzone > 0.0f) ? ApplyRadialDeadzone(raw, radialDeadzone) : raw;
    }
};

static_assert(sizeof(TickInput) == 64);

// L1 cache-aligned render-frame input view for UI, cursor, and key-rebinding menus.
// Computed once per frame in EndInputFrame().
// Invariant: sizeof(FrameInput) == 64.
struct alignas(64) FrameInput {
    u64    frameIndex    = 0;
    u16    held          = 0;
    u16    pressed       = 0;
    u16    released      = 0;
    Device activeDevice  = Device::None;
    u8     pad0          = 0;
    Source lastPressed{}; // Captures physical button press for "Press any key to rebind"
    Vec2   mousePos{0.0f, 0.0f};
    Vec2   mouseDelta{0.0f, 0.0f};
    Vec2   mouseWheel{0.0f, 0.0f};
    u8     pad1[20]{};

    [[nodiscard]] constexpr bool Held(InputId auto a) const noexcept {
        return ((held >> ToInputIndex(a)) & 1u) != 0;
    }

    [[nodiscard]] constexpr bool Pressed(InputId auto a) const noexcept {
        return ((pressed >> ToInputIndex(a)) & 1u) != 0;
    }

    [[nodiscard]] constexpr bool Released(InputId auto a) const noexcept {
        return ((released >> ToInputIndex(a)) & 1u) != 0;
    }
};

static_assert(sizeof(FrameInput) == 64);

} // namespace EngineN