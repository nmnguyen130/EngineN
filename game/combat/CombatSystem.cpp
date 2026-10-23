#include "game/combat/CombatSystem.h"
#include "game/combat/WaveReactionResolver.h"

#include <algorithm>
#include <cmath>

namespace vots {

namespace {

constexpr void AddHarmonyClamped(PlayerStateComp& player, u8 amount) noexcept {
    const u32 sum = static_cast<u32>(player.harmony) + static_cast<u32>(amount);
    player.harmony = static_cast<u8>(std::min<u32>(kMaxHarmonyGauge, sum));
}

void InterruptTargetIfAllowed(
    EntityId targetId,
    en::World& world,
    BreakAndCcComp& breakComp) noexcept {
    if (!breakComp.HasRuleFlag(BreakAndCcComp::kFlagInterruptibleWindup)) return;
    breakComp.flags &= static_cast<u8>(~BreakAndCcComp::kFlagInterruptibleWindup);
    if (auto* ai = world.TryGet<EnemyAiComp>(targetId)) {
        ai->turnDelayTicks = 0U;
        ai->actionTimerTicks = 0U;
        ai->cooldownTicks = std::max<u16>(ai->cooldownTicks, 48U);
    }
    if (auto* rig = world.TryGet<en::HitboxRig2D>(targetId)) {
        rig->EndSwing();
    }
}

} // namespace

i32 CombatSystem::ComputeModifiedHpDamage(
    i32 rawHpDamage,
    const BreakAndCcComp* breakComp) noexcept {
    if (rawHpDamage <= 0) return 0;
    f32 mult = 1.0f;
    if (breakComp != nullptr) {
        if (breakComp->fractureTicks > 0U) {
            mult *= kFractureDamageMultiplier;
        }
        if (breakComp->breakPhase == BreakPhase::BreakState) {
            mult *= kBreakStateDamageMultiplier;
        }
    }
    return std::max<i32>(1, static_cast<i32>(static_cast<f32>(rawHpDamage) * mult + 0.5f));
}

bool CombatSystem::ApplyBreakDamage(
    EntityId targetId,
    u8 rawBreak,
    en::World& world,
    BreakAndCcComp& breakComp,
    CombatResolutionReport* outReport) noexcept {
    if (rawBreak == 0U) return false;
    if (const auto* ai = world.TryGet<EnemyAiComp>(targetId)) {
        if (ai->phaseTransitionTicks > 0U) return false;
    }

    if (breakComp.breakPhase != BreakPhase::Normal) {
        return false;
    }

    const f32 scaled = static_cast<f32>(rawBreak) * std::max(0.0f, breakComp.breakResistance);
    const u32 delta = static_cast<u32>(scaled + 0.5f);
    if (delta == 0U) return false;

    const u32 nextBreak = std::min<u32>(kMaxBreakMeter, static_cast<u32>(breakComp.breakMeter) + delta);
    breakComp.breakMeter = static_cast<u8>(nextBreak);
    if (outReport != nullptr) {
        outReport->primaryBreakAdded = static_cast<u8>(
            std::min<u32>(255U, static_cast<u32>(outReport->primaryBreakAdded) + delta));
    }

    if (breakComp.breakMeter < kMaxBreakMeter) {
        return false;
    }

    if (breakComp.category == TargetCategory::Standard) {
        breakComp.breakPhase = BreakPhase::Staggered;
        breakComp.phaseTicks = kStandardStaggerTicks;
        InterruptTargetIfAllowed(targetId, world, breakComp);
        if (auto* fsm = world.TryGet<en::StateMachine2D>(targetId)) {
            fsm->TransitionTo(static_cast<u8>(ActorFsmState::HurtStagger));
        }
        if (outReport != nullptr) {
            outReport->targetEnteredStagger = true;
        }
    } else {
        breakComp.breakMeter = 0U;
        breakComp.breakPhase = BreakPhase::BreakState;
        breakComp.phaseTicks = kEliteBossBreakStateTicks;
        breakComp.lockKind = ActionLockKind::BreakLock;
        breakComp.actionLockTicks = kEliteBossBreakStateTicks;
        InterruptTargetIfAllowed(targetId, world, breakComp);
        if (auto* fsm = world.TryGet<en::StateMachine2D>(targetId)) {
            fsm->TransitionTo(static_cast<u8>(ActorFsmState::BreakExposed));
        }
        if (outReport != nullptr) {
            outReport->targetEnteredBreakState = true;
        }
    }
    return true;
}

void CombatSystem::ApplyCrowdControl(
    EntityId targetId,
    ActionLockKind lockKind,
    u16 durationTicks,
    bool interruptWindup,
    en::World& world,
    BreakAndCcComp& breakComp) noexcept {
    if (interruptWindup) {
        InterruptTargetIfAllowed(targetId, world, breakComp);
    }
    if (lockKind == ActionLockKind::None || durationTicks == 0U) return;

    if (breakComp.category == TargetCategory::Boss && lockKind != ActionLockKind::BreakLock) {
        return;
    }
    if (breakComp.category == TargetCategory::HeavyElite && lockKind == ActionLockKind::MicroStagger) {
        return;
    }

    if (static_cast<u8>(lockKind) < static_cast<u8>(breakComp.lockKind) && breakComp.actionLockTicks > 0U) {
        return;
    }
    if (lockKind == breakComp.lockKind) {
        breakComp.actionLockTicks = std::max(breakComp.actionLockTicks, durationTicks);
    } else {
        breakComp.lockKind = lockKind;
        breakComp.actionLockTicks = durationTicks;
    }
}

void CombatSystem::ApplySlowEffect(
    BreakAndCcComp& breakComp,
    u8 slowPercent,
    u16 durationTicks) noexcept {
    if (slowPercent == 0U || durationTicks == 0U) return;
    if (breakComp.category == TargetCategory::Boss) return;

    if (breakComp.slowTicks > 0U) {
        breakComp.slowPercent = std::max(breakComp.slowPercent, slowPercent);
        breakComp.slowTicks = std::max(breakComp.slowTicks, durationTicks);
    } else {
        breakComp.slowPercent = slowPercent;
        breakComp.slowTicks = durationTicks;
    }
}

void CombatSystem::StepCombatTimersAndDelayedStrikes(
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    (void)pipeline;

    for (auto [id, mark] : world.View<ResonanceMarkComp>()) {
        if (mark.remainingTicks > 0U) {
            if (--mark.remainingTicks == 0U) {
                mark.ClearMark();
            }
        }
        if (mark.reactionVfxTicks > 0U) --mark.reactionVfxTicks;
        if (mark.hitFlashTicks > 0U) --mark.hitFlashTicks;

        if (mark.chronoDelayTicks > 0U) {
            if (--mark.chronoDelayTicks == 0U) {
                const i16 delayedDmg = mark.chronoPendingDamage;
                mark.chronoPendingDamage = 0;
                if (delayedDmg > 0 && world.IsAlive(id)) {
                    auto* recv = world.TryGet<en::DamageReceiver2D>(id);
                    const auto* breakComp = world.TryGet<BreakAndCcComp>(id);
                    if (recv != nullptr && !recv->HasFlag(en::ReceiverFlags::Dead) && recv->hp > 0) {
                        const i32 finalDmg = ComputeModifiedHpDamage(delayedDmg, breakComp);
                        recv->hp = std::max(0, recv->hp - finalDmg);
                        if (recv->hp == 0) recv->AddFlags(en::ReceiverFlags::Dead);
                        mark.hitFlashTicks = 12U;
                    }
                }
            }
        }
    }

    for (auto [id, breakComp] : world.View<BreakAndCcComp>()) {
        if (breakComp.fractureTicks > 0U) --breakComp.fractureTicks;
        if (breakComp.slowTicks > 0U) {
            if (--breakComp.slowTicks == 0U) breakComp.slowPercent = 0U;
        }
        if (breakComp.guardBrokenTicks > 0U) --breakComp.guardBrokenTicks;

        if (breakComp.actionLockTicks > 0U) {
            if (--breakComp.actionLockTicks == 0U && breakComp.breakPhase != BreakPhase::BreakState) {
                breakComp.lockKind = ActionLockKind::None;
            }
        }

        if (breakComp.breakPhase != BreakPhase::Normal && breakComp.phaseTicks > 0U) {
            if (--breakComp.phaseTicks == 0U) {
                if (breakComp.breakPhase == BreakPhase::Staggered) {
                    breakComp.breakMeter = 0U;
                    breakComp.breakPhase = BreakPhase::Normal;
                } else if (breakComp.breakPhase == BreakPhase::BreakState) {
                    breakComp.breakPhase = BreakPhase::BreakRecovery;
                    breakComp.phaseTicks = kEliteBossBreakRecovTicks;
                    if (breakComp.lockKind == ActionLockKind::BreakLock) {
                        breakComp.lockKind = ActionLockKind::None;
                        breakComp.actionLockTicks = 0U;
                    }
                } else if (breakComp.breakPhase == BreakPhase::BreakRecovery) {
                    breakComp.breakPhase = BreakPhase::Normal;
                }
            }
        }

        if (breakComp.pullTicks > 0U) {
            --breakComp.pullTicks;
            if (breakComp.category == TargetCategory::Standard &&
                !breakComp.HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant | BreakAndCcComp::kFlagAnchored)) {
                en::LocalTransform2D local = world.GetLocalTransform(id);
                const f32 dx = breakComp.pullTargetX - local.x;
                const f32 dy = breakComp.pullTargetY - local.y;
                const f32 dist = std::sqrt(dx * dx + dy * dy);
                if (dist > 0.5f) {
                    const f32 stepDist = std::min(dist - 0.5f, 1.5f / static_cast<f32>(kEclipsePullDurationTicks));
                    local.x += (dx / dist) * stepDist;
                    local.y += (dy / dist) * stepDist;
                    world.SetLocalTransform(id, local);
                }
            }
        }
    }
}

