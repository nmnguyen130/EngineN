#pragma once

#include "core/Types.h"
#include "platform/input/Gamepad.h"
#include <array>

union SDL_Event;

namespace EngineN {

// Abstract game actions
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

// Contextual action states
enum class ActionState : u8 {
    Idle = 0,
    JustPressed,
    Held,
    JustReleased
};

// Sub-frame timestamped action event
struct TimestampedAction {
    PlayerAction action = PlayerAction::Jump;
    u64 timestamp = 0;   // SDL event timestamp in nanoseconds
    bool pressed = true;
};

inline constexpr usize kMaxTimestampedActions = 64;
inline constexpr usize kMaxKeyboardKeys       = 512;
inline constexpr usize kMaxMouseButtons       = 8;

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

enum class AxisId : u8 {
    MoveX = 0,
    MoveY,
    AimX,
    AimY,
    Count
};

class InputSystem {
public:
    InputSystem();
    ~InputSystem() = default;

    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    void BeginFrame();
    void ProcessEvent(const SDL_Event& event);
    void EndFrame();

    // Continuous Keyboard Polling
    [[nodiscard]] bool IsKeyDown(u32 scancode) const noexcept;
    [[nodiscard]] bool IsKeyUp(u32 scancode) const noexcept;

    // Continuous Mouse Polling
    [[nodiscard]] bool IsMouseButtonDown(u8 button) const noexcept;
    [[nodiscard]] i32 GetMouseX() const noexcept { return m_mouseX; }
    [[nodiscard]] i32 GetMouseY() const noexcept { return m_mouseY; }
    [[nodiscard]] i32 GetMouseDeltaX() const noexcept { return m_mouseDeltaX; }
    [[nodiscard]] i32 GetMouseDeltaY() const noexcept { return m_mouseDeltaY; }
    [[nodiscard]] f32 GetMouseWheelX() const noexcept { return m_mouseWheelX; }
    [[nodiscard]] f32 GetMouseWheelY() const noexcept { return m_mouseWheelY; }

    // Gamepad Queries
    [[nodiscard]] f32 GetRawAxis(AxisId axis) const noexcept;
    [[nodiscard]] bool HasGamepad() const noexcept { return m_gamepad.IsConnected(); }
    [[nodiscard]] const Gamepad& GetGamepad() const noexcept { return m_gamepad; }
    [[nodiscard]] Gamepad& GetGamepad() noexcept { return m_gamepad; }

    // Virtual Action State Queries
    [[nodiscard]] ActionState GetActionState(PlayerAction action) const noexcept;
    [[nodiscard]] bool IsActionJustPressed(PlayerAction action) const noexcept;
    [[nodiscard]] bool IsActionHeld(PlayerAction action) const noexcept;
    [[nodiscard]] bool IsActionJustReleased(PlayerAction action) const noexcept;

    // Virtual Axis Queries
    [[nodiscard]] f32 GetAxis(AxisId axis) const noexcept;

    // Timestamped Action Buffer Queries
    [[nodiscard]] u32 GetTimestampedActionCount() const noexcept { return m_tsActionCount; }
    [[nodiscard]] const TimestampedAction& GetTimestampedAction(u32 index) const noexcept;

    // Bindings
    void SetDefaultBindings();
    void BindKey(u32 scancode, PlayerAction action);
    void BindMouseButton(u8 button, PlayerAction action);
    void BindGamepadButton(u8 button, PlayerAction action);

    // Deadzone
    void SetStickDeadzone(f32 deadzone) noexcept { m_stickDeadzone = deadzone; }
    [[nodiscard]] f32 GetStickDeadzone() const noexcept { return m_stickDeadzone; }

private:
    void ProcessKeyEvent(const SDL_Event& event);
    void ProcessMouseButtonEvent(const SDL_Event& event);
    void ProcessMouseMotionEvent(const SDL_Event& event);
    void ProcessMouseWheelEvent(const SDL_Event& event);
    void ProcessGamepadButtonEvent(const SDL_Event& event);
    void ProcessGamepadAxisEvent(const SDL_Event& event);
    void ProcessGamepadDeviceEvent(const SDL_Event& event);

    void PushTimestampedAction(PlayerAction action, u64 timestamp, bool pressed);
    void UpdateActionFromPhysical(PlayerAction action, bool pressed, u64 timestamp);

    std::array<bool, kMaxKeyboardKeys> m_keyState{};
    std::array<bool, kMaxMouseButtons> m_mouseButtonState{};
    i32 m_mouseX = 0;
    i32 m_mouseY = 0;
    i32 m_mouseDeltaX = 0;
    i32 m_mouseDeltaY = 0;
    f32 m_mouseWheelX = 0.0f;
    f32 m_mouseWheelY = 0.0f;

    Gamepad m_gamepad;
    f32 m_stickDeadzone = 0.22f;

    static constexpr usize kActionCount = static_cast<usize>(PlayerAction::Count);
    std::array<ActionState, kActionCount> m_actionStates{};

    static constexpr usize kMaxKeyBindings = 32;
    static constexpr usize kMaxMouseBindings = 8;
    static constexpr usize kMaxGamepadBindings = 16;

    std::array<KeyBinding, kMaxKeyBindings> m_keyBindings{};
    std::array<MouseButtonBinding, kMaxMouseBindings> m_mouseBindings{};
    std::array<GamepadButtonBinding, kMaxGamepadBindings> m_gamepadBindings{};
    u32 m_keyBindingCount = 0;
    u32 m_mouseBindingCount = 0;
    u32 m_gamepadBindingCount = 0;

    std::array<TimestampedAction, kMaxTimestampedActions> m_tsActions{};
    u32 m_tsActionCount = 0;

    // Track active physical inputs holding each action
    std::array<u8, kActionCount> m_actionHeldInputsCount{};
};

} // namespace EngineN
