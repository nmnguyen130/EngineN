#include "platform/input/InputSystem.h"
#include "core/Assert.h"
#include <SDL3/SDL.h>

namespace EngineN {

InputSystem::InputSystem() = default;

void InputSystem::BeginFrame() {
    m_keyboard.BeginFrame();
    m_mouse.BeginFrame();
    m_actionMapper.BeginFrame();

    // Reset per-frame sub-frame event log
    m_frameActionCount = 0;
}

void InputSystem::EndFrame() {
    // Reserved for frame finalization
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
            // Continuous polling via SDL_GetGamepadAxis with deadzone
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
    // 1. Update physical keyboard device state (filters out OS key repeats)
    if (!m_keyboard.ProcessEvent(event.key)) {
        return;
    }

    u32 scancode = static_cast<u32>(event.key.scancode);
    bool pressed = (event.type == SDL_EVENT_KEY_DOWN);
    u64 timestamp = event.key.timestamp;

    // 2. Dispatch to ActionMapper; record state transitions
    m_actionMapper.OnKeyEvent(scancode, pressed, timestamp, [this](PlayerAction action, bool isPressed, u64 ts) {
        if (isPressed) {
            m_actionBuffer.RecordAction(action, ts);
        }
        PushFrameTimestampedAction(action, ts, isPressed);
    });
}

void InputSystem::ProcessMouseButtonEvent(const SDL_Event& event) {
    m_mouse.ProcessButtonEvent(event.button);

    u8 button = event.button.button;
    bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    u64 timestamp = event.button.timestamp;

    m_actionMapper.OnMouseButtonEvent(button, pressed, timestamp, [this](PlayerAction action, bool isPressed, u64 ts) {
        if (isPressed) {
            m_actionBuffer.RecordAction(action, ts);
        }
        PushFrameTimestampedAction(action, ts, isPressed);
    });
}

void InputSystem::ProcessMouseMotionEvent(const SDL_Event& event) {
    m_mouse.ProcessMotionEvent(event.motion);
}

void InputSystem::ProcessMouseWheelEvent(const SDL_Event& event) {
    m_mouse.ProcessWheelEvent(event.wheel);
}

void InputSystem::ProcessGamepadButtonEvent(const SDL_Event& event) {
    u8 button = event.gbutton.button;
    bool pressed = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    u64 timestamp = event.gbutton.timestamp;

    m_actionMapper.OnGamepadButtonEvent(button, pressed, timestamp, [this](PlayerAction action, bool isPressed, u64 ts) {
        if (isPressed) {
            m_actionBuffer.RecordAction(action, ts);
        }
        PushFrameTimestampedAction(action, ts, isPressed);
    });
}

void InputSystem::ProcessGamepadDeviceEvent(const SDL_Event& event) {
    m_gamepad.HandleDeviceEvent(event);
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

f32 InputSystem::GetAxis(AxisId axis) const noexcept {
    return m_actionMapper.EvaluateAxis(axis, m_gamepad, m_keyboard, m_stickDeadzone);
}

const TimestampedAction& InputSystem::GetTimestampedAction(u32 index) const noexcept {
    ENGINE_ASSERT(index < m_frameActionCount, "Index out of range for TimestampedAction buffer");
    return m_frameActions[index];
}

void InputSystem::PushFrameTimestampedAction(PlayerAction action, u64 timestamp, bool pressed) {
    if (m_frameActionCount < kMaxTimestampedActions) {
        m_frameActions[m_frameActionCount++] = TimestampedAction{
            .action = action,
            .timestamp = timestamp,
            .pressed = pressed
        };
    }
}

} // namespace EngineN