CombatResolutionReport CombatSystem::ResolvePlayerWeaponHit(
    EntityId attackerId,
    EntityId primaryTargetId,
    const WeaponAttackComp& attack,
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    CombatResolutionReport report{};
    if (!world.IsAlive(attackerId) || !world.IsAlive(primaryTargetId)) return report;

    auto* recv = world.TryGet<en::DamageReceiver2D>(primaryTargetId);
    if (recv == nullptr || recv->HasFlag(en::ReceiverFlags::Dead) || recv->hp <= 0) {
        return report;
    }

    if (const auto* ai = world.TryGet<EnemyAiComp>(primaryTargetId)) {
        if (ai->phaseTransitionTicks > 0U) return report;
    }

    auto* markComp = world.TryGet<ResonanceMarkComp>(primaryTargetId);
    auto* breakComp = world.TryGet<BreakAndCcComp>(primaryTargetId);
    auto* playerState = world.TryGet<PlayerStateComp>(attackerId);

    if (markComp != nullptr && attack.instanceId != 0U &&
        markComp->lastAppliedAttackInstance == attack.instanceId) {
        return report;
    }
    if (markComp != nullptr) {
        if (attack.instanceId != 0U) {
            markComp->lastAppliedAttackInstance = attack.instanceId;
        }
        markComp->hitFlashTicks = 12U;
    }

    const EchoId hitEcho = attack.capturedEchoId;
    const WaveReactionSpec* reactionSpec = nullptr;

    if (markComp != nullptr && IsValidEcho(hitEcho)) {
        if (!markComp->HasActiveMark() || markComp->activeMark == hitEcho) {
            markComp->ApplyOrRefreshMark(hitEcho, attack.instanceId);
        } else {
            reactionSpec = FindWaveReaction(markComp->activeMark, hitEcho);
            if (reactionSpec != nullptr) {
                markComp->ClearMark();
                markComp->lastReactionTriggered = reactionSpec->id;
                markComp->reactionVfxTicks = 30U;
            } else {
                markComp->ApplyOrRefreshMark(hitEcho, attack.instanceId);
            }
        }
    }

    const i32 rawTotalPrimaryHp = static_cast<i32>(attack.baseHpDamage) +
                                  (reactionSpec != nullptr ? static_cast<i32>(reactionSpec->bonusHpDamage) : 0);
    const i32 modifiedPrimaryHp = ComputeModifiedHpDamage(rawTotalPrimaryHp, breakComp);
    report.primaryHpDamageDealt = modifiedPrimaryHp;

    const u8 totalPrimaryBreak = static_cast<u8>(
        std::min<u32>(255U, static_cast<u32>(attack.baseBreakDamage) +
                            (reactionSpec != nullptr ? static_cast<u32>(reactionSpec->bonusBreak) : 0U)));

    if (breakComp != nullptr) {
        (void)ApplyBreakDamage(primaryTargetId, totalPrimaryBreak, world, *breakComp, &report);
    }

    if (reactionSpec != nullptr) {
        report.reactionTriggered = reactionSpec->id;
        if (playerState != nullptr) {
            AddHarmonyClamped(*playerState, kReactionHarmonyGain);
        }
        WaveReactionResolver::ApplySignatureReactionPayload(primaryTargetId, *reactionSpec, world, pipeline, report);
    }

    // Hollow Knight-style Attacker Nail Recoil + Defender Hit Knockback for tactile combat spacing
    const f32 dxToTarget = world.GetWorldByIndex(primaryTargetId.index).x - world.GetWorldByIndex(attackerId.index).x;
    const f32 strikeDir = (dxToTarget >= 0.0f) ? 1.0f : -1.0f;
    if (auto* attMotion = world.TryGet<en::MotionStack2D>(attackerId)) {
        attMotion->AddKnockbackImpulse(-strikeDir * kPlayerAttackHitRecoilVx, 0.0f);
    }
    if (auto* attMotor = world.TryGet<en::CharacterMotor2D>(attackerId)) {
        attMotor->vx = -strikeDir * kPlayerAttackHitRecoilVx;
    }
    if (breakComp != nullptr &&
        !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant | BreakAndCcComp::kFlagAnchored)) {
        if (auto* defMotion = world.TryGet<en::MotionStack2D>(primaryTargetId)) {
            defMotion->AddKnockbackImpulse(strikeDir * kEnemyHitKnockbackVx, 0.0f);
        }
        if (auto* defMotor = world.TryGet<en::CharacterMotor2D>(primaryTargetId)) {
            defMotor->vx = strikeDir * kEnemyHitKnockbackVx;
        }
    }

    recv->hp -= modifiedPrimaryHp;
    if (recv->hp <= 0) {
        recv->hp = 0;
        recv->AddFlags(en::ReceiverFlags::Dead);
        if (markComp != nullptr) {
            markComp->chronoDelayTicks = 0U;
            markComp->chronoPendingDamage = 0;
        }
    }

    return report;
}

