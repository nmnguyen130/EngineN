#pragma once

#include "game/core/ActionTable.h"
#include "game/core/Components.h"
#include "game/core/EchoPantheon.h"
#include "game/core/Types.h"

namespace en::veil {

// Orchestrates deterministic 120 Hz combat: Amplitude Ascension, Standing-Wave Interference,
// Phase-Inversion Counter, and Polyphonic Unison / Sacred Chords.
// Stateless system class; operates on non-owning references to combat components.
// Invariant: Gauge values remain clamped to [0.0f, 100.0f]; mark timers decrement down to 0.
class CombatSystem {
public:
    static constexpr u16 kWaveMarkDurationTicks = 420;
    static constexpr u16 kAmplitudeDecayTicks = 480;
    static constexpr f32 kTierPromotionThreshold = 30.0f;

    static CombatTickSummary Step(
        ResonanceLanternComponent& lantern,
        AmplitudeAscensionComponent& amp,
        VeilActionStateComponent& action,
        TraversalMutationComponent& traversal,
        WingAssistComponent& wing,
        PhaseInversionCounterComponent& parry,
        VeilCombatStatsComponent& playerStats,
        VeilCombatStatsComponent& targetStats,
        StandingWaveComponent& targetWave,
        BossConductorComponent* bossOptional,
        bool primaryAttackPressed,
        bool weaveShiftPressed,
        bool wingLeftPressed,
        bool wingRightPressed,
        bool parryPressed,
        bool dualUnisonPressed,
        bool triadUnisonPressed,
        bool targetInMeleeRange = true,
        bool targetInWingRange = true) noexcept;

    static void ApplyEchoStrike(
        ResonanceLanternComponent& lantern,
        AmplitudeAscensionComponent& amp,
        TraversalMutationComponent& traversal,
        VeilCombatStatsComponent& playerStats,
        VeilCombatStatsComponent& targetStats,
        StandingWaveComponent& targetWave,
        EchoId strikingEcho,
        f32 rawDamage,
        f32 rawBreak,
        f32 energyGain,
        f32 harmonyGain,
        CombatTickSummary& out) noexcept;

private:
    static void TickTimers(
        ResonanceLanternComponent& lantern,
        AmplitudeAscensionComponent& amp,
        WingAssistComponent& wing,
        VeilCombatStatsComponent& targetStats,
        StandingWaveComponent& targetWave) noexcept;
};

} // namespace en::veil
