#include "platform/input/Gamepad.h"
#include "core/Log.h"
#include <SDL3/SDL.h>

namespace EngineN {

Gamepad::~Gamepad() {
    Close();
}

Gamepad::Gamepad(Gamepad&& other) noexcept
    : m_gamepad(other.m_gamepad),
      m_instanceId(other.m_instanceId) {
    other.m_gamepad = nullptr;
    other.m_instanceId = 0;
}

Gamepad& Gamepad::operator=(Gamepad&& other) noexcept {
    if (this != &other) {
        Close();
        m_gamepad = other.m_gamepad;
        m_instanceId = other.m_instanceId;
        other.m_gamepad = nullptr;
        other.m_instanceId = 0;
    }
    return *this;
}

bool Gamepad::Open(u32 instanceId) {
    Close();

    m_gamepad = SDL_OpenGamepad(instanceId);
    if (!m_gamepad) {
        ENGINE_LOG(Error, Platform, "SDL_OpenGamepad failed for ID {}: {}", instanceId, SDL_GetError());
        return false;
    }

    m_instanceId = instanceId;
    const char* name = SDL_GetGamepadName(m_gamepad);
    ENGINE_LOG(Info, Platform, "Gamepad connected: {} (Instance ID: {})", name ? name : "Unknown", instanceId);
    return true;
}

void Gamepad::Close() {
    if (m_gamepad) {
        SDL_CloseGamepad(m_gamepad);
        m_gamepad = nullptr;
        m_instanceId = 0;
        ENGINE_LOG(Info, Platform, "Gamepad disconnected");
    }
}

void Gamepad::HandleDeviceEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        if (!IsConnected()) {
            Open(event.gdevice.which);
        }
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        if (IsConnected() && m_instanceId == event.gdevice.which) {
            Close();
        }
    }
}

const char* Gamepad::GetName() const {
    if (!m_gamepad) return "Disconnected";
    const char* name = SDL_GetGamepadName(m_gamepad);
    return name ? name : "Unknown Gamepad";
}

bool Gamepad::IsButtonDown(u8 button) const {
    if (!m_gamepad) return false;
    return SDL_GetGamepadButton(m_gamepad, static_cast<SDL_GamepadButton>(button));
}

f32 Gamepad::GetAxisNormalized(u8 axis) const {
    if (!m_gamepad) return 0.0f;
    i16 raw = SDL_GetGamepadAxis(m_gamepad, static_cast<SDL_GamepadAxis>(axis));
    if (raw >= 0) {
        return static_cast<f32>(raw) / 32767.0f;
    }
    return static_cast<f32>(raw) / 32768.0f;
}

DeadzoneResult Gamepad::GetLeftStick(f32 deadzone) const {
    f32 x = GetAxisNormalized(SDL_GAMEPAD_AXIS_LEFTX);
    f32 y = GetAxisNormalized(SDL_GAMEPAD_AXIS_LEFTY);
    return ApplyRadialDeadzone(x, y, deadzone);
}

DeadzoneResult Gamepad::GetRightStick(f32 deadzone) const {
    f32 x = GetAxisNormalized(SDL_GAMEPAD_AXIS_RIGHTX);
    f32 y = GetAxisNormalized(SDL_GAMEPAD_AXIS_RIGHTY);
    return ApplyRadialDeadzone(x, y, deadzone);
}

f32 Gamepad::GetLeftTrigger() const {
    if (!m_gamepad) return 0.0f;
    i16 raw = SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    return std::clamp(static_cast<f32>(raw) / 32767.0f, 0.0f, 1.0f);
}

f32 Gamepad::GetRightTrigger() const {
    if (!m_gamepad) return 0.0f;
    i16 raw = SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    return std::clamp(static_cast<f32>(raw) / 32767.0f, 0.0f, 1.0f);
}

bool Gamepad::Rumble(u16 lowFreq, u16 highFreq, u32 durationMs) {
    if (!m_gamepad) return false;
    return SDL_RumbleGamepad(m_gamepad, lowFreq, highFreq, durationMs);
}

} // namespace EngineN
