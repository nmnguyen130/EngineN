#pragma once

#include "game/assets/GameAssetPipeline.h"
#include "game/components/GameComponents.h"
#include "gameplay/GameplayPipeline.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

namespace vots {

struct CombatResolutionReport {
    WaveReactionId reactionTriggered{WaveReactionId::None};
    SacredChordId chordTriggered{SacredChordId::None};
    i32 primaryHpDamageDealt{0};
    u8 primaryBreakAdded{0U};
    u8 secondaryTargetsHit{0U};
    u8 projectilesCleared{0U};
    u8 destructiblesBroken{0U};
    bool parrySucceeded{false};
    bool phaseInversionBonus{false};
    bool targetEnteredStagger{false};
    bool targetEnteredBreakState{false};
    bool unisonRefundedOnHitstun{false};
};

// Stateless GDD v0.3–v0.5 combat rules engine enforcing independent HP Damage, Break, CC, 420-tick Marks,
// 10 Wave Reactions, Parry, Nail Recoil, Knockback Arc, and Unison/Sacred Chords.
// Operates directly on World component pools; zero dynamic memory allocations.
// Invariant: expired marks clear before hit checks, reactions never recurse, and Hitstun before Unison Resolve refunds full Harmony.
class CombatSystem {
public:
    static void StepCombatTimersAndDelayedStrikes(
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    static void StepTimers(
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept {
        StepCombatTimersAndDelayedStrikes(world, pipeline);
    }

    [[nodiscard]] static i32 ComputeModifiedHpDamage(
        i32 rawHpDamage,
        const BreakAndCcComp* breakComp) noexcept;

    static bool ApplyBreakDamage(
        EntityId targetId,
        u8 rawBreak,
        en::World& world,
        BreakAndCcComp& breakComp,
        CombatResolutionReport* outReport = nullptr) noexcept;

    static void ApplyCrowdControl(
        EntityId targetId,
        ActionLockKind lockKind,
        u16 durationTicks,
        bool interruptWindup,
        en::World& world,
        BreakAndCcComp& breakComp) noexcept;

    static void ApplySlowEffect(
        BreakAndCcComp& breakComp,
        u8 slowPercent,
        u16 durationTicks) noexcept;

    static CombatResolutionReport ResolvePlayerWeaponHit(
        EntityId attackerId,
        EntityId primaryTargetId,
        const WeaponAttackComp& attack,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    static CombatResolutionReport ResolveWeaponHit(
        EntityId attackerId,
        EntityId primaryTargetId,
        const WeaponAttackComp& attack,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept {
        return ResolvePlayerWeaponHit(attackerId, primaryTargetId, attack, world, pipeline);
    }

    static CombatResolutionReport ResolveEnemyAttackAgainstPlayer(
        EntityId attackerId,
        EntityId playerId,
        i16 attackHpDamage,
        u8 attackBreakDamage,
        AttackTelegraphKind telegraph,
        EchoId counterEchoId,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    static CombatResolutionReport ResolveEnemyHit(
        EntityId attackerId,
        EntityId playerId,
        i16 attackHpDamage,
        u8 attackBreakDamage,
        AttackTelegraphKind telegraph,
        EchoId counterEchoId,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept {
        return ResolveEnemyAttackAgainstPlayer(
            attackerId, playerId, attackHpDamage, attackBreakDamage, telegraph, counterEchoId, world, pipeline);
    }

    [[nodiscard]] static bool BeginUnisonOrSacredChord(
        EntityId playerId,
        UnisonActionState kind,
        en::World& world) noexcept;

    static void StepActiveUnisonOrChord(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        CombatResolutionReport* outReport = nullptr) noexcept;

    [[nodiscard]] static bool TryActivateUnisonBurst(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        CombatResolutionReport* outReport = nullptr) noexcept;

    [[nodiscard]] static bool TryActivateSacredChord(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        CombatResolutionReport* outReport = nullptr) noexcept;

    static void OnEngineCombatHitCallback(
        const en::CombatHitEvent& hit,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;
};

} // namespace vots
