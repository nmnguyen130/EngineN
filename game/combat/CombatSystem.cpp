#include "game/combat/CombatSystem.h"

#include <algorithm>
#include <cmath>

namespace en::veil {

CombatTickSummary CombatSystem::Step(
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
    bool targetInMeleeRange,
    bool targetInWingRange) noexcept {
    CombatTickSummary out{};
    TickTimers(lantern, amp, wing, targetStats, targetWave);

    if (wingLeftPressed && wing.wingLeftCooldownTicks == 0) {
        const EchoSpec& wingSpec = GetEchoSpec(lantern.WingLeftEcho());
        wing.wingLeftCooldownTicks = wingSpec.wingAssistCooldownTicks;
        if (targetInWingRange) {
            ApplyEchoStrike(
                lantern, amp, traversal, playerStats, targetStats, targetWave,
                wingSpec.id, wingSpec.wingAssistDamage, wingSpec.wingAssistBreak, 12.0f, 8.0f, out);
        }
    }
    if (wingRightPressed && wing.wingRightCooldownTicks == 0) {
        const EchoSpec& wingSpec = GetEchoSpec(lantern.WingRightEcho());
        wing.wingRightCooldownTicks = wingSpec.wingAssistCooldownTicks;
        if (targetInWingRange) {
            ApplyEchoStrike(
                lantern, amp, traversal, playerStats, targetStats, targetWave,
                wingSpec.id, wingSpec.wingAssistDamage, wingSpec.wingAssistBreak, 12.0f, 8.0f, out);
        }
    }

    if (triadUnisonPressed && lantern.harmonyGauge >= 100.0f) {
        lantern.harmonyGauge = 0.0f;
        const SacredChordId chord = ResolveSacredChord(
            lantern.LeadEcho(), lantern.WingLeftEcho(), lantern.WingRightEcho());
        if (chord != SacredChordId::None) {
            action.currentAction = ActionId::SacredChordCast;
            action.actionTick = 0;
            action.hitConfirmedThisAction = true;
            out.triggeredSacredChord = chord;
            if (targetInWingRange) {
                ApplyEchoStrike(
                    lantern, amp, traversal, playerStats, targetStats, targetWave,
                    lantern.LeadEcho(), 260.0f, 240.0f, 50.0f, 0.0f, out);
            }
        } else {
            action.currentAction = ActionId::TriadUnisonCast;
            action.actionTick = 0;
            action.hitConfirmedThisAction = true;
            if (targetInWingRange) {
                ApplyEchoStrike(
                    lantern, amp, traversal, playerStats, targetStats, targetWave,
                    lantern.LeadEcho(), 180.0f, 160.0f, 35.0f, 0.0f, out);
            }
        }
        return out;
    }

    if (dualUnisonPressed && lantern.harmonyGauge >= 50.0f) {
        lantern.harmonyGauge -= 50.0f;
        action.currentAction = ActionId::DualUnisonCast;
        action.actionTick = 0;
        action.hitConfirmedThisAction = true;
        if (targetInWingRange) {
            ApplyEchoStrike(
                lantern, amp, traversal, playerStats, targetStats, targetWave,
                lantern.LeadEcho(), 110.0f, 95.0f, 22.0f, 0.0f, out);
        }
        return out;
    }

    if (parryPressed) {
        action.currentAction = ActionId::DefensiveParry;
        action.actionTick = 0;

        const bool bossTelegraphing = (bossOptional != nullptr &&
            bossOptional->activeTelegraph != BossTelegraphKind::None &&
            bossOptional->telegraphTicksRemaining > 0);
        const bool bossStriking = (bossOptional != nullptr && bossOptional->attackStrikeTicks > 0);

        if (bossTelegraphing || bossStriking) {
            if (bossOptional->activeTelegraph == BossTelegraphKind::GoldHarmonic &&
                lantern.LeadEcho() == bossOptional->requiredCounterEcho) {
                action.currentAction = ActionId::PhaseCounter;
                action.actionTick = 0;
                action.hitConfirmedThisAction = true;
                bossOptional->activeTelegraph = BossTelegraphKind::None;
                bossOptional->telegraphTicksRemaining = 0;
                bossOptional->attackStrikeTicks = 0;
                ++bossOptional->phaseInversionsSuffered;
                targetStats.posture = BossPosture::Submissive;
                targetStats.submissiveTicksRemaining = 360;
                amp.tier = AmplitudeTier::Crescendo;
                amp.tierDecayTicksRemaining = kAmplitudeDecayTicks;
                lantern.harmonyGauge = std::min(100.0f, lantern.harmonyGauge + 50.0f);
                parry.phaseCounterSucceeded = true;
                ++parry.totalCountersLanded;
                out.phaseInversionSucceeded = true;
                ApplyEchoStrike(
                    lantern, amp, traversal, playerStats, targetStats, targetWave,
                    lantern.LeadEcho(), 120.0f, 350.0f, 40.0f, 0.0f, out);
                return out;
            }

            // Guarded Deflection: Deflects any incoming boss attack, absorbs damage, and restores resources
            bossOptional->activeTelegraph = BossTelegraphKind::None;
            bossOptional->telegraphTicksRemaining = 0;
            bossOptional->attackStrikeTicks = 0;
            parry.phaseCounterSucceeded = true;
            out.parryDeflectionSucceeded = true;
            lantern.harmonyGauge = std::min(100.0f, lantern.harmonyGauge + 25.0f);
            amp.waveEnergy = std::min(100.0f, amp.waveEnergy + 15.0f);
            return out;
        }
        return out;
    }

    if (weaveShiftPressed) {
        lantern.RotateTriadForward();
        (void)amp.PromoteOnShift(kTierPromotionThreshold, kAmplitudeDecayTicks);
        action.currentAction = ActionId::WeaveShiftStrike;
        action.actionTick = 0;
        action.hitConfirmedThisAction = true;
        const EchoSpec& leadSpec = GetEchoSpec(lantern.LeadEcho());
        if (targetInMeleeRange) {
            ApplyEchoStrike(
                lantern, amp, traversal, playerStats, targetStats, targetWave,
                leadSpec.id, leadSpec.baseDamage * 1.35f, leadSpec.baseBreak * 1.40f,
                18.0f, 12.0f, out);
        }
        return out;
    }

    const auto& curSpec = GetActionSpec(action.currentAction);
    const bool canStartOrCancel = (action.currentAction == ActionId::Idle) ||
                                  (action.actionTick >= curSpec.cancelWindowStartTick);

    if (primaryAttackPressed && canStartOrCancel) {
        if (action.currentAction == ActionId::LightChain1) {
            action.currentAction = ActionId::LightChain2;
        } else if (action.currentAction == ActionId::LightChain2) {
            action.currentAction = ActionId::LightChain3;
        } else {
            action.currentAction = ActionId::LightChain1;
        }
        action.actionTick = 0;
        action.hitConfirmedThisAction = true;

        const EchoSpec& leadSpec = GetEchoSpec(lantern.LeadEcho());
        const ActionFrameSpec& frameSpec = GetActionSpec(action.currentAction);
        f32 dmg = leadSpec.baseDamage * frameSpec.motionValue;
        f32 brk = leadSpec.baseBreak * frameSpec.breakMultiplier;

        if (leadSpec.id == EchoId::Stridor) {
            ++action.stridorHitCounter;
            if (action.stridorHitCounter >= 3) {
                action.stridorHitCounter = 0;
                dmg *= 1.45f;
                brk += 65.0f;
            }
        }

        if (targetInMeleeRange) {
            ApplyEchoStrike(
                lantern, amp, traversal, playerStats, targetStats, targetWave,
                leadSpec.id, dmg, brk, frameSpec.waveEnergyGain, frameSpec.harmonyGain, out);

            if (amp.tier == AmplitudeTier::Crescendo &&
                (action.currentAction == ActionId::LightChain3 || action.currentAction == ActionId::AirChain2)) {
                out.crescendoBurstTriggered = true;
                ApplyEchoStrike(
                    lantern, amp, traversal, playerStats, targetStats, targetWave,
                    leadSpec.id, leadSpec.baseDamage * 1.60f, leadSpec.baseBreak * 1.50f,
                    0.0f, 20.0f, out);
            }
        } else {
            lantern.harmonyGauge = std::min(100.0f, lantern.harmonyGauge + frameSpec.harmonyGain * 0.5f);
            amp.waveEnergy = std::min(100.0f, amp.waveEnergy + frameSpec.waveEnergyGain * 0.5f);
        }
        return out;
    }

    if (action.currentAction != ActionId::Idle) {
        ++action.actionTick;
        if (action.actionTick >= GetActionSpec(action.currentAction).TotalTicks()) {
            action.currentAction = ActionId::Idle;
            action.actionTick = 0;
        }
    }

    return out;
}

void CombatSystem::ApplyEchoStrike(
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
    CombatTickSummary& out) noexcept {
    const EchoSpec& spec = GetEchoSpec(strikingEcho);

    f32 scaledDamage = rawDamage * amp.DamageMultiplier();
    f32 scaledBreak = rawBreak;

    if (strikingEcho == EchoId::Ignis) {
        targetWave.armorShredRatio = std::max(targetWave.armorShredRatio, 0.25f);
        targetWave.armorShredTicksRemaining = 360;
    } else if (strikingEcho == EchoId::Aqua) {
        playerStats.hp = std::min(playerStats.maxHp, playerStats.hp + 8.0f);
    } else if (strikingEcho == EchoId::Tempus) {
        targetWave.delayedDamagePool += scaledDamage * 0.45f;
        targetWave.delayedBurstTicksRemaining = 12;
    } else if (strikingEcho == EchoId::Aether) {
        targetWave.sympatheticLinked = true;
    }

    if (targetWave.hasActiveWaveMark && targetWave.markedEcho != strikingEcho) {
        const WaveReactionId reaction = ResolveWaveReaction(targetWave.markedEcho, strikingEcho);
        const WaveReactionSpec& rSpec = GetWaveReactionSpec(reaction);

        scaledDamage += rSpec.bonusDamage;
        scaledBreak += rSpec.bonusBreak;
        if (rSpec.armorShredRatio > targetWave.armorShredRatio) {
            targetWave.armorShredRatio = rSpec.armorShredRatio;
            targetWave.armorShredTicksRemaining = 720;
        }
        if (rSpec.stunTicks > targetWave.stunTicksRemaining) {
            targetWave.stunTicksRemaining = rSpec.stunTicks;
        }
        if (rSpec.refundsAirDash) {
            traversal.airDashesRemaining = static_cast<u8>(std::min<u8>(2, traversal.airDashesRemaining + 1));
        }
        lantern.harmonyGauge = std::min(100.0f, lantern.harmonyGauge + rSpec.harmonyRefund);

        targetWave.hasActiveWaveMark = false;
        targetWave.markTicksRemaining = 0;
        targetWave.lastTriggeredReaction = reaction;
        ++targetWave.totalInterferencesTriggered;
        out.triggeredReaction = reaction;
    } else {
        targetWave.hasActiveWaveMark = true;
        targetWave.markedEcho = strikingEcho;
        targetWave.markedPolarity = spec.polarity;
        targetWave.markTicksRemaining = kWaveMarkDurationTicks;
    }

    if (targetWave.sympatheticLinked) {
        scaledDamage *= 1.45f;
    }

    if (targetStats.posture == BossPosture::Submissive) {
        scaledDamage *= 1.50f;
    }

    const f32 effectiveArmor = std::max(0.0f, targetStats.baseArmorRatio - targetWave.armorShredRatio);
    const f32 finalDamage = scaledDamage * (1.0f - effectiveArmor);

    targetStats.hp = std::max(0.0f, targetStats.hp - finalDamage);
    targetStats.breakMeter += scaledBreak;
    if (targetStats.breakMeter >= targetStats.maxBreakMeter) {
        targetStats.breakMeter = 0.0f;
        targetStats.posture = BossPosture::Submissive;
        targetStats.submissiveTicksRemaining = 360;
    }
    if (targetStats.hp <= 0.0f) {
        targetStats.alive = false;
    }

    amp.waveEnergy = std::min(100.0f, amp.waveEnergy + energyGain);
    lantern.harmonyGauge = std::min(100.0f, lantern.harmonyGauge + harmonyGain);
    amp.tierDecayTicksRemaining = kAmplitudeDecayTicks;

    out.damageDealt += finalDamage;
    out.breakDealt += scaledBreak;
}

void CombatSystem::TickTimers(
    ResonanceLanternComponent& lantern,
    AmplitudeAscensionComponent& amp,
    WingAssistComponent& wing,
    VeilCombatStatsComponent& targetStats,
    StandingWaveComponent& targetWave) noexcept {
    (void)lantern;
    if (wing.wingLeftCooldownTicks > 0) --wing.wingLeftCooldownTicks;
    if (wing.wingRightCooldownTicks > 0) --wing.wingRightCooldownTicks;

    if (amp.tierDecayTicksRemaining > 0) {
        --amp.tierDecayTicksRemaining;
        if (amp.tierDecayTicksRemaining == 0 && amp.tier != AmplitudeTier::Fundamental) {
            amp.tier = (amp.tier == AmplitudeTier::Crescendo) ? AmplitudeTier::Harmonic : AmplitudeTier::Fundamental;
            if (amp.tier != AmplitudeTier::Fundamental) {
                amp.tierDecayTicksRemaining = kAmplitudeDecayTicks;
            }
        }
    }

    if (targetWave.markTicksRemaining > 0) {
        --targetWave.markTicksRemaining;
        if (targetWave.markTicksRemaining == 0) {
            targetWave.hasActiveWaveMark = false;
        }
    }
    if (targetWave.armorShredTicksRemaining > 0) {
        --targetWave.armorShredTicksRemaining;
        if (targetWave.armorShredTicksRemaining == 0) {
            targetWave.armorShredRatio = 0.0f;
        }
    }
    if (targetWave.stunTicksRemaining > 0) {
        --targetWave.stunTicksRemaining;
    }
    if (targetWave.delayedBurstTicksRemaining > 0) {
        --targetWave.delayedBurstTicksRemaining;
        if (targetWave.delayedBurstTicksRemaining == 0 && targetWave.delayedDamagePool > 0.0f) {
            targetStats.hp = std::max(0.0f, targetStats.hp - targetWave.delayedDamagePool);
            targetWave.delayedDamagePool = 0.0f;
            if (targetStats.hp <= 0.0f) targetStats.alive = false;
        }
    }
    if (targetStats.submissiveTicksRemaining > 0) {
        --targetStats.submissiveTicksRemaining;
        if (targetStats.submissiveTicksRemaining == 0 && targetStats.posture == BossPosture::Submissive) {
            targetStats.posture = BossPosture::Neutral;
        }
    }
}

} // namespace en::veil
