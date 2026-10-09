#include "platform/input/InputSystem.h"
#include "core/Assert.h"
#include <SDL3/SDL.h>

namespace EngineN {

InputSystem::InputSystem() {
    SetDefaultBindings();
}

void InputSystem::BeginFrame() noexcept {
    m_keysPressedFrame.fill(false);
    m_keysReleasedFrame.fill(false);
    m_mousePressedFrame.fill(false);
    m_mouseReleasedFrame.fill(false);

    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
    m_mouseWheelX = 0.0f;
    m_mouseWheelY = 0.0f;

    for (usize i = 0; i < kActionCount; ++i) {
        m_frameStates[i].pressed = false;
        m_frameStates[i].released = false;
    }

    m_frameLogCount = 0;
}

void InputSystem::BeginFixedTick() noexcept {
    for (usize i = 0; i < kActionCount; ++i) {
        m_tickStates[i] = ActionFrameState{
            .down = m_frameStates[i].down,
            .pressed = m_pendingPressed[i],
            .released = m_pendingReleased[i]
        };
        m_pendingPressed[i] = false;
        m_pendingReleased[i] = false;
    }
}

void InputSystem::ProcessEvent(const SDL_Event& event) noexcept {
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            if (event.key.repeat) return;
            u32 scancode = static_cast<u32>(event.key.scancode);
            if (scancode >= kMaxKeyboardKeys) return;

            bool pressed = (event.type == SDL_EVENT_KEY_DOWN);
            if (m_keysDown[scancode] == pressed) return;

            m_keysDown[scancode] = pressed;
            if (pressed) {
                m_keysPressedFrame[scancode] = true;
            } else {
                m_keysReleasedFrame[scancode] = true;
            }

            RecomputeActionStates(event.key.timestamp);
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            u8 button = event.button.button;
            if (button >= kMaxMouseButtons) return;

            m_mouseX = event.button.x;
            m_mouseY = event.button.y;

            bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            if (m_mouseButtonsDown[button] == pressed) return;

            m_mouseButtonsDown[button] = pressed;
            if (pressed) {
                m_mousePressedFrame[button] = true;
            } else {
                m_mouseReleasedFrame[button] = true;
            }

            RecomputeActionStates(event.button.timestamp);
            break;
        }

        case SDL_EVENT_MOUSE_MOTION: {
            m_mouseX = event.motion.x;
            m_mouseY = event.motion.y;
            m_mouseDeltaX += event.motion.xrel;
            m_mouseDeltaY += event.motion.yrel;
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL: {
            f32 dir = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;
            m_mouseWheelX += event.wheel.x * dir;
            m_mouseWheelY += event.wheel.y * dir;
            break;
        }

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            ReleaseKeyboardAndMouse(event.window.timestamp);
            break;

        default:
            break;
    }
}

bool InputSystem::IsActionDown(InputAction action) const noexcept {
    usize idx = static_cast<usize>(action);
    return (idx < kActionCount) ? m_frameStates[idx].down : false;
}

bool InputSystem::WasActionPressedThisFrame(InputAction action) const noexcept {
    usize idx = static_cast<usize>(action);
    return (idx < kActionCount) ? m_frameStates[idx].pressed : false;
}

bool InputSystem::WasActionReleasedThisFrame(InputAction action) const noexcept {
    usize idx = static_cast<usize>(action);
    return (idx < kActionCount) ? m_frameStates[idx].released : false;
}

bool InputSystem::WasActionPressedThisTick(InputAction action) const noexcept {
    usize idx = static_cast<usize>(action);
    return (idx < kActionCount) ? m_tickStates[idx].pressed : false;
}

bool InputSystem::WasActionReleasedThisTick(InputAction action) const noexcept {
    usize idx = static_cast<usize>(action);
    return (idx < kActionCount) ? m_tickStates[idx].released : false;
}

const ActionFrameState& InputSystem::GetActionFrameState(InputAction action) const noexcept {
    static constexpr ActionFrameState kEmpty{};
    usize idx = static_cast<usize>(action);
    return (idx < kActionCount) ? m_frameStates[idx] : kEmpty;
}

