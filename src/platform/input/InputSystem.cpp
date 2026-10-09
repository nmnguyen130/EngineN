#include "platform/input/InputSystem.h"
#include "core/Assert.h"
#include <SDL3/SDL.h>

namespace EngineN {

InputSystem::InputSystem() {
    SetDefaultBindings();
}

void InputSystem::BeginFrame() {
    // Advance action state machine:
    // JustPressed -> Held
    // JustReleased -> Idle
    for (usize i = 0; i < kActionCount; ++i) {
        if (m_actionStates[i] == ActionState::JustPressed) {
            m_actionStates[i] = ActionState::Held;
        } else if (m_actionStates[i] == ActionState::JustReleased) {
            m_actionStates[i] = ActionState::Idle;
        }
    }

    // Reset relative mouse deltas and wheel
    m_mouseDeltaX = 0;
    m_mouseDeltaY = 0;
    m_mouseWheelX = 0.0f;
    m_mouseWheelY = 0.0f;

    // Reset per-frame timestamped queue
    m_tsActionCount = 0;
}

void InputSystem::EndFrame() {
    // Reserved for future frame finalization
}

void InputSystem::ProcessEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            ProcessKeyEvent(event);
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            ProcessMouseButtonEvent(event);
            break;

        case SDL_EVENT_MOUSE_MOTION:
            ProcessMouseMotionEvent(event);
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            ProcessMouseWheelEvent(event);
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            ProcessGamepadButtonEvent(event);
            break;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            ProcessGamepadAxisEvent(event);
            break;

        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            ProcessGamepadDeviceEvent(event);
            break;

        default:
            break;
    }
}

void InputSystem::ProcessKeyEvent(const SDL_Event& event) {
    u32 scancode = static_cast<u32>(event.key.scancode);
    if (scancode >= kMaxKeyboardKeys) return;

    bool pressed = (event.type == SDL_EVENT_KEY_DOWN);
    m_keyState[scancode] = pressed;

    // Filter out OS key repeat for discrete actions
    if (event.key.repeat != 0) {
        return;
    }

    u64 timestamp = event.key.timestamp;

    for (u32 i = 0; i < m_keyBindingCount; ++i) {
        if (m_keyBindings[i].scancode == scancode) {
            UpdateActionFromPhysical(m_keyBindings[i].action, pressed, timestamp);
        }
    }
}

void InputSystem::ProcessMouseButtonEvent(const SDL_Event& event) {
    u8 button = event.button.button;
    if (button >= kMaxMouseButtons) return;

    bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    m_mouseButtonState[button] = pressed;
    u64 timestamp = event.button.timestamp;

    for (u32 i = 0; i < m_mouseBindingCount; ++i) {
        if (m_mouseBindings[i].button == button) {
            UpdateActionFromPhysical(m_mouseBindings[i].action, pressed, timestamp);
        }
    }
}

void InputSystem::ProcessMouseMotionEvent(const SDL_Event& event) {
    m_mouseX = static_cast<i32>(event.motion.x);
    m_mouseY = static_cast<i32>(event.motion.y);
    m_mouseDeltaX += static_cast<i32>(event.motion.xrel);
    m_mouseDeltaY += static_cast<i32>(event.motion.yrel);
}

void InputSystem::ProcessMouseWheelEvent(const SDL_Event& event) {
    m_mouseWheelX += event.wheel.x;
    m_mouseWheelY += event.wheel.y;
}

void InputSystem::ProcessGamepadButtonEvent(const SDL_Event& event) {
    u8 button = event.gbutton.button;
    bool pressed = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    u64 timestamp = event.gbutton.timestamp;

    for (u32 i = 0; i < m_gamepadBindingCount; ++i) {
        if (m_gamepadBindings[i].button == button) {
            UpdateActionFromPhysical(m_gamepadBindings[i].action, pressed, timestamp);
        }
    }
}