CombatResolutionReport CombatSystem::ResolveEnemyAttackAgainstPlayer(
    EntityId attackerId,
    EntityId playerId,
    i16 attackHpDamage,
    u8 attackBreakDamage,
    AttackTelegraphKind telegraph,
    EchoId counterEchoId,
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    (void)attackBreakDamage;
    CombatResolutionReport report{};
    if (!world.IsAlive(playerId)) return report;

    auto* playerState = world.TryGet<PlayerStateComp>(playerId);
    auto* playerRecv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (playerRecv == nullptr || playerRecv->HasFlag(en::ReceiverFlags::Dead)) return report;

    const bool canParryTelegraph = (telegraph != AttackTelegraphKind::CrimsonOverloadWave);
    const bool inParryWindow = (playerState != nullptr && playerState->parryWindowTicks > 0U);

    if (canParryTelegraph && inParryWindow) {
        report.parrySucceeded = true;
        playerState->parryWindowTicks = 0U;
        playerState->dashLockoutTicks = 0U;
        playerRecv->RemoveFlags(en::ReceiverFlags::Parrying);
        playerRecv->iframeTicks = std::max<u16>(playerRecv->iframeTicks, 18U);
        AddHarmonyClamped(*playerState, kParryHarmonyGain);

        u8 breakToAttacker = kParryBaseBreakDamage;
        if (IsValidEcho(counterEchoId) && playerState->capturedParryEcho == counterEchoId) {
            breakToAttacker = static_cast<u8>(breakToAttacker + kParryPhaseInversionBonus);
            report.phaseInversionBonus = true;
        }

        if (world.IsAlive(attackerId)) {
            if (auto* attBreak = world.TryGet<BreakAndCcComp>(attackerId)) {
                InterruptTargetIfAllowed(attackerId, world, *attBreak);
                (void)ApplyBreakDamage(attackerId, breakToAttacker, world, *attBreak, &report);
                if (attBreak->category == TargetCategory::Standard && attBreak->actionLockTicks < 24U) {
                    ApplyCrowdControl(attackerId, ActionLockKind::MicroStagger, 24U, true, world, *attBreak);
                }
            }
            if (auto* attMotion = world.TryGet<en::MotionStack2D>(attackerId)) {
                const f32 dx = world.GetWorldByIndex(attackerId.index).x - world.GetWorldByIndex(playerId.index).x;
                attMotion->AddKnockbackImpulse(((dx >= 0.0f) ? 1.0f : -1.0f) * 3.8f, 0.0f);
            }
        }

        if (auto* fsm = world.TryGet<en::StateMachine2D>(playerId)) {
            fsm->TransitionTo(static_cast<u8>(ActorFsmState::ParrySuccess));
        }

        en::SimCue parryCue{};
        parryCue.kind = en::SimCueKind::AudioOneShot;
        parryCue.assetId = en::AssetId::Hash(report.phaseInversionBonus ? "sfx_phase_counter" : "sfx_parry_clang");
        parryCue.emitter = playerId;
        pipeline.EmitCue(parryCue);
        return report;
    }

    if (playerRecv->HasFlag(en::ReceiverFlags::Invulnerable) || playerRecv->iframeTicks > 0U) {
        return report;
    }

    const i32 dmg = std::max<i32>(0, attackHpDamage);
    playerRecv->hp -= dmg;
    playerRecv->iframeTicks = kPlayerHitProtectionTicks;
    playerRecv->stunTicks = kPlayerHitstunTicks;
    report.primaryHpDamageDealt = dmg;

    if (auto* atk = world.TryGet<WeaponAttackComp>(playerId)) {
        atk->moveKind = WeaponMoveKind::None;
        atk->elapsedTicks = 0U;
        atk->packedHitAndChain = 0U;
    }
    if (auto* rig = world.TryGet<en::HitboxRig2D>(playerId)) {
        rig->EndSwing();
    }
    if (playerState != nullptr) {
        playerState->dashActiveTicks = 0U;
        playerState->dashLockoutTicks = 0U;
    }

    // Clear Directional Knockback Arc (Horizontal push away + upward pop) & Camera Trauma Pulse so hits are unmistakable!
    f32 awaySign = -1.0f;
    if (world.IsAlive(attackerId)) {
        const f32 dx = world.GetWorldByIndex(playerId.index).x - world.GetWorldByIndex(attackerId.index).x;
        awaySign = (dx >= 0.0f) ? 1.0f : -1.0f;
    }
    if (auto* motion = world.TryGet<en::MotionStack2D>(playerId)) {
        motion->overrideTicks = 0U;
        motion->AddKnockbackImpulse(awaySign * kPlayerHitKnockbackAwayVx, kPlayerHitKnockbackUpVy);
    }
    if (auto* motor = world.TryGet<en::CharacterMotor2D>(playerId)) {
        motor->vx = awaySign * kPlayerHitKnockbackAwayVx;
        motor->vy = std::max(motor->vy, kPlayerHitKnockbackUpVy);
    }

    en::SimCue hurtCue{};
    hurtCue.kind = en::SimCueKind::CameraTraumaPulse;
    hurtCue.emitter = playerId;
    hurtCue.SetMagnitude(0.45f);
    pipeline.EmitCue(hurtCue);

    if (playerState != nullptr && playerState->unisonState != UnisonActionState::None) {
        if (playerState->unisonState == UnisonActionState::StandardUnison &&
            playerState->unisonElapsedTicks < kUnisonResolveTick) {
            AddHarmonyClamped(*playerState, kUnisonHarmonyCost);
            report.unisonRefundedOnHitstun = true;
        } else if (playerState->unisonState == UnisonActionState::SacredChord &&
                   playerState->unisonElapsedTicks < kSacredChordResolveTick) {
            AddHarmonyClamped(*playerState, kSacredChordHarmonyCost);
            report.unisonRefundedOnHitstun = true;
        }
        playerState->unisonState = UnisonActionState::None;
        playerState->unisonElapsedTicks = 0U;
    }

    if (playerRecv->hp <= 0) {
        playerRecv->hp = 0;
        playerRecv->AddFlags(en::ReceiverFlags::Dead);
    }
    return report;
}

