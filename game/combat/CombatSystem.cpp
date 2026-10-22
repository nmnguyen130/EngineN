#include "game/combat/CombatSystem.h"

#include <algorithm>
#include <cmath>

namespace vots {

namespace {

constexpr void AddHarmonyClamped(PlayerStateComp& player, u8 amount) noexcept {
    const u32 sum = static_cast<u32>(player.harmony) + static_cast<u32>(amount);
    player.harmony = static_cast<u8>(std::min<u32>(kMaxHarmonyGauge, sum));
}

void ApplyDirectHpDamage(
    en::DamageReceiver2D& recv,
    const BreakAndCcComp* breakComp,
    i32 baseHpDamage) noexcept {
    if (baseHpDamage <= 0 || recv.HasFlag(en::ReceiverFlags::Dead)) return;
    const i32 finalDmg = CombatSystem::ComputeModifiedHpDamage(baseHpDamage, breakComp);
    recv.hp -= finalDmg;
    if (recv.hp <= 0) {
        recv.hp = 0;
        recv.AddFlags(en::ReceiverFlags::Dead);
    }
}

void InterruptTargetIfAllowed(
    EntityId targetId,
    en::World& world,
    BreakAndCcComp& breakComp) noexcept {
    if (!breakComp.HasRuleFlag(BreakAndCcComp::kFlagInterruptibleWindup)) return;
    breakComp.flags &= static_cast<u8>(~BreakAndCcComp::kFlagInterruptibleWindup);
    if (auto* ai = world.TryGet<EnemyAiComp>(targetId)) {
        ai->windupInterrupted = 1U;
        ai->actionTimerTicks = 0U;
    }
    if (auto* rig = world.TryGet<en::HitboxRig2D>(targetId)) {
        rig->EndSwing();
    }
}

// Collects up to maxCount living enemy entities within radiusMeters of center, sorted by ascending distance.
[[nodiscard]] u32 GatherLivingEnemiesInRadius(
    en::World& world,
    Vec2 center,
    f32 radiusMeters,
    EntityId excludeEntity,
    std::span<EntityId> outTargets) noexcept {
    struct Candidate {
        EntityId id{kNullEntity};
        f32 distSq{0.0f};
    };
    std::array<Candidate, 32> candidates{};
    u32 candCount = 0U;
    const f32 rSq = radiusMeters * radiusMeters;

    for (auto [id, recv, breakComp] : world.View<en::DamageReceiver2D, BreakAndCcComp>()) {
        (void)breakComp;
        if (id == excludeEntity) continue;
        if (recv.TeamId() == 1U || recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) continue;

        const en::WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        const f32 dx = xf.x - center.x;
        const f32 dy = xf.y - center.y;
        const f32 dSq = dx * dx + dy * dy;
        if (dSq <= rSq && candCount < candidates.size()) {
            candidates[candCount++] = Candidate{id, dSq};
        }
    }

    std::sort(candidates.begin(), candidates.begin() + candCount,
              [](const Candidate& a, const Candidate& b) noexcept {
                  if (std::abs(a.distSq - b.distSq) > 1e-5f) return a.distSq < b.distSq;
                  return a.id.index < b.id.index;
              });

    const u32 count = std::min<u32>(candCount, static_cast<u32>(outTargets.size()));
    for (u32 i = 0U; i < count; ++i) {
        outTargets[i] = candidates[i].id;
    }
    return count;
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

    // GDD v0.2 Section 4.3: Ignore all incoming Break during Stagger, Break State, and Break Recovery.
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
        // Standard enemy enters Stagger for 45 ticks; Break meter resets to 0 when Stagger ends.
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
        // Heavy/Elite and Boss enter Break State for 150 ticks; Break meter resets to 0 immediately on entry.
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

    // GDD v0.2 Section 4.3: Bosses are immune to standard Stun and Micro-stagger.
    if (breakComp.category == TargetCategory::Boss && lockKind != ActionLockKind::BreakLock) {
        return;
    }
    if (breakComp.category == TargetCategory::HeavyElite && lockKind == ActionLockKind::MicroStagger) {
        return;
    }

    // Priority order: BreakLock > Stun > MicroStagger. Lower priority cannot shorten or overwrite higher priority.
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

    // 1. Step ResonanceMarkComp: expire marks reaching 0 BEFORE any hit checks on this tick, and resolve Chrono-Overdrive.
    for (auto [id, mark] : world.View<ResonanceMarkComp>()) {
        if (mark.remainingTicks > 0U) {
            if (--mark.remainingTicks == 0U) {
                mark.ClearMark();
            }
        }
        if (mark.reactionVfxTicks > 0U) {
            --mark.reactionVfxTicks;
        }
        if (mark.hitFlashTicks > 0U) {
            --mark.hitFlashTicks;
        }
        if (mark.chronoDelayTicks > 0U) {
            if (--mark.chronoDelayTicks == 0U) {
                const i16 delayedDmg = mark.chronoPendingDamage;
                mark.chronoPendingDamage = 0;
                if (delayedDmg > 0 && world.IsAlive(id)) {
                    auto* recv = world.TryGet<en::DamageReceiver2D>(id);
                    const auto* breakComp = world.TryGet<BreakAndCcComp>(id);
                    if (recv != nullptr && !recv->HasFlag(en::ReceiverFlags::Dead) && recv->hp > 0) {
                        // Chrono-Overdrive delayed hit: deals 10 HP Damage (scaled by active modifiers), 0 Break, no mark, no Harmony.
                        ApplyDirectHpDamage(*recv, breakComp, delayedDmg);
                        mark.hitFlashTicks = 8U;
                    }
                }
            }
        }
    }

    // 2. Step BreakAndCcComp timers, Break State -> Break Recovery transitions, and Eclipse/Singularity pulls.
    for (auto [id, breakComp] : world.View<BreakAndCcComp>()) {
        if (breakComp.fractureTicks > 0U) --breakComp.fractureTicks;
        if (breakComp.slowTicks > 0U) {
            if (--breakComp.slowTicks == 0U) {
                breakComp.slowPercent = 0U;
            }
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

    auto* markComp = world.TryGet<ResonanceMarkComp>(primaryTargetId);
    auto* breakComp = world.TryGet<BreakAndCcComp>(primaryTargetId);
    auto* playerState = world.TryGet<PlayerStateComp>(attackerId);

    // Prevent a single attack instance from applying marks or hitting the same target multiple times.
    if (markComp != nullptr && attack.instanceId != 0U &&
        markComp->lastAppliedAttackInstance == attack.instanceId) {
        return report;
    }
    if (markComp != nullptr) {
        if (attack.instanceId != 0U) {
            markComp->lastAppliedAttackInstance = attack.instanceId;
        }
        markComp->hitFlashTicks = 10U;
    }

    const EchoId hitEcho = attack.capturedEchoId;
    const WaveReactionSpec* reactionSpec = nullptr;

    if (markComp != nullptr && IsValidEcho(hitEcho)) {
        if (!markComp->HasActiveMark()) {
            markComp->ApplyOrRefreshMark(hitEcho, attack.instanceId);
        } else if (markComp->activeMark == hitEcho) {
            // Same Echo: refresh duration to 420 ticks, do not trigger a reaction.
            markComp->ApplyOrRefreshMark(hitEcho, attack.instanceId);
        } else {
            reactionSpec = FindWaveReaction(markComp->activeMark, hitEcho);
            if (reactionSpec != nullptr) {
                // Valid reaction pair: consume both old and new marks.
                markComp->ClearMark();
                markComp->lastReactionTriggered = reactionSpec->id;
                markComp->reactionVfxTicks = 30U;
            } else {
                // Invalid reaction pair: replace old mark with new Echo at 420 ticks.
                markComp->ApplyOrRefreshMark(hitEcho, attack.instanceId);
            }
        }
    }

    // 1. Resolve primary hit base HP Damage + Reaction bonus HP Damage.
    // Note: Melt-Shock applies Fracture for subsequent attacks, so apply primary hit damage before setting Fracture,
    // while Break State (1.25x) and any pre-existing Fracture (1.15x) apply to this hit + reaction bonus.
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

    // 2. If a Signature Reaction triggered, execute its exact GDD v0.2 Section 4.4 effects BEFORE despawn/death cleanup.
    if (reactionSpec != nullptr) {
        report.reactionTriggered = reactionSpec->id;
        if (playerState != nullptr) {
            AddHarmonyClamped(*playerState, kReactionHarmonyGain);
        }

        const en::WorldTransform2D& primaryXf = world.GetWorldByIndex(primaryTargetId.index);
        const Vec2 primaryPos{primaryXf.x, primaryXf.y};

        en::SimCue reactCue{};
        reactCue.kind = en::SimCueKind::AudioOneShot;
        reactCue.assetId = en::AssetId::Hash("sfx_reaction_burst");
        reactCue.worldX = primaryPos.x;
        reactCue.worldY = primaryPos.y;
        reactCue.emitter = primaryTargetId;
        pipeline.EmitCue(reactCue);

        switch (reactionSpec->id) {
            case WaveReactionId::MeltShock: {
                if (breakComp != nullptr) {
                    breakComp->fractureTicks = kFractureDurationTicks;
                }
                break;
            }
            case WaveReactionId::PiezoQuake: {
                if (breakComp != nullptr) {
                    ApplyCrowdControl(primaryTargetId, ActionLockKind::Stun, kPiezoQuakeStunTicks, true, world, *breakComp);
                }
                break;
            }
            case WaveReactionId::SteamCavitation: {
                if (breakComp != nullptr &&
                    breakComp->category == TargetCategory::Standard &&
                    !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant)) {
                    if (auto* ms = world.TryGet<en::MotionStack2D>(primaryTargetId)) {
                        ms->AddKnockbackImpulse(2.5f, 0.0f);
                    }
                }
                std::array<EntityId, 2> secTargets{};
                const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 1.75f, primaryTargetId, secTargets);
                for (u32 i = 0U; i < count; ++i) {
                    const EntityId secId = secTargets[i];
                    auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                    auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                    if (secRecv != nullptr) {
                        ApplyDirectHpDamage(*secRecv, secBreak, 8);
                        ++report.secondaryTargetsHit;
                    }
                    if (secBreak != nullptr &&
                        secBreak->category == TargetCategory::Standard &&
                        !secBreak->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant)) {
                        const en::WorldTransform2D& sxf = world.GetWorldByIndex(secId.index);
                        const f32 dirSign = (sxf.x >= primaryPos.x) ? 1.0f : -1.0f;
                        if (auto* sms = world.TryGet<en::MotionStack2D>(secId)) {
                            sms->AddKnockbackImpulse(dirSign * 2.5f, 0.0f);
                        }
                    }
                }
                break;
            }
            case WaveReactionId::SuperconductorGale: {
                std::array<EntityId, 2> secTargets{};
                const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 2.5f, primaryTargetId, secTargets);
                for (u32 i = 0U; i < count; ++i) {
                    const EntityId secId = secTargets[i];
                    auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                    auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                    if (secRecv != nullptr) {
                        ApplyDirectHpDamage(*secRecv, secBreak, 9);
                        ++report.secondaryTargetsHit;
                    }
                    if (secBreak != nullptr) {
                        (void)ApplyBreakDamage(secId, 4U, world, *secBreak);
                    }
                }
                break;
            }
            case WaveReactionId::PyroCyclone: {
                std::array<EntityId, 4> secTargets{};
                const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 2.5f, primaryTargetId, secTargets);
                for (u32 i = 0U; i < count; ++i) {
                    const EntityId secId = secTargets[i];
                    auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                    const auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                    if (secRecv != nullptr) {
                        ApplyDirectHpDamage(*secRecv, secBreak, 12);
                        ++report.secondaryTargetsHit;
                    }
                }
                // Clear up to 3 eligible normal projectiles within 2.5m (never clear protected Boss projectiles/traps).
                const f32 rSq = 2.5f * 2.5f;
                for (auto [projId, proj] : world.View<ProjectileComp>()) {
                    if (report.projectilesCleared >= 3U) break;
                    if (proj.active == 0U || proj.protectedFromClear != 0U) continue;
                    const en::WorldTransform2D& pxf = world.GetWorldByIndex(projId.index);
                    const f32 dx = pxf.x - primaryPos.x;
                    const f32 dy = pxf.y - primaryPos.y;
                    if (dx * dx + dy * dy <= rSq) {
                        proj.active = 0U;
                        (void)world.DeferDestroy(projId);
                        ++report.projectilesCleared;
                    }
                }
                break;
            }
            case WaveReactionId::TectonicShatter: {
                if (breakComp != nullptr) {
                    ApplyCrowdControl(primaryTargetId, ActionLockKind::MicroStagger, kTectonicMicroStaggerTicks, true, world, *breakComp);
                    if (breakComp->HasRuleFlag(BreakAndCcComp::kFlagBreakableGuard) &&
                        breakComp->HasRuleFlag(BreakAndCcComp::kFlagGuardActive)) {
                        breakComp->flags &= static_cast<u8>(~BreakAndCcComp::kFlagGuardActive);
                        breakComp->guardBrokenTicks = kGuardBrokenLockoutTicks;
                    }
                }
                break;
            }
            case WaveReactionId::EclipseSingularity: {
                std::array<EntityId, 4> secTargets{};
                const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 3.5f, primaryTargetId, secTargets);
                for (u32 i = 0U; i < count; ++i) {
                    const EntityId secId = secTargets[i];
                    auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                    if (secBreak != nullptr &&
                        secBreak->category == TargetCategory::Standard &&
                        !secBreak->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant | BreakAndCcComp::kFlagAnchored)) {
                        secBreak->pullTargetX = primaryPos.x;
                        secBreak->pullTargetY = primaryPos.y;
                        secBreak->pullTicks = kEclipsePullDurationTicks;
                        ++report.secondaryTargetsHit;
                    }
                }
                break;
            }
            case WaveReactionId::GravitonQuake: {
                if (breakComp != nullptr &&
                    breakComp->category == TargetCategory::Standard &&
                    !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant)) {
                    if (auto* motor = world.TryGet<en::CharacterMotor2D>(primaryTargetId)) {
                        if (!motor->IsGrounded()) {
                            motor->vy = std::min(motor->vy, -18.0f);
                        }
                    }
                }
                std::array<EntityId, 4> secTargets{};
                const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 2.0f, primaryTargetId, secTargets);
                for (u32 i = 0U; i < count; ++i) {
                    const EntityId secId = secTargets[i];
                    auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                    const auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                    if (secRecv != nullptr) {
                        ApplyDirectHpDamage(*secRecv, secBreak, 8);
                        ++report.secondaryTargetsHit;
                    }
                }
                break;
            }
            case WaveReactionId::ChronoOverdrive: {
                if (markComp != nullptr) {
                    markComp->chronoDelayTicks = kChronoOverdriveDelayTicks;
                    markComp->chronoPendingDamage = 10;
                }
                break;
            }
            case WaveReactionId::SanguineBoil: {
                std::array<EntityId, 2> secTargets{};
                const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 2.0f, primaryTargetId, secTargets);
                for (u32 i = 0U; i < count; ++i) {
                    const EntityId secId = secTargets[i];
                    auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                    const auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                    if (secRecv != nullptr) {
                        ApplyDirectHpDamage(*secRecv, secBreak, 8);
                        ++report.secondaryTargetsHit;
                    }
                }
                break;
            }
            case WaveReactionId::None:
                break;
        }
    }

    // 3. Apply primary HP Damage after reaction secondary query so primary killing blow still fires its reaction.
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

    // GDD v0.2 Section 4.5: Parry succeeds iff input was issued within 10 ticks AND attack is NOT CrimsonOverloadWave.
    const bool canParryTelegraph = (telegraph != AttackTelegraphKind::CrimsonOverloadWave);
    const bool inParryWindow = (playerState != nullptr && playerState->parryWindowTicks > 0U);

    if (canParryTelegraph && inParryWindow) {
        report.parrySucceeded = true;
        playerState->parryWindowTicks = 0U;
        playerRecv->RemoveFlags(en::ReceiverFlags::Parrying);
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

    // Failed parry or normal hit: no hidden block or invulnerability when Parry is mistimed.
    if (playerRecv->HasFlag(en::ReceiverFlags::Invulnerable) || playerRecv->iframeTicks > 0U) {
        return report;
    }

    const i32 dmg = std::max<i32>(0, attackHpDamage);
    playerRecv->hp -= dmg;
    playerRecv->iframeTicks = playerRecv->autoIframeTicks;
    report.primaryHpDamageDealt = dmg;
    if (playerRecv->hp <= 0) {
        playerRecv->hp = 0;
        playerRecv->AddFlags(en::ReceiverFlags::Dead);
    }
    return report;
}

