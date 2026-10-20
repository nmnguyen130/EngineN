#pragma once

#include "core/Types.h"
#include "world/WorldTypes.h"
#include <array>

namespace en {

enum class AttributeKind : u8 {
    MoveSpeed = 0,
    AttackDamage = 1,
    Defense = 2,
    FocusRegen = 3,
    MaxHp = 4,
    Count = 5,
};

enum class ModifierOp : u8 {
    FlatAdd = 0,
    PercentAdd = 1,
    PercentMult = 2,
};

struct alignas(4) AttributeModifier {
    u16 modifierId{0};
    u16 remainingTicks{0};
    f32 magnitude{0.0f};
    AttributeKind attribute{AttributeKind::MoveSpeed};
    ModifierOp op{ModifierOp::FlatAdd};
    u8 stacks{1};
    u8 maxStacks{1};
};
static_assert(sizeof(AttributeModifier) == 12);

// Data-oriented Buff/Debuff and Attribute Modifier stack component (kTypeId = 23).
// Evaluates (Base + sum(FlatAdd)) * (1 + sum(PercentAdd)) * prod(PercentMult) in a single pass.
// Invariant: active modifiers have stacks >= 1 and expire automatically when remainingTicks reaches 0.
struct alignas(8) ModifierStack2D {
    static constexpr u8 kTypeId = 23;
    static constexpr usize kMaxModifiers = 8;

    std::array<AttributeModifier, kMaxModifiers> modifiers{};
    u8 activeCount{0};
    u8 dirtyMask{0xFFU};
    u16 totalAppliedCount{0};

    bool ApplyModifier(const AttributeModifier& mod) noexcept;
    bool RemoveModifier(u16 modifierId) noexcept;
    void TickModifiers() noexcept;
    [[nodiscard]] f32 Evaluate(AttributeKind attr, f32 baseValue) const noexcept;
};
static_assert(sizeof(ModifierStack2D) == 104);

struct alignas(4) AbilitySlot {
    u16 abilityId{0};
    u16 cooldownTicks{60};
    u16 remainingCooldown{0};
    u16 durationTicks{15};
    f32 resourceCost{20.0f};
    u16 grantModifierId{0};
    AttributeKind grantAttr{AttributeKind::AttackDamage};
    f32 grantMagnitude{0.25f};
};
static_assert(sizeof(AbilitySlot) == 20);

// Cooldown- and resource-gated ability activator with automatic ModifierStack2D buff application.
class AbilitySystem2D {
public:
    static constexpr usize kMaxAbilities = 8;

    constexpr void Clear() noexcept { m_count = 0; }

    bool RegisterAbility(const AbilitySlot& slot) noexcept;
    void TickCooldowns() noexcept;
    [[nodiscard]] bool IsReady(u16 abilityId, f32 availableResource) const noexcept;
    bool TryActivate(u16 abilityId, f32& ioResource, ModifierStack2D* optionalModStack = nullptr) noexcept;

private:
    std::array<AbilitySlot, kMaxAbilities> m_slots{};
    usize m_count{0};
};

} // namespace en
