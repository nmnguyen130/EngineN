#include "game/world/WorldSystem.h"

namespace en::veil {

void WorldSystem::RestAtTuningResonator(
    ResonanceLanternComponent& lantern,
    VeilCombatStatsComponent& playerStats,
    WorldProgressComponent& progress,
    EchoId newLead,
    EchoId newWingL,
    EchoId newWingR) noexcept {
    lantern.lanternRestSiteOpen = true;
    lantern.triadWicks = {newLead, newWingL, newWingR};
    lantern.harmonyGauge = 100.0f;
    playerStats.hp = playerStats.maxHp * (1.0f - progress.maxHpLockRatio);
    playerStats.alive = true;
}

void WorldSystem::RecordPlayerDeath(
    f32 posX,
    f32 posY,
    VeilCombatStatsComponent& playerStats,
    WorldProgressComponent& progress) noexcept {
    playerStats.alive = false;
    progress.hasLostWickHusk = true;
    progress.lostWickX = posX;
    progress.lostWickY = posY;
    progress.shardsInLostWick = progress.tuningShards;
    progress.tuningShards = 0;
    progress.maxHpLockRatio = 0.25f;
}

void WorldSystem::ReclaimLostWickHusk(
    VeilCombatStatsComponent& playerStats,
    WorldProgressComponent& progress) noexcept {
    if (!progress.hasLostWickHusk) {
        return;
    }
    progress.tuningShards += progress.shardsInLostWick;
    progress.shardsInLostWick = 0;
    progress.hasLostWickHusk = false;
    progress.maxHpLockRatio = 0.0f;
    playerStats.hp = playerStats.maxHp;
}

bool WorldSystem::TryUnlockFrequencyGate(
    const ResonanceLanternComponent& lantern,
    WorldProgressComponent& progress,
    u8 gateIndex,
    EchoId requiredEcho) noexcept {
    if (lantern.LeadEcho() != requiredEcho) {
        return false;
    }
    progress.solvedFrequencyGatesMask |= (1u << gateIndex);
    return true;
}

} // namespace en::veil