bool CombatSystem::TryActivateUnisonBurst(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline,
    CombatResolutionReport* outReport) noexcept {
    if (!world.IsAlive(playerId)) return false;
    auto* playerState = world.TryGet<PlayerStateComp>(playerId);
    if (playerState == nullptr || playerState->harmony < kUnisonHarmonyCost) {
        return false;
    }

    playerState->harmony = static_cast<u8>(playerState->harmony - kUnisonHarmonyCost);
    const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
    const Vec2 center{pxf.x, pxf.y};

    std::array<EntityId, 5> targets{};
    const u32 count = GatherLivingEnemiesInRadius(world, center, 3.0f, playerId, targets);
    for (u32 i = 0U; i < count; ++i) {
        const EntityId tid = targets[i];
        auto* recv = world.TryGet<en::DamageReceiver2D>(tid);
        auto* breakComp = world.TryGet<BreakAndCcComp>(tid);
        if (recv != nullptr) {
            ApplyDirectHpDamage(*recv, breakComp, 18);
        }
        if (breakComp != nullptr) {
            (void)ApplyBreakDamage(tid, 15U, world, *breakComp);
            if (breakComp->category == TargetCategory::Standard) {
                ApplyCrowdControl(tid, ActionLockKind::MicroStagger, kUnisonInterruptTicks, true, world, *breakComp);
            }
        }
        if (outReport != nullptr) {
            ++outReport->secondaryTargetsHit;
        }
    }

    en::SimCue cue{};
    cue.kind = en::SimCueKind::AudioOneShot;
    cue.assetId = en::AssetId::Hash("sfx_unison_pulse");
    cue.worldX = center.x;
    cue.worldY = center.y;
    cue.emitter = playerId;
    pipeline.EmitCue(cue);
    return true;
}

