#pragma once

#include "game/core/Components.h"
#include "game/core/Types.h"

namespace en::veil {

// Manages Boss Conductor telegraph windows, symphony phase shifts, and attack cues.
// Stateless system class; operates on non-owning references to boss components.
// Invariant: Telegraph ticks decrement to 0 and reset activeTelegraph to None.
class BossSystem {
public:
    static void Step(
        BossConductorComponent& boss,
        VeilCombatStatsComponent& bossStats,
        VeilCombatStatsComponent* optPlayerStats = nullptr,
        f32 distanceToPlayer = 10.0f,
        const VeilActionStateComponent* optPlayerAction = nullptr) noexcept;

    static void CueGoldHarmonicAttack(
        BossConductorComponent& boss,
        VeilCombatStatsComponent& bossStats,
        EchoId requiredCounter,
        u16 windowTicks = 45) noexcept;

    static void CueCrimsonOverloadAttack(
        BossConductorComponent& boss,
        VeilCombatStatsComponent& bossStats,
        u16 windowTicks = 36) noexcept;
};

} // namespace en::veil