bool CombatSystem::BeginUnisonOrSacredChord(
    EntityId playerId,
    UnisonActionState kind,
    en::World& world) noexcept {
    if (!world.IsAlive(playerId)) return false;
    if (kind != UnisonActionState::StandardUnison && kind != UnisonActionState::SacredChord) {
        return false;
    }
    auto* playerState = world.TryGet<PlayerStateComp>(playerId);
    const auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (playerState == nullptr || recv == nullptr) return false;
    if (recv->HasFlag(en::ReceiverFlags::Dead) || recv->stunTicks > 0U) return false;
    if (playerState->IsInDashLockout() || playerState->unisonState != UnisonActionState::None) {
        return false;
    }

    if (kind == UnisonActionState::StandardUnison) {
        if (playerState->harmony < kUnisonHarmonyCost) return false;
        playerState->harmony = static_cast<u8>(playerState->harmony - kUnisonHarmonyCost);
    } else {
        if (playerState->harmony < kSacredChordHarmonyCost) return false;
        if (FindMatchingSacredChord(playerState->deck) == nullptr) return false;
        playerState->harmony = static_cast<u8>(playerState->harmony - kSacredChordHarmonyCost);
    }

    playerState->unisonState = kind;
    playerState->unisonElapsedTicks = 0U;
    return true;
}

