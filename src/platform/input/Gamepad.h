#pragma once

#include "core/Types.h"
#include <cmath>
#include <algorithm>

struct SDL_Gamepad;

namespace EngineN {

struct DeadzoneResult {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

/// Apply radial deadzone to a 2D stick input.
/// Returns {0,0} inside deadzone; rescales outside to [0,1].
inline DeadzoneResult ApplyRadialDeadzone(f32 rawX, f32 rawY, f32 deadzone) noexcept {
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
inline f32 ApplyAxialDeadzone(f32 rawValue, f32 deadzone) noexcept {
    if (std::abs(rawValue) < deadzone) {
        return 0.0f;
    }
    f32 sign = (rawValue > 0.0f) ? 1.0f : -1.0f;
    f32 scale = (std::abs(rawValue) - deadzone) / (1.0f - deadzone);
    return sign * std::min(scale, 1.0f);
}

/// RAII wrapper around SDL_Gamepad*.
/// Handles connection lifecycle, stick normalization, and deadzones.
class Gamepad {
public:
    Gamepad() = default;
    ~Gamepad();

    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;
    Gamepad(Gamepad&& other) noexcept;
    Gamepad& operator=(Gamepad&& other) noexcept;

    bool Open(u32 instanceId);
    void Close();

    [[nodiscard]] bool IsConnected() const noexcept { return m_gamepad != nullptr; }
    [[nodiscard]] u32 GetInstanceId() const noexcept { return m_instanceId; }
    [[nodiscard]] const char* GetName() const;

    [[nodiscard]] bool IsButtonDown(u8 button) const;
    [[nodiscard]] f32 GetAxisNormalized(u8 axis) const;

    [[nodiscard]] DeadzoneResult GetLeftStick(f32 deadzone = 0.22f) const;
    [[nodiscard]] DeadzoneResult GetRightStick(f32 deadzone = 0.22f) const;

    [[nodiscard]] f32 GetLeftTrigger() const;
    [[nodiscard]] f32 GetRightTrigger() const;

    bool Rumble(u16 lowFreq, u16 highFreq, u32 durationMs);

    [[nodiscard]] SDL_Gamepad* GetRawHandle() noexcept { return m_gamepad; }

private:
    SDL_Gamepad* m_gamepad = nullptr;
    u32 m_instanceId = 0;
};

} // namespace EngineN