const ActionFrameState& InputSystem::GetActionTickState(InputAction action) const noexcept {
    static constexpr ActionFrameState kEmpty{};
    usize idx = static_cast<usize>(action);
    return (idx < kActionCount) ? m_tickStates[idx] : kEmpty;
}

f32 InputSystem::GetAxis(AxisId axis) const noexcept {
    return m_bindings.EvaluateAxisKeys(axis, m_activeContext, m_keysDown);
}

bool InputSystem::IsKeyDown(u32 scancode) const noexcept {
    return (scancode < kMaxKeyboardKeys) ? m_keysDown[scancode] : false;
}

bool InputSystem::IsKeyUp(u32 scancode) const noexcept {
    return !IsKeyDown(scancode);
}

bool InputSystem::IsKeyJustPressed(u32 scancode) const noexcept {
    return (scancode < kMaxKeyboardKeys) ? m_keysPressedFrame[scancode] : false;
}

bool InputSystem::IsKeyJustReleased(u32 scancode) const noexcept {
    return (scancode < kMaxKeyboardKeys) ? m_keysReleasedFrame[scancode] : false;
}

bool InputSystem::IsMouseButtonDown(u8 button) const noexcept {
    return (button < kMaxMouseButtons) ? m_mouseButtonsDown[button] : false;
}

bool InputSystem::IsMouseButtonJustPressed(u8 button) const noexcept {
    return (button < kMaxMouseButtons) ? m_mousePressedFrame[button] : false;
}

bool InputSystem::IsMouseButtonJustReleased(u8 button) const noexcept {
    return (button < kMaxMouseButtons) ? m_mouseReleasedFrame[button] : false;
}

f32 InputSystem::GetMouseX() const noexcept { return m_mouseX; }
f32 InputSystem::GetMouseY() const noexcept { return m_mouseY; }
f32 InputSystem::GetMouseDeltaX() const noexcept { return m_mouseDeltaX; }
f32 InputSystem::GetMouseDeltaY() const noexcept { return m_mouseDeltaY; }
f32 InputSystem::GetMouseWheelX() const noexcept { return m_mouseWheelX; }
f32 InputSystem::GetMouseWheelY() const noexcept { return m_mouseWheelY; }

bool InputSystem::ConsumeAction(InputAction action, u64 maxWindowNs) noexcept {
    return m_actionBuffer.ConsumeAction(action, maxWindowNs);
}

bool InputSystem::ConsumeAction(InputAction action, u64 currentTimestampNs, u64 maxWindowNs) noexcept {
    return m_actionBuffer.ConsumeAction(action, currentTimestampNs, maxWindowNs);
}

bool InputSystem::HasBufferedAction(InputAction action, u64 maxWindowNs) const noexcept {
    return m_actionBuffer.HasAction(action, maxWindowNs);
}

bool InputSystem::HasBufferedAction(InputAction action, u64 currentTimestampNs, u64 maxWindowNs) const noexcept {
    return m_actionBuffer.HasAction(action, currentTimestampNs, maxWindowNs);
}

void InputSystem::ClearActionBuffer() noexcept {
    m_actionBuffer.Clear();
}

void InputSystem::ClearBufferedAction(InputAction action) noexcept {
    m_actionBuffer.ClearAction(action);
}

usize InputSystem::GetTimestampedActionCount() const noexcept {
    return m_frameLogCount;
}

const TimestampedAction& InputSystem::GetTimestampedAction(usize index) const noexcept {
    static constexpr TimestampedAction kEmpty{};
    ENGINE_ASSERT(index < m_frameLogCount, "Index out of range for TimestampedAction log");
    return (index < m_frameLogCount) ? m_frameLog[index] : kEmpty;
}

