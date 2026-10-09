#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>

namespace EngineN {

/**
 * InputBindings: Clean registry mapping Keyboard & Mouse inputs to logical actions and axes.
 * Supports context isolation (Gameplay vs UI) and zero heap allocations.
 */
class InputBindings {
public:
    InputBindings();

    void SetDefaultBindings() noexcept;
    void Clear() noexcept;

    [[nodiscard]] bool BindKey(
        u32 scancode,
        InputAction action,
        InputContext context = InputContext::Gameplay) noexcept;
    [[nodiscard]] bool UnbindKey(
        u32 scancode,
        InputAction action,
        InputContext context = InputContext::Gameplay) noexcept;

    [[nodiscard]] bool BindMouseButton(
        u8 button,
        InputAction action,
        InputContext context = InputContext::Gameplay) noexcept;
    [[nodiscard]] bool UnbindMouseButton(
        u8 button,
        InputAction action,
        InputContext context = InputContext::Gameplay) noexcept;

    [[nodiscard]] bool BindAxisKeys(
        AxisId axis,
        u32 negativeScancode,
        u32 positiveScancode,
        InputContext context = InputContext::Gameplay) noexcept;
    [[nodiscard]] bool UnbindAxisKeys(
        AxisId axis,
        u32 negativeScancode,
        u32 positiveScancode,
        InputContext context = InputContext::Gameplay) noexcept;

    [[nodiscard]] bool IsActionSourceDown(
        InputAction action,
        InputContext context,
        const std::array<bool, kMaxKeyboardKeys>& keysDown,
        const std::array<bool, kMaxMouseButtons>& mouseButtonsDown) const noexcept;

    [[nodiscard]] f32 EvaluateAxisKeys(
        AxisId axis,
        InputContext context,
        const std::array<bool, kMaxKeyboardKeys>& keysDown) const noexcept;

    template <typename F>
    void ForEachKeyAction(u32 scancode, InputContext context, F&& func) const noexcept {
        for (usize i = 0; i < m_keyBindingCount; ++i) {
            if (m_keyBindings[i].scancode == scancode && m_keyBindings[i].context == context) {
                func(m_keyBindings[i].action);
            }
        }
    }

    template <typename F>
    void ForEachMouseButtonAction(u8 button, InputContext context, F&& func) const noexcept {
        for (usize i = 0; i < m_mouseBindingCount; ++i) {
            if (m_mouseBindings[i].button == button && m_mouseBindings[i].context == context) {
                func(m_mouseBindings[i].action);
            }
        }
    }

private:
    std::array<KeyBinding, kMaxKeyBindings> m_keyBindings{};
    usize m_keyBindingCount = 0;

    std::array<MouseButtonBinding, kMaxMouseBindings> m_mouseBindings{};
    usize m_mouseBindingCount = 0;

    std::array<AxisKeyBinding, kMaxAxisBindings> m_axisBindings{};
    usize m_axisBindingCount = 0;
};

} // namespace EngineN