void CombatSystem::StepActiveUnisonOrChord(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline,
    CombatResolutionReport* outReport) noexcept {
    if (!world.IsAlive(playerId)) return;
    auto* playerState = world.TryGet<PlayerStateComp>(playerId);
    if (playerState == nullptr || playerState->unisonState == UnisonActionState::None) return;

    ++playerState->unisonElapsedTicks;
    if (playerState->unisonState == UnisonActionState::StandardUnison) {
        if (playerState->unisonElapsedTicks == kUnisonResolveTick) {
            playerState->harmony = static_cast<u8>(
                std::min<u32>(kMaxHarmonyGauge, static_cast<u32>(playerState->harmony) + kUnisonHarmonyCost));
            (void)TryActivateUnisonBurst(playerId, world, pipeline, outReport);
        }
        if (playerState->unisonElapsedTicks >= kUnisonTotalTicks) {
            playerState->unisonState = UnisonActionState::None;
            playerState->unisonElapsedTicks = 0U;
        }
    } else if (playerState->unisonState == UnisonActionState::SacredChord) {
        if (playerState->unisonElapsedTicks == kSacredChordResolveTick) {
            playerState->harmony = static_cast<u8>(
                std::min<u32>(kMaxHarmonyGauge, static_cast<u32>(playerState->harmony) + kSacredChordHarmonyCost));
            (void)TryActivateSacredChord(playerId, world, pipeline, outReport);
        }
        if (playerState->unisonElapsedTicks >= kSacredChordTotalTicks) {
            playerState->unisonState = UnisonActionState::None;
            playerState->unisonElapsedTicks = 0U;
        }
    }
}

