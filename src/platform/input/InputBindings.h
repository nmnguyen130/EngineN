#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>
#include <span>

namespace EngineN {

struct ActionBinding {
    Source    source{};
    ContextId context = 0;
    ActionId  action = 0;
};

// Dual-source axis mapping: digital key pairs (A/D) and analog sticks/triggers.
struct AxisBinding {
    ContextId   context = 0;
    AxisId      axis = 0;
    Source      negativeSource{};
    Source      positiveSource{};
    GamepadAxis gamepadAxis = GamepadAxis::None;
    u32         gamepad = Source::kAnyGamepad;
    f32         deadzone = 0.15f;
    f32         scale = 1.0f;
    bool        inverted = false;
};

/**
 * Flat binding tables with validated physical sources and context layer awareness.
 * Guarantees zero heap allocation and zero out-of-bounds reads.
 */
class InputBindings {
public:
    static constexpr usize kMaxActionBindings = 64;
    static constexpr usize kMaxAxisBindings = 16;

    template <typename A = ActionId>
    [[nodiscard]] Result<void, InputError> BindAction(ContextId ctx, Source src, A action) noexcept {
        const usize a = static_cast<usize>(action);
        if (!src.IsValid() || a >= kMaxActions) {
            return Err(InputError::InvalidSource);
        }
        return BindActionImpl(ctx, src, static_cast<ActionId>(a));
    }

    template <typename A = ActionId>
    [[nodiscard]] Result<void, InputError> UnbindAction(ContextId ctx, Source src, A action) noexcept {
        const usize a = static_cast<usize>(action);
        if (!src.IsValid() || a >= kMaxActions) {
            return Err(InputError::InvalidSource);
        }
        return UnbindActionImpl(ctx, src, static_cast<ActionId>(a));
    }

    template <typename A = AxisId>
    [[nodiscard]] Result<void, InputError> BindAxis(ContextId ctx, Source neg, Source pos, A axis) noexcept {
        const usize ax = static_cast<usize>(axis);
        if (ax >= kMaxAxes || !neg.IsValid() || !pos.IsValid() || neg == pos) {
            return Err(InputError::InvalidSource);
        }
        return BindAxisImpl(ctx, neg, pos, static_cast<AxisId>(ax));
    }

    template <typename A = AxisId>
    [[nodiscard]] Result<void, InputError> BindGamepadAxis(
        ContextId ctx,
        GamepadAxis gpAxis,
        A targetAxis,
        f32 deadzone = 0.15f,
        f32 scale = 1.0f,
        bool inverted = false,
        u32 gamepad = Source::kAnyGamepad) noexcept {
        const usize ax = static_cast<usize>(targetAxis);
        if (ax >= kMaxAxes) {
            return Err(InputError::InvalidSource);
        }
        return BindGamepadAxisImpl(ctx, gpAxis, static_cast<AxisId>(ax), deadzone, scale, inverted, gamepad);
    }

    template <typename A = AxisId>
    [[nodiscard]] Result<void, InputError> UnbindAxis(ContextId ctx, A axis) noexcept {
        const usize ax = static_cast<usize>(axis);
        if (ax >= kMaxAxes) {
            return Err(InputError::InvalidSource);
        }
        return UnbindAxisImpl(ctx, static_cast<AxisId>(ax));
    }

    void Clear() noexcept;
    void SetDefaultBindings() noexcept;

    [[nodiscard]] std::span<const ActionBinding> GetActionBindings() const noexcept {
        return {m_actions.data(), m_actionCount};
    }

    [[nodiscard]] std::span<const AxisBinding> GetAxisBindings() const noexcept {
        return {m_axes.data(), m_axisCount};
    }

    // Bumped on every successful mutation; InputSystem uses it to re-validate held actions.
    [[nodiscard]] u32 Revision() const noexcept {
        return m_revision;
    }

private:
    [[nodiscard]] Result<void, InputError> BindActionImpl(ContextId ctx, Source src, ActionId action) noexcept;
    [[nodiscard]] Result<void, InputError> UnbindActionImpl(ContextId ctx, Source src, ActionId action) noexcept;
    [[nodiscard]] Result<void, InputError> BindAxisImpl(ContextId ctx, Source neg, Source pos, AxisId axis) noexcept;
    [[nodiscard]] Result<void, InputError> BindGamepadAxisImpl(
        ContextId ctx,
        GamepadAxis gpAxis,
        AxisId targetAxis,
        f32 deadzone,
        f32 scale,
        bool inverted,
        u32 gamepad) noexcept;
    [[nodiscard]] Result<void, InputError> UnbindAxisImpl(ContextId ctx, AxisId axis) noexcept;

    // One AxisBinding per (context, axis): keyboard pair and gamepad axis share the slot.
    [[nodiscard]] AxisBinding* AcquireAxis(ContextId ctx, AxisId axis) noexcept;

    std::array<ActionBinding, kMaxActionBindings> m_actions{};
    std::array<AxisBinding, kMaxAxisBindings>     m_axes{};
    usize m_actionCount = 0;
    usize m_axisCount   = 0;
    u32   m_revision    = 0;
};

} // namespace EngineN
