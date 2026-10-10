#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>
#include <span>
#include <type_traits>

namespace EngineN {

struct ActionBinding {
    ContextId context = 0;
    Source source{};
    ActionId action = 0;
};

struct AxisBinding {
    ContextId context = 0;
    Source negativeSource{};
    Source positiveSource{};
    AxisId axis = 0;
};

/**
 * Flat binding tables with validated physical sources and context layer awareness.
 * Guarantees zero heap allocation and zero out-of-bounds reads.
 */
class InputBindings {
public:
    static constexpr usize kMaxActionBindings = 64;
    static constexpr usize kMaxAxisBindings = 16;

    [[nodiscard]] Result<void, InputError> BindAction(ContextId ctx, Source src, ActionId action) noexcept;
    [[nodiscard]] Result<void, InputError> UnbindAction(ContextId ctx, Source src, ActionId action) noexcept;
    [[nodiscard]] Result<void, InputError> BindAxis(ContextId ctx, Source neg, Source pos, AxisId axis) noexcept;
    [[nodiscard]] Result<void, InputError> UnbindAxis(ContextId ctx, AxisId axis) noexcept;

    void Clear() noexcept;
    void SetDefaultBindings() noexcept;

    [[nodiscard]] std::span<const ActionBinding> GetActionBindings() const noexcept {
        return std::span<const ActionBinding>(m_actionBindings.data(), m_actionBindingCount);
    }

    [[nodiscard]] std::span<const AxisBinding> GetAxisBindings() const noexcept {
        return std::span<const AxisBinding>(m_axisBindings.data(), m_axisBindingCount);
    }

    template <typename EAction>
    requires std::is_enum_v<EAction>
    [[nodiscard]] Result<void, InputError> BindAction(ContextId ctx, Source src, EAction action) noexcept {
        return BindAction(ctx, src, static_cast<ActionId>(action));
    }

    template <typename EAction>
    requires std::is_enum_v<EAction>
    [[nodiscard]] Result<void, InputError> UnbindAction(ContextId ctx, Source src, EAction action) noexcept {
        return UnbindAction(ctx, src, static_cast<ActionId>(action));
    }

    template <typename EAxis>
    requires std::is_enum_v<EAxis>
    [[nodiscard]] Result<void, InputError> BindAxis(ContextId ctx, Source neg, Source pos, EAxis axis) noexcept {
        return BindAxis(ctx, neg, pos, static_cast<AxisId>(axis));
    }

    template <typename EAxis>
    requires std::is_enum_v<EAxis>
    [[nodiscard]] Result<void, InputError> UnbindAxis(ContextId ctx, EAxis axis) noexcept {
        return UnbindAxis(ctx, static_cast<AxisId>(axis));
    }

private:
    std::array<ActionBinding, kMaxActionBindings> m_actionBindings{};
    usize m_actionBindingCount = 0;

    std::array<AxisBinding, kMaxAxisBindings> m_axisBindings{};
    usize m_axisBindingCount = 0;
};

} // namespace EngineN
