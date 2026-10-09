#include "platform/input/ActionMapper.h"
#include "platform/input/Keyboard.h"
#include "platform/input/Gamepad.h"
#include <SDL3/SDL.h>

namespace EngineN {

ActionMapper::ActionMapper() {
    SetDefaultBindings();
}

void ActionMapper::BeginFrame() noexcept {
    for (usize i = 0; i < kActionCount; ++i) {
        if (m_actionStates[i] == ActionState::JustPressed) {
            m_actionStates[i] = ActionState::Held;
        } else if (m_actionStates[i] == ActionState::JustReleased) {
            m_actionStates[i] = ActionState::Idle;
        }
    }
}

ActionState ActionMapper::GetActionState(PlayerAction action) const noexcept {
    usize idx = static_cast<usize>(action);
    if (idx >= kActionCount) return ActionState::Idle;
    return m_actionStates[idx];
}

bool ActionMapper::IsActionJustPressed(PlayerAction action) const noexcept {
    return GetActionState(action) == ActionState::JustPressed;
}

bool ActionMapper::IsActionHeld(PlayerAction action) const noexcept {
    ActionState s = GetActionState(action);
    return s == ActionState::Held || s == ActionState::JustPressed;
}

bool ActionMapper::IsActionJustReleased(PlayerAction action) const noexcept {
    return GetActionState(action) == ActionState::JustReleased;
}

f32 ActionMapper::EvaluateAxis(AxisId axis, const Gamepad& gamepad, const Keyboard& keyboard, f32 deadzone) const noexcept {
    // 1. Check Gamepad analog stick with radial deadzone first
    if (gamepad.IsConnected()) {
        f32 gamepadVal = 0.0f;
        switch (axis) {
            case AxisId::MoveX: gamepadVal = gamepad.GetLeftStick(deadzone).x; break;
            case AxisId::MoveY: gamepadVal = gamepad.GetLeftStick(deadzone).y; break;
            case AxisId::AimX:  gamepadVal = gamepad.GetRightStick(deadzone).x; break;
            case AxisId::AimY:  gamepadVal = gamepad.GetRightStick(deadzone).y; break;
            default: break;
        }

        if (std::abs(gamepadVal) > 1e-4f) {
            return gamepadVal;
        }
    }

    // 2. Keyboard digital fallback
    f32 keyboardVal = 0.0f;
    if (axis == AxisId::MoveX) {
        if (keyboard.IsDown(SDL_SCANCODE_D) || keyboard.IsDown(SDL_SCANCODE_RIGHT)) keyboardVal += 1.0f;
        if (keyboard.IsDown(SDL_SCANCODE_A) || keyboard.IsDown(SDL_SCANCODE_LEFT))  keyboardVal -= 1.0f;
    } else if (axis == AxisId::MoveY) {
        if (keyboard.IsDown(SDL_SCANCODE_S) || keyboard.IsDown(SDL_SCANCODE_DOWN))  keyboardVal += 1.0f;
        if (keyboard.IsDown(SDL_SCANCODE_W) || keyboard.IsDown(SDL_SCANCODE_UP))    keyboardVal -= 1.0f;
    }

    return keyboardVal;
}

void ActionMapper::SetDefaultBindings() noexcept {
    ClearBindings();

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
    BindGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH,          PlayerAction::Jump);        // A
    BindGamepadButton(SDL_GAMEPAD_BUTTON_WEST,           PlayerAction::Attack);      // X
    BindGamepadButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PlayerAction::Dash);        // RB
    BindGamepadButton(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  PlayerAction::Parry);       // LB
    BindGamepadButton(SDL_GAMEPAD_BUTTON_NORTH,          PlayerAction::Interact);    // Y
    BindGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH,          PlayerAction::MenuConfirm);
    BindGamepadButton(SDL_GAMEPAD_BUTTON_EAST,           PlayerAction::MenuCancel);  // B
    BindGamepadButton(SDL_GAMEPAD_BUTTON_START,          PlayerAction::MenuPause);
}

void ActionMapper::ClearBindings() noexcept {
    m_keyBindingCount = 0;
    m_mouseBindingCount = 0;
    m_gamepadBindingCount = 0;
}

void ActionMapper::BindKey(u32 scancode, PlayerAction action) noexcept {
    if (m_keyBindingCount < kMaxKeyBindings) {
        m_keyBindings[m_keyBindingCount++] = KeyBinding{.scancode = scancode, .action = action};
    }
}

void ActionMapper::BindMouseButton(u8 button, PlayerAction action) noexcept {
    if (m_mouseBindingCount < kMaxMouseBindings) {
        m_mouseBindings[m_mouseBindingCount++] = MouseButtonBinding{.button = button, .action = action};
    }
}

void ActionMapper::BindGamepadButton(u8 button, PlayerAction action) noexcept {
    if (m_gamepadBindingCount < kMaxGamepadBindings) {
        m_gamepadBindings[m_gamepadBindingCount++] = GamepadButtonBinding{.button = button, .action = action};
    }
}

void ActionMapper::UnbindKey(u32 scancode) noexcept {
    u32 writeIdx = 0;
    for (u32 i = 0; i < m_keyBindingCount; ++i) {
        if (m_keyBindings[i].scancode != scancode) {
            m_keyBindings[writeIdx++] = m_keyBindings[i];
        }
    }
    m_keyBindingCount = writeIdx;
}

void ActionMapper::UnbindMouseButton(u8 button) noexcept {
    u32 writeIdx = 0;
    for (u32 i = 0; i < m_mouseBindingCount; ++i) {
        if (m_mouseBindings[i].button != button) {
            m_mouseBindings[writeIdx++] = m_mouseBindings[i];
        }
    }
    m_mouseBindingCount = writeIdx;
}

void ActionMapper::UnbindGamepadButton(u8 button) noexcept {
    u32 writeIdx = 0;
    for (u32 i = 0; i < m_gamepadBindingCount; ++i) {
        if (m_gamepadBindings[i].button != button) {
            m_gamepadBindings[writeIdx++] = m_gamepadBindings[i];
        }
    }
    m_gamepadBindingCount = writeIdx;
}

void ActionMapper::Reset() noexcept {
    m_actionStates.fill(ActionState::Idle);
    m_actionHeldInputsCount.fill(0);
}

} // namespace EngineN
