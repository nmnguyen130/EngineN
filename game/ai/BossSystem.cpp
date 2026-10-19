#include "game/ai/BossSystem.h"

#include <algorithm>

namespace en::veil {

void BossSystem::Step(
    BossConductorComponent& boss,
    VeilCombatStatsComponent& bossStats,
    VeilCombatStatsComponent* optPlayerStats,
    f32 distanceToPlayer,
    const VeilActionStateComponent* optPlayerAction) noexcept {
    if (!bossStats.alive) {
        boss.activeTelegraph = BossTelegraphKind::None;
        boss.telegraphTicksRemaining = 0;
        return;
    }

    if (boss.attackStrikeTicks > 0) {
        --boss.attackStrikeTicks;
    }

    // Phase transition at 50% HP
    if (boss.symphonyMovementPhase == 1 && bossStats.hp <= (bossStats.maxHp * 0.50f)) {
        boss.symphonyMovementPhase = 2;
        bossStats.posture = BossPosture::Dominant;
    }

    // Staggered / Submissive posture recovery
    if (bossStats.posture == BossPosture::Submissive) {
        boss.activeTelegraph = BossTelegraphKind::None;
        boss.telegraphTicksRemaining = 0;
        return;
    }

    // Active telegraph execution countdown
    if (boss.telegraphTicksRemaining > 0) {
        --boss.telegraphTicksRemaining;
        if (boss.telegraphTicksRemaining == 0) {
            const bool playerGuarding = (optPlayerAction != nullptr &&
                (optPlayerAction->currentAction == ActionId::DefensiveParry ||
                 optPlayerAction->currentAction == ActionId::PhaseCounter));

            if (!playerGuarding) {
                // Telegraph window expired without player parrying -> Boss executes attack!
                boss.attackStrikeTicks = 14;
                if (optPlayerStats != nullptr) {
                    if (boss.activeTelegraph == BossTelegraphKind::GoldHarmonic && distanceToPlayer <= 4.2f) {
                        optPlayerStats->hp = std::max(0.0f, optPlayerStats->hp - 65.0f);
                        if (optPlayerStats->hp <= 0.0f) optPlayerStats->alive = false;
                    } else if (boss.activeTelegraph == BossTelegraphKind::CrimsonOverload && distanceToPlayer <= 5.0f) {
                        optPlayerStats->hp = std::max(0.0f, optPlayerStats->hp - 95.0f);
                        if (optPlayerStats->hp <= 0.0f) optPlayerStats->alive = false;
                    }
                }
            }
            boss.activeTelegraph = BossTelegraphKind::None;
        }
        return;
    }

    // Boss Combat AI cadence: cue attacks periodically
    ++boss.actionTimerTicks;
    const u16 attackInterval = (boss.symphonyMovementPhase == 1) ? 320 : 200; // 2.6s or 1.6s
    if (boss.actionTimerTicks >= attackInterval) {
        boss.actionTimerTicks = 0;
        const bool isCrimson = (boss.symphonyMovementPhase >= 2) && ((boss.phaseInversionsSuffered % 2) == 1);
        if (isCrimson) {
            CueCrimsonOverloadAttack(boss, bossStats, 48);
        } else {
            CueGoldHarmonicAttack(boss, bossStats, boss.requiredCounterEcho, 58);
        }
    }
}

void BossSystem::CueGoldHarmonicAttack(
    BossConductorComponent& boss,
    VeilCombatStatsComponent& bossStats,
    EchoId requiredCounter,
    u16 windowTicks) noexcept {
    bossStats.posture = BossPosture::Dominant;
    boss.requiredCounterEcho = requiredCounter;
    boss.activeTelegraph = BossTelegraphKind::GoldHarmonic;
    boss.telegraphTicksRemaining = windowTicks;
}

void BossSystem::CueCrimsonOverloadAttack(
    BossConductorComponent& boss,
    VeilCombatStatsComponent& bossStats,
    u16 windowTicks) noexcept {
    bossStats.posture = BossPosture::Dominant;
    boss.activeTelegraph = BossTelegraphKind::CrimsonOverload;
    boss.telegraphTicksRemaining = windowTicks;
}

} // namespace en::veil
