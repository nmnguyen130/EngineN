#pragma once

#include "core/Types.h"
#include <cmath>
#include <algorithm>

namespace EngineN {

// Virtual game actions decoupled from physical hardware
enum class PlayerAction : u8 {
    Jump = 0,
    Attack,
    Dash,
    Parry,
    Interact,
    MenuConfirm,
    MenuCancel,
    MenuPause,
    Count
};

// Edge and contextual state for virtual actions
enum class ActionState : u8 {
    Idle = 0,
    JustPressed,
    Held,
    JustReleased
};

// Virtual 2D gameplay axes
enum class AxisId : u8 {
    MoveX = 0,
    MoveY,
    AimX,
    AimY,
    Count
};

// Fixed array capacities to ensure zero dynamic heap allocations in hot paths
inline constexpr usize kActionCount           = static_cast<usize>(PlayerAction::Count);
inline constexpr usize kAxisCount             = static_cast<usize>(AxisId::Count);
inline constexpr usize kMaxKeyboardKeys       = 512;
inline constexpr usize kMaxMouseButtons       = 8;
inline constexpr usize kMaxTimestampedActions = 64;
inline constexpr usize kMaxBufferedActions    = 64;
inline constexpr usize kMaxKeyBindings        = 32;
inline constexpr usize kMaxMouseBindings      = 8;
inline constexpr usize kMaxGamepadBindings    = 16;

// Standard Metroidvania Input Buffering Windows (in nanoseconds)
// Derived from 120Hz fixed-timestep requirements (architecture.md):
// 100 ms = ~12 simulation ticks; 67 ms = ~8 simulation ticks; 40 ms = ~5 simulation ticks.
inline constexpr u64 kDefaultBufferWindowNs   = 100'000'000ULL; // 100 ms
inline constexpr u64 kJumpBufferWindowNs      = 100'000'000ULL; // 100 ms
inline constexpr u64 kAttackBufferWindowNs    = 100'000'000ULL; // 100 ms
inline constexpr u64 kDashBufferWindowNs      =  67'000'000ULL; //  67 ms
inline constexpr u64 kWallJumpBufferWindowNs  =  67'000'000ULL; //  67 ms
inline constexpr u64 kParryBufferWindowNs     =  40'000'000ULL; //  40 ms

[[nodiscard]] constexpr u64 MsToNs(u64 ms) noexcept {
    return ms * 1'000'000ULL;
}

[[nodiscard]] constexpr f64 NsToMs(u64 ns) noexcept {
    return static_cast<f64>(ns) / 1'000'000.0;
}

// Low-level deadzone calculation output
struct DeadzoneResult {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

/// Apply radial deadzone to a 2D analog stick input.
/// Returns {0,0} inside deadzone; rescales smoothly to [0,1] outside.
[[nodiscard]] inline DeadzoneResult ApplyRadialDeadzone(f32 rawX, f32 rawY, f32 deadzone) noexcept {
    f32 magnitude = std::sqrt(rawX * rawX + rawY * rawY);
    if (magnitude < deadzone || magnitude <= 1e-6f) {
        return {0.0f, 0.0f};
    }
    f32 scale = (magnitude - deadzone) / (1.0f - deadzone);
    if (scale > 1.0f) scale = 1.0f;
    f32 normalizedX = rawX / magnitude;
    f32 normalizedY = rawY / magnitude;
    return {normalizedX * scale, normalizedY * scale};
}

/// Apply axial deadzone to a single axis value.
[[nodiscard]] inline f32 ApplyAxialDeadzone(f32 rawValue, f32 deadzone) noexcept {
    if (std::abs(rawValue) < deadzone) {
        return 0.0f;
    }
    f32 sign = (rawValue > 0.0f) ? 1.0f : -1.0f;
    f32 scale = (std::abs(rawValue) - deadzone) / (1.0f - deadzone);
    return sign * std::min(scale, 1.0f);
}

// Sub-frame timestamped action event recorded within a single frame
struct TimestampedAction {
    PlayerAction action = PlayerAction::Jump;
    u64 timestamp = 0;   // Hardware timestamp in nanoseconds
    bool pressed = true;
};

// Binding descriptors mapping physical hardware codes to virtual actions
struct KeyBinding {
    u32 scancode = 0;
    PlayerAction action = PlayerAction::Jump;
};

struct MouseButtonBinding {
    u8 button = 0;
    PlayerAction action = PlayerAction::Attack;
};

struct GamepadButtonBinding {
    u8 button = 0;
    PlayerAction action = PlayerAction::Jump;
};

} // namespace EngineN