bool CombatSystem::TryActivateUnisonBurst(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline,
    CombatResolutionReport* outReport) noexcept {
    return WaveReactionResolver::ExecuteUnisonBurst(playerId, world, pipeline, outReport);
}

bool CombatSystem::TryActivateSacredChord(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline,
    CombatResolutionReport* outReport) noexcept {
    return WaveReactionResolver::ExecuteSacredChord(playerId, world, pipeline, outReport);
}

void CombatSystem::OnEngineCombatHitCallback(
    const en::CombatHitEvent& hit,
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    if (!world.IsAlive(hit.attacker) || !world.IsAlive(hit.defender)) return;

    // Revert default damage applied by EngineN's StateAndCombatSolver2D before applying GDD v0.3/v0.5 rules
    if (auto* defRecv = world.TryGet<en::DamageReceiver2D>(hit.defender)) {
        if (hit.damageApplied > 0) {
            defRecv->hp = std::min<i32>(defRecv->maxHp, defRecv->hp + hit.damageApplied);
            if (defRecv->hp > 0) {
                defRecv->RemoveFlags(en::ReceiverFlags::Dead);
            }
        }
    }

    if (world.Has<PlayerStateComp>(hit.attacker)) {
        if (const auto* atk = world.TryGet<WeaponAttackComp>(hit.attacker)) {
            (void)ResolvePlayerWeaponHit(hit.attacker, hit.defender, *atk, world, pipeline);
        }
    } else if (world.Has<PlayerStateComp>(hit.defender)) {
        if (const auto* atk = world.TryGet<WeaponAttackComp>(hit.attacker)) {
            if (auto* pRecv = world.TryGet<en::DamageReceiver2D>(hit.defender)) {
                pRecv->iframeTicks = 0U;
            }
            (void)ResolveEnemyAttackAgainstPlayer(
                hit.attacker,
                hit.defender,
                atk->baseHpDamage,
                atk->baseBreakDamage,
                atk->telegraph,
                atk->counterEchoId,
                world,
                pipeline);
        }
    }
}

} // namespace vots
