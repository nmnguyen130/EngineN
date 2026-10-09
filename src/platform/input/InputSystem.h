#pragma once

#include "core/Types.h"
#include "platform/PlatformTypes.h"
#include "platform/input/InputTypes.h"
#include "platform/input/Keyboard.h"
#include "platform/input/Mouse.h"
#include "platform/input/Gamepad.h"
#include "platform/input/ActionMapper.h"
#include "platform/input/ActionBuffer.h"

union SDL_Event;

namespace EngineN {

/// High-Level Input Subsystem Facade for EngineN.
/// Decouples and coordinates:
///   1. Low-Level Hardware: Keyboard, Mouse, Gamepad abstractions.
///   2. Mid-Level Mapping: Multi-input binding tables, reference counts, virtual axes.
///   3. High-Level Buffering: Metroidvania ActionBuffer with windowed consumption and anti-ghosting.
///
/// Fully deterministic and zero dynamic heap allocations in hot paths.
/// Implements IEventListener for modular decoupled event routing.
class InputSystem : public IEventListener {
public:
    InputSystem();
    ~InputSystem() override = default;

    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;
    InputSystem(InputSystem&&) noexcept = default;
    InputSystem& operator=(InputSystem&&) noexcept = default;

    // --- Frame Lifecycle ---
    void BeginFrame();
    void ProcessEvent(const SDL_Event& event) override;
    void EndFrame();

    // --- Low-Level Subsystem Accessors ---
    [[nodiscard]] const Keyboard& GetKeyboard() const noexcept { return m_keyboard; }
    [[nodiscard]] Keyboard& GetKeyboard() noexcept { return m_keyboard; }

    [[nodiscard]] const Mouse& GetMouse() const noexcept { return m_mouse; }
    [[nodiscard]] Mouse& GetMouse() noexcept { return m_mouse; }

    [[nodiscard]] const Gamepad& GetGamepad() const noexcept { return m_gamepad; }
    [[nodiscard]] Gamepad& GetGamepad() noexcept { return m_gamepad; }

    [[nodiscard]] const ActionMapper& GetActionMapper() const noexcept { return m_actionMapper; }
    [[nodiscard]] ActionMapper& GetActionMapper() noexcept { return m_actionMapper; }

    [[nodiscard]] const ActionBuffer& GetActionBuffer() const noexcept { return m_actionBuffer; }
    [[nodiscard]] ActionBuffer& GetActionBuffer() noexcept { return m_actionBuffer; }

    // --- Continuous Keyboard Polling (Convenience forwards) ---
    [[nodiscard]] bool IsKeyDown(u32 scancode) const noexcept { return m_keyboard.IsDown(scancode); }
    [[nodiscard]] bool IsKeyUp(u32 scancode) const noexcept { return m_keyboard.IsUp(scancode); }
    [[nodiscard]] bool IsKeyJustPressed(u32 scancode) const noexcept { return m_keyboard.IsJustPressed(scancode); }
    [[nodiscard]] bool IsKeyJustReleased(u32 scancode) const noexcept { return m_keyboard.IsJustReleased(scancode); }

    // --- Continuous Mouse Polling (Convenience forwards) ---
    [[nodiscard]] bool IsMouseButtonDown(u8 button) const noexcept { return m_mouse.IsButtonDown(button); }
    [[nodiscard]] bool IsMouseButtonJustPressed(u8 button) const noexcept { return m_mouse.IsButtonJustPressed(button); }
    [[nodiscard]] bool IsMouseButtonJustReleased(u8 button) const noexcept { return m_mouse.IsButtonJustReleased(button); }
    [[nodiscard]] i32 GetMouseX() const noexcept { return m_mouse.GetX(); }
    [[nodiscard]] i32 GetMouseY() const noexcept { return m_mouse.GetY(); }
    [[nodiscard]] i32 GetMouseDeltaX() const noexcept { return m_mouse.GetDeltaX(); }
    [[nodiscard]] i32 GetMouseDeltaY() const noexcept { return m_mouse.GetDeltaY(); }
    [[nodiscard]] f32 GetMouseWheelX() const noexcept { return m_mouse.GetWheelX(); }
    [[nodiscard]] f32 GetMouseWheelY() const noexcept { return m_mouse.GetWheelY(); }

