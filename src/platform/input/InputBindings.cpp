#include "platform/input/InputBindings.h"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <cmath>

namespace EngineN {

Result<void, InputError> InputBindings::BindActionImpl(ContextId ctx, Source src, ActionId action) noexcept {
    for (usize i = 0; i < m_actionCount; ++i) {
        const auto& b = m_actions[i];
        if (b.context == ctx && b.source == src && b.action == action) {
            return Err(InputError::Duplicate);
        }
    }

    if (m_actionCount >= kMaxActionBindings) {
        return Err(InputError::TableFull);
    }

    m_actions[m_actionCount++] = {.source = src, .context = ctx, .action = action};
    ++m_revision;
    return Ok;
}

Result<void, InputError> InputBindings::UnbindActionImpl(ContextId ctx, Source src, ActionId action) noexcept {
    for (usize i = 0; i < m_actionCount; ++i) {
        const auto& b = m_actions[i];
        if (b.context == ctx && b.source == src && b.action == action) {
            m_actions[i] = m_actions[--m_actionCount]; // swap-with-last; order not preserved
            ++m_revision;
            return Ok;
        }
    }
    return Err(InputError::NotFound);
}

Result<void, InputError> InputBindings::BindAxisImpl(ContextId ctx, Source neg, Source pos, AxisId axis) noexcept {
    AxisBinding* b = AcquireAxis(ctx, axis);
    if (!b) {
        return Err(InputError::TableFull);
    }

    b->negativeSource = neg;
    b->positiveSource = pos;
    ++m_revision;
    return Ok;
}

Result<void, InputError> InputBindings::BindGamepadAxisImpl(
    ContextId ctx,
    GamepadAxis gpAxis,
    AxisId targetAxis,
    f32 deadzone,
    f32 scale,
    bool inverted,
    u32 gamepad) noexcept {
    const bool validDeadzone = std::isfinite(deadzone) && deadzone >= 0.0f && deadzone < 1.0f;
    if (gpAxis > GamepadAxis::RightTrigger || !validDeadzone || !std::isfinite(scale)) {
        return Err(InputError::InvalidParam);
    }

    AxisBinding* b = AcquireAxis(ctx, targetAxis);
    if (!b) {
        return Err(InputError::TableFull);
    }

    b->gamepadAxis = gpAxis;
    b->gamepad     = gamepad;
    b->deadzone    = deadzone;
    b->scale       = scale;
    b->inverted    = inverted;
    ++m_revision;
    return Ok;
}

Result<void, InputError> InputBindings::UnbindAxisImpl(ContextId ctx, AxisId axis) noexcept {
    for (usize i = 0; i < m_axisCount; ++i) {
        if (m_axes[i].context == ctx && m_axes[i].axis == axis) {
            m_axes[i] = m_axes[--m_axisCount]; // swap-with-last; order not preserved
            ++m_revision;
            return Ok;
        }
    }
    return Err(InputError::NotFound);
}

AxisBinding* InputBindings::AcquireAxis(ContextId ctx, AxisId axis) noexcept {
    for (usize i = 0; i < m_axisCount; ++i) {
        if (m_axes[i].context == ctx && m_axes[i].axis == axis) {
            return &m_axes[i];
        }
    }

    if (m_axisCount >= kMaxAxisBindings) {
        return nullptr;
    }

    m_axes[m_axisCount] = {.context = ctx, .axis = axis};
    return &m_axes[m_axisCount++];
}

void InputBindings::Clear() noexcept {
    m_actionCount = 0;
    m_axisCount   = 0;
    ++m_revision;
}

void InputBindings::SetDefaultBindings() noexcept {
    Clear();

    // Context 0: Base Gameplay (Axes)
    (void)BindAxis(0, Source{Device::Keyboard, SDL_SCANCODE_A}, Source{Device::Keyboard, SDL_SCANCODE_D}, 0);
    (void)BindAxis(0, Source{Device::Keyboard, SDL_SCANCODE_W}, Source{Device::Keyboard, SDL_SCANCODE_S}, 1);
    (void)BindGamepadAxis(0, GamepadAxis::LeftX, 0, 0.15f);
    (void)BindGamepadAxis(0, GamepadAxis::LeftY, 1, 0.15f);

    // Context 0: Base Gameplay (Actions)
    static constexpr ActionBinding kDefaults[] = {
        {.source = {Device::Keyboard, SDL_SCANCODE_SPACE},                .context = 0, .action = 0},
        {.source = {Device::Gamepad,  SDL_GAMEPAD_BUTTON_SOUTH},          .context = 0, .action = 0},
        {.source = {Device::Keyboard, SDL_SCANCODE_J},                    .context = 0, .action = 1},
        {.source = {Device::Mouse,    1},                                 .context = 0, .action = 1},
        {.source = {Device::Gamepad,  SDL_GAMEPAD_BUTTON_WEST},           .context = 0, .action = 1},
        {.source = {Device::Keyboard, SDL_SCANCODE_K},                    .context = 0, .action = 2},
        {.source = {Device::Mouse,    3},                                 .context = 0, .action = 2},
        {.source = {Device::Gamepad,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, .context = 0, .action = 2},
        {.source = {Device::Keyboard, SDL_SCANCODE_LSHIFT},               .context = 0, .action = 3},
        {.source = {Device::Keyboard, SDL_SCANCODE_ESCAPE},               .context = 0, .action = 4},
        {.source = {Device::Keyboard, SDL_SCANCODE_RETURN},               .context = 0, .action = 5},
    };

    for (const auto& d : kDefaults) {
        (void)BindAction(d.context, d.source, d.action);
    }
}

} // namespace EngineN
