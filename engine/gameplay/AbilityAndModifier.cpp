#include "gameplay/AbilityAndModifier.h"
#include <algorithm>

namespace en {

bool ModifierStack2D::ApplyModifier(const AttributeModifier& mod) noexcept {
    if (mod.modifierId == 0 || mod.remainingTicks == 0) return false;
    for (u8 i = 0; i < activeCount; ++i) {
        AttributeModifier& cur = modifiers[i];
        if (cur.modifierId == mod.modifierId) {
            cur.remainingTicks = std::max(cur.remainingTicks, mod.remainingTicks);
            cur.magnitude = mod.magnitude;
            cur.stacks = std::min<u8>(static_cast<u8>(cur.stacks + mod.stacks), std::max<u8>(1U, mod.maxStacks));
            dirtyMask |= static_cast<u8>(1U << static_cast<u8>(mod.attribute));
            ++totalAppliedCount;
            return true;
        }
    }
    if (activeCount >= kMaxModifiers) return false;
    AttributeModifier inserted = mod;
    inserted.stacks = std::clamp<u8>(mod.stacks, 1U, std::max<u8>(1U, mod.maxStacks));
    modifiers[activeCount++] = inserted;
    dirtyMask |= static_cast<u8>(1U << static_cast<u8>(mod.attribute));
    ++totalAppliedCount;
    return true;
}

bool ModifierStack2D::RemoveModifier(u16 modifierId) noexcept {
    for (u8 i = 0; i < activeCount; ++i) {
        if (modifiers[i].modifierId == modifierId) {
            dirtyMask |= static_cast<u8>(1U << static_cast<u8>(modifiers[i].attribute));
            modifiers[i] = modifiers[--activeCount];
            return true;
        }
    }
    return false;
}

void ModifierStack2D::TickModifiers() noexcept {
    u8 writeIdx = 0;
    for (u8 i = 0; i < activeCount; ++i) {
        AttributeModifier m = modifiers[i];
        if (m.remainingTicks > 0) --m.remainingTicks;
        if (m.remainingTicks > 0) {
            modifiers[writeIdx++] = m;
        } else {
            dirtyMask |= static_cast<u8>(1U << static_cast<u8>(m.attribute));
        }
    }
    activeCount = writeIdx;
}

f32 ModifierStack2D::Evaluate(AttributeKind attr, f32 baseValue) const noexcept {
    f32 flatSum = 0.0f;
    f32 pctAddSum = 0.0f;
    f32 pctMultProd = 1.0f;

    for (u8 i = 0; i < activeCount; ++i) {
        const AttributeModifier& m = modifiers[i];
        if (m.attribute != attr) continue;
        const f32 scaledMag = m.magnitude * static_cast<f32>(m.stacks);
        switch (m.op) {
            case ModifierOp::FlatAdd:
                flatSum += scaledMag;
                break;
            case ModifierOp::PercentAdd:
                pctAddSum += scaledMag;
                break;
            case ModifierOp::PercentMult:
                pctMultProd *= (1.0f + scaledMag);
                break;
        }
    }
    return std::max(0.0f, (baseValue + flatSum) * (1.0f + pctAddSum) * pctMultProd);
}

bool AbilitySystem2D::RegisterAbility(const AbilitySlot& slot) noexcept {
    if (slot.abilityId == 0 || m_count >= kMaxAbilities) return false;
    m_slots[m_count++] = slot;
    return true;
}

void AbilitySystem2D::TickCooldowns() noexcept {
    for (usize i = 0; i < m_count; ++i) {
        if (m_slots[i].remainingCooldown > 0) {
            --m_slots[i].remainingCooldown;
        }
    }
}

bool AbilitySystem2D::IsReady(u16 abilityId, f32 availableResource) const noexcept {
    for (usize i = 0; i < m_count; ++i) {
        if (m_slots[i].abilityId == abilityId) {
            return m_slots[i].remainingCooldown == 0 && availableResource >= m_slots[i].resourceCost;
        }
    }
    return false;
}

bool AbilitySystem2D::TryActivate(
    u16 abilityId, f32& ioResource, ModifierStack2D* optionalModStack) noexcept {
    for (usize i = 0; i < m_count; ++i) {
        AbilitySlot& s = m_slots[i];
        if (s.abilityId == abilityId) {
            if (s.remainingCooldown > 0 || ioResource < s.resourceCost) return false;
            ioResource -= s.resourceCost;
            s.remainingCooldown = s.cooldownTicks;
            if (optionalModStack && s.grantModifierId != 0 && s.durationTicks > 0) {
                AttributeModifier mod{};
                mod.modifierId = s.grantModifierId;
                mod.remainingTicks = s.durationTicks;
                mod.magnitude = s.grantMagnitude;
                mod.attribute = s.grantAttr;
                mod.op = ModifierOp::PercentAdd;
                mod.stacks = 1;
                mod.maxStacks = 3;
                (void)optionalModStack->ApplyModifier(mod);
            }
            return true;
        }
    }
    return false;
}

} // namespace en