    // --- Gamepad Polling & Deadzones ---
    [[nodiscard]] f32 GetRawAxis(AxisId axis) const noexcept;
    [[nodiscard]] bool HasGamepad() const noexcept { return m_gamepad.IsConnected(); }
    void SetStickDeadzone(f32 deadzone) noexcept { m_stickDeadzone = deadzone; }
    [[nodiscard]] f32 GetStickDeadzone() const noexcept { return m_stickDeadzone; }

    // --- Mid-Level Virtual Action State Queries ---
    [[nodiscard]] ActionState GetActionState(PlayerAction action) const noexcept { return m_actionMapper.GetActionState(action); }
    [[nodiscard]] bool IsActionJustPressed(PlayerAction action) const noexcept { return m_actionMapper.IsActionJustPressed(action); }
    [[nodiscard]] bool IsActionHeld(PlayerAction action) const noexcept { return m_actionMapper.IsActionHeld(action); }
    [[nodiscard]] bool IsActionJustReleased(PlayerAction action) const noexcept { return m_actionMapper.IsActionJustReleased(action); }

    // --- Mid-Level Virtual Axis Queries ---
    [[nodiscard]] f32 GetAxis(AxisId axis) const noexcept;

    // --- Action Rebinding ---
    void SetDefaultBindings() { m_actionMapper.SetDefaultBindings(); }
    void BindKey(u32 scancode, PlayerAction action) { m_actionMapper.BindKey(scancode, action); }
    void BindMouseButton(u8 button, PlayerAction action) { m_actionMapper.BindMouseButton(button, action); }
    void BindGamepadButton(u8 button, PlayerAction action) { m_actionMapper.BindGamepadButton(button, action); }
    void UnbindKey(u32 scancode) { m_actionMapper.UnbindKey(scancode); }
    void UnbindMouseButton(u8 button) { m_actionMapper.UnbindMouseButton(button); }
    void UnbindGamepadButton(u8 button) { m_actionMapper.UnbindGamepadButton(button); }
    void ClearBindings() { m_actionMapper.ClearBindings(); }

    // --- High-Level Metroidvania Action Buffering & Forgiveness ---
    /// Consumes a buffered action intent within a maximum forgiveness window (using current hardware ticks).
    [[nodiscard]] bool ConsumeAction(PlayerAction action, u64 maxWindowNs = kDefaultBufferWindowNs) noexcept {
        return m_actionBuffer.ConsumeAction(action, maxWindowNs);
    }

    /// Consumes a buffered action intent within a deterministic simulation tick window.
    [[nodiscard]] bool ConsumeAction(PlayerAction action, u64 currentTimestampNs, u64 maxWindowNs) noexcept {
        return m_actionBuffer.ConsumeAction(action, currentTimestampNs, maxWindowNs);
    }

    /// Checks if a buffered action intent exists within the window without consuming it.
    [[nodiscard]] bool HasBufferedAction(PlayerAction action, u64 maxWindowNs = kDefaultBufferWindowNs) const noexcept {
        return m_actionBuffer.HasAction(action, maxWindowNs);
    }

    [[nodiscard]] bool HasBufferedAction(PlayerAction action, u64 currentTimestampNs, u64 maxWindowNs) const noexcept {
        return m_actionBuffer.HasAction(action, currentTimestampNs, maxWindowNs);
    }

    void ClearActionBuffer() noexcept { m_actionBuffer.Clear(); }

    // --- Sub-Frame Timestamped Action Queue (Per-Frame Backward-Compatibility) ---
    [[nodiscard]] u32 GetTimestampedActionCount() const noexcept { return m_frameActionCount; }
    [[nodiscard]] const TimestampedAction& GetTimestampedAction(u32 index) const noexcept;

private:
    void ProcessKeyEvent(const SDL_Event& event);
    void ProcessMouseButtonEvent(const SDL_Event& event);
    void ProcessMouseMotionEvent(const SDL_Event& event);
    void ProcessMouseWheelEvent(const SDL_Event& event);
    void ProcessGamepadButtonEvent(const SDL_Event& event);
    void ProcessGamepadDeviceEvent(const SDL_Event& event);

    void PushFrameTimestampedAction(PlayerAction action, u64 timestamp, bool pressed);

    Keyboard m_keyboard;
    Mouse m_mouse;
    Gamepad m_gamepad;
    ActionMapper m_actionMapper;
    ActionBuffer m_actionBuffer;

    f32 m_stickDeadzone = 0.22f;

    std::array<TimestampedAction, kMaxTimestampedActions> m_frameActions{};
    u32 m_frameActionCount = 0;
};

} // namespace EngineN
