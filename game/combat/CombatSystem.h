#pragma once

#include "game/assets/GameAssetPipeline.h"
#include "game/components/GameComponents.h"
#include "gameplay/GameplayPipeline.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

namespace vots {

// Summary of a resolved combat hit or reaction for HUD telemetry, audio cues, and deterministic testing.
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
};

// Stateless GDD v0.2 combat rules engine enforcing independent HP Damage, Break, CC, 420-tick Marks, 10 Reactions, Parry, and Unison/Chords.
// Operates directly on World component pools; zero dynamic memory allocations.
// Invariant: expired marks clear before hit checks, reactions never recurse or restore Air-Dash, and Boss Break Recovery blocks Break re-entry.
class CombatSystem {
public:
    // Advances mark timers (expiring 0-tick marks first), Chrono-Overdrive delayed strikes, Break State/Recovery, and CC timers.
    static void StepCombatTimersAndDelayedStrikes(
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    // Computes effective HP Damage after Fracture (1.15x) and Break State (1.25x) multiplicative modifiers.
    [[nodiscard]] static i32 ComputeModifiedHpDamage(
        i32 rawHpDamage,
        const BreakAndCcComp* breakComp) noexcept;

    // Applies Break damage respecting Break resistance, Stagger (45t), Break State (150t), and Break Recovery (240t) immunities.
    static bool ApplyBreakDamage(
        EntityId targetId,
        u8 rawBreak,
        en::World& world,
        BreakAndCcComp& breakComp,
        CombatResolutionReport* outReport = nullptr) noexcept;

    // Applies action-locking CC (BreakLock > Stun > MicroStagger) and Slow without stacking durations or downgrading stronger locks.
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

    // Resolves a single Resonance Blade weapon hit from attacker onto primaryTarget, including mark state and Signature Reactions.
    static CombatResolutionReport ResolvePlayerWeaponHit(
        EntityId attackerId,
        EntityId primaryTargetId,
        const WeaponAttackComp& attack,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    // Resolves an enemy/boss attack against Kaelen, checking the 10-tick Parry window, Gold vs Crimson telegraph, and CounterEchoId bonus.
    static CombatResolutionReport ResolveEnemyAttackAgainstPlayer(
        EntityId attackerId,
        EntityId playerId,
        i16 attackHpDamage,
        u8 attackBreakDamage,
        AttackTelegraphKind telegraph,
        EchoId counterEchoId,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    // Activates Standard Unison Burst (50 Harmony, 3m radius, up to 5 enemies, 18 HP Damage, 15 Break, 30t Standard interrupt).
    [[nodiscard]] static bool TryActivateUnisonBurst(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        CombatResolutionReport* outReport = nullptr) noexcept;

    // Activates a Sacred Chord (100 Harmony + matching unordered 3-Echo Triad Deck).
    [[nodiscard]] static bool TryActivateSacredChord(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        CombatResolutionReport* outReport = nullptr) noexcept;

    // GameplayPolicyTable::onCombatHit bridge invoked automatically by EngineN's StateAndCombatSolver2D.
    static void OnEngineCombatHitCallback(
        const en::CombatHitEvent& hit,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;
};

} // namespace vots