bool CombatSystem::TryActivateSacredChord(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline,
    CombatResolutionReport* outReport) noexcept {
    if (!world.IsAlive(playerId)) return false;
    auto* playerState = world.TryGet<PlayerStateComp>(playerId);
    if (playerState == nullptr || playerState->harmony < kSacredChordHarmonyCost) {
        return false;
    }

    const SacredChordSpec* chord = FindMatchingSacredChord(playerState->deck);
    if (chord == nullptr) {
        return false;
    }

    playerState->harmony = 0U;
    if (outReport != nullptr) {
        outReport->chordTriggered = chord->id;
    }

    const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
    const Vec2 center{pxf.x, pxf.y};

    std::array<EntityId, 5> targets{};
    const u32 maxT = std::min<u32>(chord->maxTargets, static_cast<u32>(targets.size()));
    const u32 count = GatherLivingEnemiesInRadius(
        world, center, chord->radiusMeters, playerId, std::span<EntityId>{targets.data(), maxT});

    for (u32 i = 0U; i < count; ++i) {
        const EntityId tid = targets[i];
        auto* recv = world.TryGet<en::DamageReceiver2D>(tid);
        auto* breakComp = world.TryGet<BreakAndCcComp>(tid);

        if (chord->id == SacredChordId::EclipseOfEternity) {
            // 3 phantom slashes of 8 HP Damage each (total 24 HP Damage) and 18 Break.
            if (recv != nullptr) {
                for (u32 s = 0U; s < 3U; ++s) {
                    ApplyDirectHpDamage(*recv, breakComp, 8);
                }
            }
        } else if (recv != nullptr) {
            ApplyDirectHpDamage(*recv, breakComp, chord->totalHpDamage);
        }

        if (breakComp != nullptr) {
            (void)ApplyBreakDamage(tid, chord->breakDamage, world, *breakComp);

            switch (chord->id) {
                case SacredChordId::ElementalCataclysm: {
                    if (breakComp->category == TargetCategory::Standard &&
                        !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant)) {
                        const en::WorldTransform2D& txf = world.GetWorldByIndex(tid.index);
                        const f32 dirSign = (txf.x >= center.x) ? 1.0f : -1.0f;
                        if (auto* ms = world.TryGet<en::MotionStack2D>(tid)) {
                            ms->AddKnockbackImpulse(dirSign * 2.0f, 0.0f);
                        }
                    }
                    break;
                }
                case SacredChordId::CryoTempestProtocol: {
                    ApplySlowEffect(*breakComp, 40U, kCryoTempestSlowTicks);
                    break;
                }
                case SacredChordId::EclipseOfEternity: {
                    if (breakComp->category == TargetCategory::Standard) {
                        ApplyCrowdControl(tid, ActionLockKind::Stun, kEclipseChordStunTicks, true, world, *breakComp);
                    }
                    break;
                }
                case SacredChordId::SingularityOverdrive: {
                    if (breakComp->category == TargetCategory::Standard &&
                        !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant | BreakAndCcComp::kFlagAnchored)) {
                        breakComp->pullTargetX = center.x;
                        breakComp->pullTargetY = center.y;
                        breakComp->pullTicks = kSingularityPullTicks;
                    }
                    break;
                }
                case SacredChordId::None:
                    break;
            }
        }

        if (outReport != nullptr) {
            ++outReport->secondaryTargetsHit;
        }
    }

    // Elemental Cataclysm damages flagged destructible terrain within 4.0m.
    if (chord->id == SacredChordId::ElementalCataclysm) {
        const f32 rSq = chord->radiusMeters * chord->radiusMeters;
        for (auto [terId, ter] : world.View<DestructibleTerrainComp>()) {
            if (ter.broken != 0U) continue;
            const en::WorldTransform2D& txf = world.GetWorldByIndex(terId.index);
            const f32 dx = txf.x - center.x;
            const f32 dy = txf.y - center.y;
            if (dx * dx + dy * dy <= rSq) {
                ter.durability -= chord->totalHpDamage;
                if (ter.durability <= 0) {
                    ter.durability = 0;
                    ter.broken = 1U;
                    if (ter.unlockedWorldFlagOnBreak != 0U) {
                        pipeline.SetWorldFlag(ter.unlockedWorldFlagOnBreak, true);
                    }
                    (void)world.DeferDestroy(terId);
                    if (outReport != nullptr) {
                        ++outReport->destructiblesBroken;
                    }
                }
            }
        }
    }

    en::SimCue cue{};
    cue.kind = en::SimCueKind::AudioOneShot;
    cue.assetId = en::AssetId::Hash("sfx_sacred_chord");
    cue.worldX = center.x;
    cue.worldY = center.y;
    cue.emitter = playerId;
    pipeline.EmitCue(cue);
    return true;
}

void CombatSystem::OnEngineCombatHitCallback(
    const en::CombatHitEvent& hit,
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    if (!world.IsAlive(hit.attacker) || !world.IsAlive(hit.defender)) return;

    // Undo EngineN's default raw damage subtraction on DamageReceiver2D if our GDD v0.2 WeaponAttackComp is present,
    // so GDD v0.2's exact HP/Break/Reaction/Parry contract is authoritative.
    if (auto* defRecv = world.TryGet<en::DamageReceiver2D>(hit.defender)) {
        if (hit.damageApplied > 0) {
            defRecv->hp += hit.damageApplied;
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