void InputSystem::ProcessGamepadAxisEvent(const SDL_Event& /*event*/) {
    // Axis queries poll continuously on demand with deadzone math
}

void InputSystem::ProcessGamepadDeviceEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        if (!m_gamepad.IsConnected()) {
            m_gamepad.Open(event.gdevice.which);
        }
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        if (m_gamepad.IsConnected() && m_gamepad.GetInstanceId() == event.gdevice.which) {
            m_gamepad.Close();
        }
    }
}

void InputSystem::PushTimestampedAction(PlayerAction action, u64 timestamp, bool pressed) {
    if (m_tsActionCount < kMaxTimestampedActions) {
        m_tsActions[m_tsActionCount++] = TimestampedAction{
            .action = action,
            .timestamp = timestamp,
            .pressed = pressed
        };
    }
}

void InputSystem::UpdateActionFromPhysical(PlayerAction action, bool pressed, u64 timestamp) {
    usize idx = static_cast<usize>(action);
    if (idx >= kActionCount) return;

    if (pressed) {
        m_actionHeldInputsCount[idx]++;
        if (m_actionHeldInputsCount[idx] == 1) {
            // First physical input pressed for this action
            m_actionStates[idx] = ActionState::JustPressed;
            PushTimestampedAction(action, timestamp, true);
        }
    } else {
        if (m_actionHeldInputsCount[idx] > 0) {
            m_actionHeldInputsCount[idx]--;
        }
        if (m_actionHeldInputsCount[idx] == 0) {
            // All physical inputs for this action released
            m_actionStates[idx] = ActionState::JustReleased;
            PushTimestampedAction(action, timestamp, false);
        }
    }
}

bool InputSystem::IsKeyDown(u32 scancode) const noexcept {
    if (scancode >= kMaxKeyboardKeys) return false;
    return m_keyState[scancode];
}

bool InputSystem::IsKeyUp(u32 scancode) const noexcept {
    if (scancode >= kMaxKeyboardKeys) return true;
    return !m_keyState[scancode];
}

bool InputSystem::IsMouseButtonDown(u8 button) const noexcept {
    if (button >= kMaxMouseButtons) return false;
    return m_mouseButtonState[button];
}

f32 InputSystem::GetRawAxis(AxisId axis) const noexcept {
    if (!m_gamepad.IsConnected()) return 0.0f;
    switch (axis) {
        case AxisId::MoveX: return m_gamepad.GetLeftStick(m_stickDeadzone).x;
        case AxisId::MoveY: return m_gamepad.GetLeftStick(m_stickDeadzone).y;
        case AxisId::AimX:  return m_gamepad.GetRightStick(m_stickDeadzone).x;
        case AxisId::AimY:  return m_gamepad.GetRightStick(m_stickDeadzone).y;
        default: return 0.0f;
    }
}

ActionState InputSystem::GetActionState(PlayerAction action) const noexcept {
    usize idx = static_cast<usize>(action);
    if (idx >= kActionCount) return ActionState::Idle;
    return m_actionStates[idx];
}

bool InputSystem::IsActionJustPressed(PlayerAction action) const noexcept {
    return GetActionState(action) == ActionState::JustPressed;
}

bool InputSystem::IsActionHeld(PlayerAction action) const noexcept {
    ActionState s = GetActionState(action);
    return s == ActionState::Held || s == ActionState::JustPressed;
}

bool InputSystem::IsActionJustReleased(PlayerAction action) const noexcept {
    return GetActionState(action) == ActionState::JustReleased;
}

