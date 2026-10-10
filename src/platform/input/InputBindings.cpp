#include "platform/input/InputBindings.h"
#include <SDL3/SDL_scancode.h>

namespace EngineN {

Result<void, InputError> InputBindings::BindAction(ContextId ctx, Source src, ActionId action) noexcept {
    if (!src.IsValid() || action >= kMaxActions) {
        return Err(InputError::InvalidSource);
    }

    // Check duplicate binding
    for (usize i = 0; i < m_actionBindingCount; ++i) {
        const auto& b = m_actionBindings[i];
        if (b.context == ctx && b.source == src && b.action == action) {
            return Err(InputError::Duplicate);
        }
    }

    if (m_actionBindingCount >= kMaxActionBindings) {
        return Err(InputError::TableFull);
    }

    m_actionBindings[m_actionBindingCount++] = ActionBinding{
        .context = ctx,
        .source = src,
        .action = action
    };
    return Ok;
}

Result<void, InputError> InputBindings::UnbindAction(ContextId ctx, Source src, ActionId action) noexcept {
    for (usize i = 0; i < m_actionBindingCount; ++i) {
        const auto& b = m_actionBindings[i];
        if (b.context == ctx && b.source == src && b.action == action) {
            // Shift remaining elements to preserve order
            for (usize j = i; j + 1 < m_actionBindingCount; ++j) {
                m_actionBindings[j] = m_actionBindings[j + 1];
            }
            --m_actionBindingCount;
            return Ok;
        }
    }
    return Err(InputError::NotFound);
}

Result<void, InputError> InputBindings::BindAxis(ContextId ctx, Source neg, Source pos, AxisId axis) noexcept {
    if (axis >= kMaxAxes) {
        return Err(InputError::InvalidSource);
    }

    const bool negValid = neg.IsValid();
    const bool posValid = pos.IsValid();
    const bool negOk = (neg.device == Device::None) || negValid;
    const bool posOk = (pos.device == Device::None) || posValid;

    if (!negOk || !posOk || (!negValid && !posValid) || (negValid && posValid && neg == pos)) {
        return Err(InputError::InvalidSource);
    }

    // Replace if binding for this context and axis already exists
    for (usize i = 0; i < m_axisBindingCount; ++i) {
        if (m_axisBindings[i].context == ctx && m_axisBindings[i].axis == axis) {
            m_axisBindings[i].negativeSource = neg;
            m_axisBindings[i].positiveSource = pos;
            return Ok;
        }
    }

    if (m_axisBindingCount >= kMaxAxisBindings) {
        return Err(InputError::TableFull);
    }

    m_axisBindings[m_axisBindingCount++] = AxisBinding{
        .context = ctx,
        .negativeSource = neg,
        .positiveSource = pos,
        .axis = axis
    };
    return Ok;
}

Result<void, InputError> InputBindings::UnbindAxis(ContextId ctx, AxisId axis) noexcept {
    for (usize i = 0; i < m_axisBindingCount; ++i) {
        if (m_axisBindings[i].context == ctx && m_axisBindings[i].axis == axis) {
            for (usize j = i; j + 1 < m_axisBindingCount; ++j) {
                m_axisBindings[j] = m_axisBindings[j + 1];
            }
            --m_axisBindingCount;
            return Ok;
        }
    }
    return Err(InputError::NotFound);
}

void InputBindings::Clear() noexcept {
    m_actionBindingCount = 0;
    m_axisBindingCount = 0;
}

void InputBindings::SetDefaultBindings() noexcept {
    Clear();

    // Axis 0: MoveX (A / D, Left / Right)
    (void)BindAxis(0, Source{Device::Keyboard, SDL_SCANCODE_A}, Source{Device::Keyboard, SDL_SCANCODE_D}, 0);
    // Axis 1: MoveY (W / S, Up / Down)
    (void)BindAxis(0, Source{Device::Keyboard, SDL_SCANCODE_W}, Source{Device::Keyboard, SDL_SCANCODE_S}, 1);

    // Action 0: Primary Action (Space / Gamepad A)
    (void)BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_SPACE}, 0);
    // Action 1: Secondary Action (J / Mouse Left)
    (void)BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_J}, 1);
    (void)BindAction(0, Source{Device::Mouse, 1}, 1);
    // Action 2: Tertiary Action (K / Mouse Right)
    (void)BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_K}, 2);
    (void)BindAction(0, Source{Device::Mouse, 3}, 2);
    // Action 3: Dash / Sprint (Left Shift)
    (void)BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_LSHIFT}, 3);
    // Action 4: Menu / Pause (Escape)
    (void)BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_ESCAPE}, 4);
    // Action 5: Confirm (Return)
    (void)BindAction(0, Source{Device::Keyboard, SDL_SCANCODE_RETURN}, 5);
}

} // namespace EngineN
