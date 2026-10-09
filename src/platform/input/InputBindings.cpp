#include "platform/input/InputBindings.h"
#include <SDL3/SDL.h>

namespace EngineN {

InputBindings::InputBindings() {
    SetDefaultBindings();
}

void InputBindings::SetDefaultBindings() noexcept {
    Clear();

    // Locomotion Axis: Clean WASD only (No arrow keys)
    (void)BindAxisKeys(AxisId::MoveX, SDL_SCANCODE_A, SDL_SCANCODE_D);
    (void)BindAxisKeys(AxisId::MoveY, SDL_SCANCODE_W, SDL_SCANCODE_S);

    // Gameplay Actions: Space = Jump, J = Attack, K = Dash, L = Parry, E = Interact (No Z/X)
    (void)BindKey(SDL_SCANCODE_SPACE,  InputAction::Jump);
    (void)BindKey(SDL_SCANCODE_J,      InputAction::Attack);
    (void)BindKey(SDL_SCANCODE_K,      InputAction::Dash);
    (void)BindKey(SDL_SCANCODE_L,      InputAction::Parry);
    (void)BindKey(SDL_SCANCODE_LSHIFT, InputAction::Dash);
    (void)BindKey(SDL_SCANCODE_E,      InputAction::Interact);
    (void)BindKey(SDL_SCANCODE_ESCAPE, InputAction::MenuPause);

    // Mouse Controls
    (void)BindMouseButton(SDL_BUTTON_LEFT,  InputAction::Attack);
    (void)BindMouseButton(SDL_BUTTON_RIGHT, InputAction::Parry);

    // UI Context Navigation
    (void)BindKey(SDL_SCANCODE_RETURN,    InputAction::MenuConfirm, InputContext::UI);
    (void)BindKey(SDL_SCANCODE_SPACE,     InputAction::MenuConfirm, InputContext::UI);
    (void)BindKey(SDL_SCANCODE_ESCAPE,    InputAction::MenuCancel,  InputContext::UI);
    (void)BindKey(SDL_SCANCODE_BACKSPACE, InputAction::MenuCancel,  InputContext::UI);
    (void)BindAxisKeys(AxisId::MoveX, SDL_SCANCODE_A, SDL_SCANCODE_D, InputContext::UI);
    (void)BindAxisKeys(AxisId::MoveY, SDL_SCANCODE_W, SDL_SCANCODE_S, InputContext::UI);
}

void InputBindings::Clear() noexcept {
    m_keyBindingCount = 0;
    m_mouseBindingCount = 0;
    m_axisBindingCount = 0;
}

bool InputBindings::BindKey(u32 scancode, InputAction action, InputContext context) noexcept {
    if (scancode >= kMaxKeyboardKeys || m_keyBindingCount >= kMaxKeyBindings) return false;
    for (usize i = 0; i < m_keyBindingCount; ++i) {
        if (m_keyBindings[i].scancode == scancode &&
            m_keyBindings[i].action == action &&
            m_keyBindings[i].context == context) {
            return false;
        }
    }
    m_keyBindings[m_keyBindingCount++] = KeyBinding{scancode, action, context};
    return true;
}

bool InputBindings::UnbindKey(u32 scancode, InputAction action, InputContext context) noexcept {
    for (usize i = 0; i < m_keyBindingCount; ++i) {
        if (m_keyBindings[i].scancode == scancode &&
            m_keyBindings[i].action == action &&
            m_keyBindings[i].context == context) {
            m_keyBindings[i] = m_keyBindings[--m_keyBindingCount];
            return true;
        }
    }
    return false;
}

bool InputBindings::BindMouseButton(u8 button, InputAction action, InputContext context) noexcept {
    if (button >= kMaxMouseButtons || m_mouseBindingCount >= kMaxMouseBindings) return false;
    for (usize i = 0; i < m_mouseBindingCount; ++i) {
        if (m_mouseBindings[i].button == button &&
            m_mouseBindings[i].action == action &&
            m_mouseBindings[i].context == context) {
            return false;
        }
    }
    m_mouseBindings[m_mouseBindingCount++] = MouseButtonBinding{button, action, context};
    return true;
}

bool InputBindings::UnbindMouseButton(u8 button, InputAction action, InputContext context) noexcept {
    for (usize i = 0; i < m_mouseBindingCount; ++i) {
        if (m_mouseBindings[i].button == button &&
            m_mouseBindings[i].action == action &&
            m_mouseBindings[i].context == context) {
            m_mouseBindings[i] = m_mouseBindings[--m_mouseBindingCount];
            return true;
        }
    }
    return false;
}

bool InputBindings::BindAxisKeys(AxisId axis, u32 negativeScancode, u32 positiveScancode, InputContext context) noexcept {
    if (m_axisBindingCount >= kMaxAxisBindings) return false;
    for (usize i = 0; i < m_axisBindingCount; ++i) {
        if (m_axisBindings[i].axis == axis &&
            m_axisBindings[i].negativeScancode == negativeScancode &&
            m_axisBindings[i].positiveScancode == positiveScancode &&
            m_axisBindings[i].context == context) {
            return false;
        }
    }
    m_axisBindings[m_axisBindingCount++] = AxisKeyBinding{axis, negativeScancode, positiveScancode, context};
    return true;
}

bool InputBindings::UnbindAxisKeys(AxisId axis, u32 negativeScancode, u32 positiveScancode, InputContext context) noexcept {
    for (usize i = 0; i < m_axisBindingCount; ++i) {
        if (m_axisBindings[i].axis == axis &&
            m_axisBindings[i].negativeScancode == negativeScancode &&
            m_axisBindings[i].positiveScancode == positiveScancode &&
            m_axisBindings[i].context == context) {
            m_axisBindings[i] = m_axisBindings[--m_axisBindingCount];
            return true;
        }
    }
    return false;
}

bool InputBindings::IsActionSourceDown(
    InputAction action,
    InputContext context,
    const std::array<bool, kMaxKeyboardKeys>& keysDown,
    const std::array<bool, kMaxMouseButtons>& mouseButtonsDown) const noexcept {
    for (usize i = 0; i < m_keyBindingCount; ++i) {
        if (m_keyBindings[i].action == action && m_keyBindings[i].context == context) {
            if (keysDown[m_keyBindings[i].scancode]) return true;
        }
    }
    for (usize i = 0; i < m_mouseBindingCount; ++i) {
        if (m_mouseBindings[i].action == action && m_mouseBindings[i].context == context) {
            if (mouseButtonsDown[m_mouseBindings[i].button]) return true;
        }
    }
    return false;
}

f32 InputBindings::EvaluateAxisKeys(
    AxisId axis,
    InputContext context,
    const std::array<bool, kMaxKeyboardKeys>& keysDown) const noexcept {
    f32 keyVal = 0.0f;
    for (usize i = 0; i < m_axisBindingCount; ++i) {
        if (m_axisBindings[i].axis == axis && m_axisBindings[i].context == context) {
            if (keysDown[m_axisBindings[i].positiveScancode]) keyVal += 1.0f;
            if (keysDown[m_axisBindings[i].negativeScancode]) keyVal -= 1.0f;
        }
    }
    return std::clamp(keyVal, -1.0f, 1.0f);
}

} // namespace EngineN
