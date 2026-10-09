#include "platform/input/Mouse.h"
#include <SDL3/SDL.h>

namespace EngineN {

void Mouse::BeginFrame() noexcept {
    m_prevButtons = m_buttons;
    m_deltaX = 0;
    m_deltaY = 0;
    m_wheelX = 0.0f;
    m_wheelY = 0.0f;
}

void Mouse::ProcessButtonEvent(const SDL_MouseButtonEvent& event) noexcept {
    u8 button = event.button;
    if (button >= kMaxMouseButtons) {
        return;
    }
    m_buttons[button] = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
}

void Mouse::ProcessMotionEvent(const SDL_MouseMotionEvent& event) noexcept {
    m_x = static_cast<i32>(event.x);
    m_y = static_cast<i32>(event.y);
    m_deltaX += static_cast<i32>(event.xrel);
    m_deltaY += static_cast<i32>(event.yrel);
}

void Mouse::ProcessWheelEvent(const SDL_MouseWheelEvent& event) noexcept {
    m_wheelX += event.x;
    m_wheelY += event.y;
}

bool Mouse::IsButtonDown(u8 button) const noexcept {
    if (button >= kMaxMouseButtons) return false;
    return m_buttons[button];
}

bool Mouse::IsButtonJustPressed(u8 button) const noexcept {
    if (button >= kMaxMouseButtons) return false;
    return m_buttons[button] && !m_prevButtons[button];
}

bool Mouse::IsButtonJustReleased(u8 button) const noexcept {
    if (button >= kMaxMouseButtons) return false;
    return !m_buttons[button] && m_prevButtons[button];
}

void Mouse::Reset() noexcept {
    m_buttons.fill(false);
    m_prevButtons.fill(false);
    m_deltaX = 0;
    m_deltaY = 0;
    m_wheelX = 0.0f;
    m_wheelY = 0.0f;
}

} // namespace EngineN
