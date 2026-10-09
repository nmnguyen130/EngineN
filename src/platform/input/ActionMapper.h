#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>

namespace EngineN {

class Keyboard;
class Gamepad;

/// Mid-Level Action Mapping & Rebinding Subsystem.
/// Decouples physical hardware inputs from abstract gameplay actions (PlayerAction)
/// and virtual axes (AxisId). Supports multi-input reference counting (e.g. multiple keys
/// bound to the same action) and runtime rebinding. Zero heap allocations.
class ActionMapper {
public:
    ActionMapper();
    ~ActionMapper() = default;

    /// Advances action state transitions for the new frame:
    /// JustPressed -> Held, JustReleased -> Idle.
    void BeginFrame() noexcept;

    // --- Action State Queries ---
    [[nodiscard]] ActionState GetActionState(PlayerAction action) const noexcept;
    [[nodiscard]] bool IsActionJustPressed(PlayerAction action) const noexcept;
    [[nodiscard]] bool IsActionHeld(PlayerAction action) const noexcept;
    [[nodiscard]] bool IsActionJustReleased(PlayerAction action) const noexcept;

    // --- Virtual Axis Evaluation ---
    /// Evaluates composite 2D axis: polls Gamepad analog stick first,
    /// falling back to digital Keyboard keys if stick is in deadzone.
    [[nodiscard]] f32 EvaluateAxis(AxisId axis, const Gamepad& gamepad, const Keyboard& keyboard, f32 deadzone) const noexcept;

    // --- Rebinding API ---
    void SetDefaultBindings() noexcept;
    void ClearBindings() noexcept;

    void BindKey(u32 scancode, PlayerAction action) noexcept;
    void BindMouseButton(u8 button, PlayerAction action) noexcept;
    void BindGamepadButton(u8 button, PlayerAction action) noexcept;

    void UnbindKey(u32 scancode) noexcept;
    void UnbindMouseButton(u8 button) noexcept;
    void UnbindGamepadButton(u8 button) noexcept;

    // --- Physical Hardware Event Dispatchers ---
    // Callback signature: void(PlayerAction action, bool pressed, u64 timestampNs)
    template <typename Callback>
    void OnKeyEvent(u32 scancode, bool pressed, u64 timestampNs, Callback&& onTransition) noexcept {
        for (u32 i = 0; i < m_keyBindingCount; ++i) {
            if (m_keyBindings[i].scancode == scancode) {
                UpdateActionFromPhysical(m_keyBindings[i].action, pressed, timestampNs, onTransition);
            }
        }
    }

    template <typename Callback>
    void OnMouseButtonEvent(u8 button, bool pressed, u64 timestampNs, Callback&& onTransition) noexcept {
        for (u32 i = 0; i < m_mouseBindingCount; ++i) {
            if (m_mouseBindings[i].button == button) {
                UpdateActionFromPhysical(m_mouseBindings[i].action, pressed, timestampNs, onTransition);
            }
        }
    }

    template <typename Callback>
    void OnGamepadButtonEvent(u8 button, bool pressed, u64 timestampNs, Callback&& onTransition) noexcept {
        for (u32 i = 0; i < m_gamepadBindingCount; ++i) {
            if (m_gamepadBindings[i].button == button) {
                UpdateActionFromPhysical(m_gamepadBindings[i].action, pressed, timestampNs, onTransition);
            }
        }
    }

    /// Resets all action state machines and held input counters to Idle.
    void Reset() noexcept;

private:
    template <typename Callback>
    void UpdateActionFromPhysical(PlayerAction action, bool pressed, u64 timestampNs, Callback&& onTransition) noexcept {
        usize idx = static_cast<usize>(action);
        if (idx >= kActionCount) return;

        if (pressed) {
            m_actionHeldInputsCount[idx]++;
            // Only trigger JustPressed if this is the FIRST physical input held for this action
            if (m_actionHeldInputsCount[idx] == 1) {
                m_actionStates[idx] = ActionState::JustPressed;
                onTransition(action, true, timestampNs);
            }
        } else {
            if (m_actionHeldInputsCount[idx] > 0) {
                m_actionHeldInputsCount[idx]--;
            }
            // Only trigger JustReleased when ALL physical inputs bound to this action are released
            if (m_actionHeldInputsCount[idx] == 0) {
                m_actionStates[idx] = ActionState::JustReleased;
                onTransition(action, false, timestampNs);
            }
        }
    }

    std::array<ActionState, kActionCount> m_actionStates{};
    std::array<u8, kActionCount> m_actionHeldInputsCount{};

    std::array<KeyBinding, kMaxKeyBindings> m_keyBindings{};
    std::array<MouseButtonBinding, kMaxMouseBindings> m_mouseBindings{};
    std::array<GamepadButtonBinding, kMaxGamepadBindings> m_gamepadBindings{};

    u32 m_keyBindingCount = 0;
    u32 m_mouseBindingCount = 0;
    u32 m_gamepadBindingCount = 0;
};

} // namespace EngineN