bool InputSystem::SetActiveContext(InputContext context) noexcept {
    if (context == m_activeContext) return true;
    m_activeContext = context;

    m_pendingPressed.fill(false);
    m_pendingReleased.fill(false);
    m_actionBuffer.Clear();

    for (usize i = 0; i < kActionCount; ++i) {
        InputAction act = static_cast<InputAction>(i);
        bool down = m_bindings.IsActionSourceDown(act, m_activeContext, m_keysDown, m_mouseButtonsDown);
        m_frameStates[i] = ActionFrameState{.down = down};
        m_tickStates[i] = ActionFrameState{.down = down};
    }
    return true;
}

InputContext InputSystem::GetActiveContext() const noexcept {
    return m_activeContext;
}

void InputSystem::SetDefaultBindings() noexcept {
    m_bindings.SetDefaultBindings();
    RecomputeActionStates(0);
}

void InputSystem::ClearBindings() noexcept {
    m_bindings.Clear();
    RecomputeActionStates(0);
}

bool InputSystem::BindKey(u32 scancode, InputAction action, InputContext context) noexcept {
    bool changed = m_bindings.BindKey(scancode, action, context);
    if (changed) RecomputeActionStates(0);
    return changed;
}

bool InputSystem::UnbindKey(u32 scancode, InputAction action, InputContext context) noexcept {
    bool changed = m_bindings.UnbindKey(scancode, action, context);
    if (changed) RecomputeActionStates(0);
    return changed;
}

bool InputSystem::BindMouseButton(u8 button, InputAction action, InputContext context) noexcept {
    bool changed = m_bindings.BindMouseButton(button, action, context);
    if (changed) RecomputeActionStates(0);
    return changed;
}

bool InputSystem::UnbindMouseButton(u8 button, InputAction action, InputContext context) noexcept {
    bool changed = m_bindings.UnbindMouseButton(button, action, context);
    if (changed) RecomputeActionStates(0);
    return changed;
}

bool InputSystem::BindAxisKeys(AxisId axis, u32 negativeScancode, u32 positiveScancode, InputContext context) noexcept {
    return m_bindings.BindAxisKeys(axis, negativeScancode, positiveScancode, context);
}

bool InputSystem::UnbindAxisKeys(AxisId axis, u32 negativeScancode, u32 positiveScancode, InputContext context) noexcept {
    return m_bindings.UnbindAxisKeys(axis, negativeScancode, positiveScancode, context);
}

void InputSystem::ReleaseKeyboardAndMouse(u64 timestampNs) noexcept {
    for (usize i = 0; i < kMaxKeyboardKeys; ++i) {
        if (m_keysDown[i]) {
            m_keysDown[i] = false;
            m_keysReleasedFrame[i] = true;
        }
    }
    for (usize i = 0; i < kMaxMouseButtons; ++i) {
        if (m_mouseButtonsDown[i]) {
            m_mouseButtonsDown[i] = false;
            m_mouseReleasedFrame[i] = true;
        }
    }
    RecomputeActionStates(timestampNs);
}

void InputSystem::RecomputeActionStates(u64 timestampNs) noexcept {
    for (usize i = 0; i < kActionCount; ++i) {
        InputAction act = static_cast<InputAction>(i);
        bool isDown = m_bindings.IsActionSourceDown(act, m_activeContext, m_keysDown, m_mouseButtonsDown);
        ActionFrameState& state = m_frameStates[i];
        if (state.down == isDown) {
            continue;
        }

        state.down = isDown;
        if (isDown) {
            state.pressed = true;
            m_pendingPressed[i] = true;
            m_actionBuffer.RecordAction(act, timestampNs);
            PushTimestampedAction(act, timestampNs, true);
        } else {
            state.released = true;
            m_pendingReleased[i] = true;
            PushTimestampedAction(act, timestampNs, false);
        }
    }
}

void InputSystem::PushTimestampedAction(InputAction action, u64 timestampNs, bool pressed) noexcept {
    if (m_frameLogCount < kMaxFrameLogActions) {
        m_frameLog[m_frameLogCount++] = TimestampedAction{action, timestampNs, pressed};
    }
}

} // namespace EngineN
