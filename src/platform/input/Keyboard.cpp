#include "platform/input/Keyboard.h"
#include <SDL3/SDL.h>

namespace EngineN {

void Keyboard::BeginFrame() noexcept {
    m_previousState = m_currentState;
}

bool Keyboard::ProcessEvent(const SDL_KeyboardEvent& event) noexcept {
    u32 scancode = static_cast<u32>(event.scancode);
    if (scancode >= kMaxKeyboardKeys) {
        return false;
    }

    // Filter out OS autorepeat events so only genuine physical edge presses register
    if (event.repeat) {
        return false;
    }

    bool pressed = (event.type == SDL_EVENT_KEY_DOWN);
    m_currentState[scancode] = pressed;
    return true;
}

bool Keyboard::IsDown(u32 scancode) const noexcept {
    if (scancode >= kMaxKeyboardKeys) return false;
    return m_currentState[scancode];
}

bool Keyboard::IsUp(u32 scancode) const noexcept {
    if (scancode >= kMaxKeyboardKeys) return true;
    return !m_currentState[scancode];
}

bool Keyboard::IsJustPressed(u32 scancode) const noexcept {
    if (scancode >= kMaxKeyboardKeys) return false;
    return m_currentState[scancode] && !m_previousState[scancode];
}

bool Keyboard::IsJustReleased(u32 scancode) const noexcept {
    if (scancode >= kMaxKeyboardKeys) return false;
    return !m_currentState[scancode] && m_previousState[scancode];
}

void Keyboard::Reset() noexcept {
    m_currentState.fill(false);
    m_previousState.fill(false);
}

} // namespace EngineN
