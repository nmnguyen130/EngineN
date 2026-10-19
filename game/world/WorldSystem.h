#pragma once

#include "game/core/Components.h"
#include "game/core/Types.h"

namespace en::veil {

// Handles Metroidvania checkpoints (Tuning Resonators), Corpse Run recovery (Lost Wick Husk), and Frequency Gates.
// Stateless system class; operates on non-owning references to world progress and player components.
// Invariant: Reclaimed shards restore full amount; maxHpLockRatio remains in [0.0f, 0.25f].
class WorldSystem {
public:
    static void RestAtTuningResonator(
        ResonanceLanternComponent& lantern,
        VeilCombatStatsComponent& playerStats,
        WorldProgressComponent& progress,
        EchoId newLead,
        EchoId newWingL,
        EchoId newWingR) noexcept;

    static void RecordPlayerDeath(
        f32 posX,
        f32 posY,
        VeilCombatStatsComponent& playerStats,
        WorldProgressComponent& progress) noexcept;

    static void ReclaimLostWickHusk(
        VeilCombatStatsComponent& playerStats,
        WorldProgressComponent& progress) noexcept;

    [[nodiscard]] static bool TryUnlockFrequencyGate(
        const ResonanceLanternComponent& lantern,
        WorldProgressComponent& progress,
        u8 gateIndex,
        EchoId requiredEcho) noexcept;
};

} // namespace en::veil