f32 InputSystem::GetAxis(AxisId axis) const noexcept {
    // Check Gamepad analog stick first
    f32 gamepadVal = GetRawAxis(axis);
    if (std::abs(gamepadVal) > 1e-4f) {
        return gamepadVal;
    }

    // Keyboard digital fallback
    f32 keyboardVal = 0.0f;
    if (axis == AxisId::MoveX) {
        if (IsKeyDown(SDL_SCANCODE_D) || IsKeyDown(SDL_SCANCODE_RIGHT)) keyboardVal += 1.0f;
        if (IsKeyDown(SDL_SCANCODE_A) || IsKeyDown(SDL_SCANCODE_LEFT))  keyboardVal -= 1.0f;
    } else if (axis == AxisId::MoveY) {
        if (IsKeyDown(SDL_SCANCODE_S) || IsKeyDown(SDL_SCANCODE_DOWN))  keyboardVal += 1.0f;
        if (IsKeyDown(SDL_SCANCODE_W) || IsKeyDown(SDL_SCANCODE_UP))    keyboardVal -= 1.0f;
    }

    return keyboardVal;
}

const TimestampedAction& InputSystem::GetTimestampedAction(u32 index) const noexcept {
    ENGINE_ASSERT(index < m_tsActionCount, "Index out of range for TimestampedAction buffer");
    return m_tsActions[index];
}

void InputSystem::SetDefaultBindings() {
    m_keyBindingCount = 0;
    m_mouseBindingCount = 0;
    m_gamepadBindingCount = 0;

    // Keyboard Defaults
    BindKey(SDL_SCANCODE_SPACE,  PlayerAction::Jump);
    BindKey(SDL_SCANCODE_W,      PlayerAction::Jump);
    BindKey(SDL_SCANCODE_UP,     PlayerAction::Jump);
    BindKey(SDL_SCANCODE_J,      PlayerAction::Attack);
    BindKey(SDL_SCANCODE_Z,      PlayerAction::Attack);
    BindKey(SDL_SCANCODE_LSHIFT, PlayerAction::Dash);
    BindKey(SDL_SCANCODE_K,      PlayerAction::Dash);
    BindKey(SDL_SCANCODE_L,      PlayerAction::Parry);
    BindKey(SDL_SCANCODE_X,      PlayerAction::Parry);
    BindKey(SDL_SCANCODE_E,      PlayerAction::Interact);
    BindKey(SDL_SCANCODE_RETURN, PlayerAction::MenuConfirm);
    BindKey(SDL_SCANCODE_ESCAPE, PlayerAction::MenuPause);

    // Mouse Defaults
    BindMouseButton(SDL_BUTTON_LEFT,  PlayerAction::Attack);
    BindMouseButton(SDL_BUTTON_RIGHT, PlayerAction::Parry);

    // Gamepad Defaults (Standard layout)
    BindGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH,         PlayerAction::Jump);        // A
    BindGamepadButton(SDL_GAMEPAD_BUTTON_WEST,          PlayerAction::Attack);      // X
    BindGamepadButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,PlayerAction::Dash);        // RB
    BindGamepadButton(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PlayerAction::Parry);       // LB
    BindGamepadButton(SDL_GAMEPAD_BUTTON_NORTH,         PlayerAction::Interact);    // Y
    BindGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH,         PlayerAction::MenuConfirm);
    BindGamepadButton(SDL_GAMEPAD_BUTTON_EAST,          PlayerAction::MenuCancel);  // B
    BindGamepadButton(SDL_GAMEPAD_BUTTON_START,         PlayerAction::MenuPause);
}

void InputSystem::BindKey(u32 scancode, PlayerAction action) {
    if (m_keyBindingCount < kMaxKeyBindings) {
        m_keyBindings[m_keyBindingCount++] = KeyBinding{.scancode = scancode, .action = action};
    }
}

void InputSystem::BindMouseButton(u8 button, PlayerAction action) {
    if (m_mouseBindingCount < kMaxMouseBindings) {
        m_mouseBindings[m_mouseBindingCount++] = MouseButtonBinding{.button = button, .action = action};
    }
}

void InputSystem::BindGamepadButton(u8 button, PlayerAction action) {
    if (m_gamepadBindingCount < kMaxGamepadBindings) {
        m_gamepadBindings[m_gamepadBindingCount++] = GamepadButtonBinding{.button = button, .action = action};
    }
}

} // namespace EngineN
