#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"

struct SDL_Gamepad;
union SDL_Event;

namespace EngineN {

/// Low-Level Hardware Abstraction: Physical Gamepad Device.
/// RAII wrapper around SDL_Gamepad*. Handles connection lifecycle, stick normalization,
/// radial/axial deadzone filtering, and haptic rumble.
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
    void HandleDeviceEvent(const SDL_Event& event);

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
