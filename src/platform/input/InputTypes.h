#pragma once

#include "core/Types.h"
#include <cmath>
#include <algorithm>

namespace EngineN {

/// Logical gameplay and interface actions decoupled from physical hardware.
enum class InputAction : u8 {
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

/// High-level input contexts isolating gameplay from menus and debugging.
enum class InputContext : u8 {
    Gameplay = 0,
    UI,
    Debug,
    Count
};

/// Virtual 2D directional axes.
enum class AxisId : u8 {
    MoveX = 0,
    MoveY,
    AimX,
    AimY,
    Count
};

inline constexpr usize kActionCount = static_cast<usize>(InputAction::Count);
inline constexpr usize kContextCount = static_cast<usize>(InputContext::Count);
inline constexpr usize kAxisCount = static_cast<usize>(AxisId::Count);

inline constexpr usize kMaxKeyboardKeys = 512;
inline constexpr usize kMaxMouseButtons = 8;

inline constexpr usize kMaxKeyBindings = 32;
inline constexpr usize kMaxMouseBindings = 16;
inline constexpr usize kMaxAxisBindings = 12;
inline constexpr usize kMaxFrameLogActions = 32;

// Standard action intent buffering window (100ms = ~12 ticks at 120Hz)
inline constexpr u64 kDefaultBufferWindowNs = 100'000'000ULL;

/**
 * ActionFrameState: Tri-state model separating continuous hold from discrete edges.
 * Allows both a press and release occurring within the same frame/tick to be captured.
 */
struct ActionFrameState {
    bool down = false;
    bool pressed = false;
    bool released = false;
};

struct TimestampedAction {
    InputAction action = InputAction::Jump;
    u64 timestampNs = 0;
    bool pressed = false;
};

struct DeadzoneResult {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

/// Apply radial deadzone to 2D analog input with finite validation and smooth rescale.
[[nodiscard]] inline DeadzoneResult ApplyRadialDeadzone(f32 rawX, f32 rawY, f32 deadzone) noexcept {
    if (!std::isfinite(rawX) || !std::isfinite(rawY) || !std::isfinite(deadzone)) {
        return {0.0f, 0.0f};
    }
    f32 clampedDeadzone = std::clamp(deadzone, 0.0f, 0.999f);
    f32 magnitude = std::sqrt(rawX * rawX + rawY * rawY);
    if (magnitude <= clampedDeadzone || magnitude <= 1e-6f) {
        return {0.0f, 0.0f};
    }
    f32 scale = std::min((magnitude - clampedDeadzone) / (1.0f - clampedDeadzone), 1.0f);
    return {(rawX / magnitude) * scale, (rawY / magnitude) * scale};
}

/// Apply axial deadzone to a 1D axis with finite validation and smooth rescale.
[[nodiscard]] inline f32 ApplyAxialDeadzone(f32 rawValue, f32 deadzone) noexcept {
    if (!std::isfinite(rawValue) || !std::isfinite(deadzone)) {
        return 0.0f;
    }
    f32 clampedDeadzone = std::clamp(deadzone, 0.0f, 0.999f);
    if (std::abs(rawValue) <= clampedDeadzone) {
        return 0.0f;
    }
    f32 sign = (rawValue > 0.0f) ? 1.0f : -1.0f;
    f32 scale = std::min((std::abs(rawValue) - clampedDeadzone) / (1.0f - clampedDeadzone), 1.0f);
    return sign * scale;
}

struct KeyBinding {
    u32 scancode = 0;
    InputAction action = InputAction::Jump;
    InputContext context = InputContext::Gameplay;
};

struct MouseButtonBinding {
    u8 button = 0;
    InputAction action = InputAction::Attack;
    InputContext context = InputContext::Gameplay;
};

struct AxisKeyBinding {
    AxisId axis = AxisId::MoveX;
    u32 negativeScancode = 0;
    u32 positiveScancode = 0;
    InputContext context = InputContext::Gameplay;
};

} // namespace EngineN
