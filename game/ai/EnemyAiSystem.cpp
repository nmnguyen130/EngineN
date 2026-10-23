#include "game/ai/EnemyAiSystem.h"
#include "framework/FrameworkTypes.h"
#include "game/combat/CombatSystem.h"
#include "game/world/MetroidvaniaSystem.h"

#include <algorithm>
#include <cmath>

namespace vots {

namespace {

[[nodiscard]] en::Result<EntityId, en::WorldErrc> SpawnEnemyCommon(
    en::World& world,
    en::BlobHandle rigBlob,
    en::AtlasHandle atlas,
    Vec2 pos,
    Vec2 halfExtents,
    Color tint,
    i32 hp,
    TargetCategory category,
    u8 ruleFlags,
    EnemyArchetype archetype,
    EnemyAttackId initialAttack,
    f32 attackRange,
    u16 shardReward,
    u8 roomId,
    u8 spawnSlotBit) noexcept {
    en::EntitySpawnDesc sp{};
    sp.local.x = pos.x;
    sp.local.y = pos.y;
    sp.timeChannel = en::TimeChannel::Enemy;

    auto idRes = world.Spawn(sp);
    if (!idRes.has_value()) return idRes;
    const EntityId id = *idRes;

    en::Sprite2D spr{};
    spr.width = halfExtents.x * 2.0f;
    spr.height = halfExtents.y * 2.0f;
    spr.layer = 11U;
    spr.tint = tint;
    (void)world.Emplace<en::Sprite2D>(id, spr);

    en::SpriteAnimator2D anim{};
    anim.atlas = atlas;
    anim.firstFrame = 0U;
    anim.frameCount = 4U;
    anim.ticksPerFrame = 8U;
    (void)world.Emplace<en::SpriteAnimator2D>(id, anim);

    en::CharacterMotor2D motor{};
    motor.bounds = en::CompactAabb2D::FromMeters(0.0f, 0.0f, halfExtents.x, halfExtents.y);
    motor.solidMask = 0x0001U;
    (void)world.Emplace<en::CharacterMotor2D>(id, motor);

    // pushboxGroup = 0U so enemies overlap naturally like Hollow Knight instead of queuing in a rigid line.
    en::MotionStack2D motion{};
    motion.pushboxGroup = 0U;
    (void)world.Emplace<en::MotionStack2D>(id, motion);

    en::StateMachine2D fsm{};
    fsm.rigBlob = rigBlob;
    (void)world.Emplace<en::StateMachine2D>(id, fsm);

    en::HitboxRig2D rig{};
    rig.hurtbox = en::CompactAabb2D::FromMeters(0.0f, 0.0f, halfExtents.x, halfExtents.y);
    rig.hitbox = en::CompactAabb2D::FromMeters(0.9f, 0.0f, 0.7f, 0.5f);
    rig.hitLayerMask = 0x01U;
    rig.SetHurtLayer(1U);
    (void)world.Emplace<en::HitboxRig2D>(id, rig);

    en::DamageReceiver2D recv{};
    recv.hp = hp;
    recv.maxHp = hp;
    recv.poise = 100U;
    recv.maxPoise = 100U;
    recv.autoIframeTicks = 0U;
    recv.SetTeamId(2U);
    (void)world.Emplace<en::DamageReceiver2D>(id, recv);

    ResonanceMarkComp mark{};
    (void)world.Emplace<ResonanceMarkComp>(id, mark);

    BreakAndCcComp brk{};
    brk.category = category;
    brk.flags = ruleFlags;
    (void)world.Emplace<BreakAndCcComp>(id, brk);

    const EnemyAttackSpec& atkSpec = GetEnemyAttackSpec(initialAttack);
    EnemyAiComp ai{};
    ai.patrolOriginX = pos.x;
    ai.attackRange = attackRange;
    ai.cooldownTicks = 48U;
    ai.archetypeId = static_cast<u8>(archetype);
    ai.shardReward = shardReward;
    ai.roomId = roomId;
    ai.spawnSlotBit = spawnSlotBit;
    ai.activeAttackId = initialAttack;
    ai.activeTelegraph = atkSpec.telegraph;
    ai.counterEcho = atkSpec.counterEcho;
    (void)world.Emplace<EnemyAiComp>(id, ai);

    WeaponAttackComp atk{};
    atk.baseHpDamage = atkSpec.hitDamage;
    atk.baseBreakDamage = atkSpec.breakDamage;
    atk.telegraph = atkSpec.telegraph;
    atk.counterEchoId = atkSpec.counterEcho;
    (void)world.Emplace<WeaponAttackComp>(id, atk);

    en::Tag2D tag{};
    tag.name = en::StringId::Hash(category == TargetCategory::Boss ? "AshSumpWardenBoss" : "Enemy");
    tag.categoryMask = en::kRoomTransientCategoryBit;
    (void)world.Emplace<en::Tag2D>(id, tag);

    return id;
}

[[nodiscard]] EnemyAttackId SelectNextEnemyAttack(EnemyAiComp& ai, TargetCategory category) noexcept {
    const auto archetype = static_cast<EnemyArchetype>(ai.archetypeId);
    if (category == TargetCategory::Boss || archetype == EnemyArchetype::AshSumpWardenBoss) {
        if (ai.bossPhase >= 2U) {
            constexpr std::array<EnemyAttackId, 4> kPhase2Cycle{{
                EnemyAttackId::WardenResonantCleave,
                EnemyAttackId::WardenRefractedSweep,
                EnemyAttackId::WardenChoirBolts,
                EnemyAttackId::WardenCrimsonCollapse,
            }};
            return kPhase2Cycle[ai.patternIndex % kPhase2Cycle.size()];
        }
        constexpr std::array<EnemyAttackId, 3> kPhase1Cycle{{
            EnemyAttackId::WardenResonantCleave,
            EnemyAttackId::WardenChoirBolts,
            EnemyAttackId::WardenCrimsonCollapse,
        }};
        return kPhase1Cycle[ai.patternIndex % kPhase1Cycle.size()];
    }

    switch (archetype) {
        case EnemyArchetype::DissonantDrifter:
            return EnemyAttackId::DrifterCloseSwipe;
        case EnemyArchetype::ChoirSentry:
            return EnemyAttackId::SentrySingleBolt;
        case EnemyArchetype::ResonanceBrute:
        case EnemyArchetype::TuningHuskElite:
            return ((ai.patternIndex & 1U) == 0U)
                       ? EnemyAttackId::BruteShoulderBreak
                       : EnemyAttackId::BruteHeavySlam;
        case EnemyArchetype::AshSumpWardenBoss:
            return EnemyAttackId::WardenResonantCleave;
    }
    return EnemyAttackId::DrifterCloseSwipe;
}

void ClearEncounterProjectiles(en::World& world) noexcept {
    for (auto [projId, proj] : world.View<ProjectileComp>()) {
        proj.active = 0U;
        (void)world.DeferDestroy(projId);
    }
}

} // namespace

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnDissonantDrifter(
    en::World& world,
    const GameAssetHandles& assets,
    Vec2 pos,
    u8 roomId,
    u8 spawnSlotBit) noexcept {
    return SpawnEnemyCommon(
        world,
        assets.enemyRigBlob,
        assets.characterAtlas,
        pos,
        Vec2{0.38f, 0.75f},
        Color{0.82f, 0.32f, 0.42f, 1.0f},
        kStandardEnemyBaseHp,
        TargetCategory::Standard,
        BreakAndCcComp::kFlagInterruptibleWindup,
        EnemyArchetype::DissonantDrifter,
        EnemyAttackId::DrifterCloseSwipe,
        1.20f,
        20U,
        roomId,
        spawnSlotBit);
}

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnChoirSentry(
    en::World& world,
    const GameAssetHandles& assets,
    Vec2 pos,
    u8 roomId,
    u8 spawnSlotBit) noexcept {
    return SpawnEnemyCommon(
        world,
        assets.enemyRigBlob,
        assets.characterAtlas,
        pos,
        Vec2{0.36f, 0.78f},
        Color{0.38f, 0.78f, 0.95f, 1.0f},
        kStandardEnemyBaseHp,
        TargetCategory::Standard,
        BreakAndCcComp::kFlagInterruptibleWindup,
        EnemyArchetype::ChoirSentry,
        EnemyAttackId::SentrySingleBolt,
        8.50f,
        25U,
        roomId,
        spawnSlotBit);
}

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnResonanceBrute(
    en::World& world,
    const GameAssetHandles& assets,
    Vec2 pos,
    u8 roomId,
    u8 spawnSlotBit) noexcept {
    const u8 flags = BreakAndCcComp::kFlagDisplacementResistant |
                     BreakAndCcComp::kFlagInterruptibleWindup;
    return SpawnEnemyCommon(
        world,
        assets.enemyRigBlob,
        assets.characterAtlas,
        pos,
        Vec2{0.62f, 1.05f},
        Color{0.88f, 0.44f, 0.22f, 1.0f},
        kHeavyEliteEnemyBaseHp,
        TargetCategory::HeavyElite,
        flags,
        EnemyArchetype::ResonanceBrute,
        EnemyAttackId::BruteShoulderBreak,
        2.00f,
        60U,
        roomId,
        spawnSlotBit);
}

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnTuningHuskElite(
    en::World& world,
    const GameAssetHandles& assets,
    Vec2 pos,
    u8 roomId,
    u8 spawnSlotBit) noexcept {
    const u8 flags = BreakAndCcComp::kFlagDisplacementResistant |
                     BreakAndCcComp::kFlagBreakableGuard |
                     BreakAndCcComp::kFlagGuardActive |
                     BreakAndCcComp::kFlagInterruptibleWindup;
    return SpawnEnemyCommon(
        world,
        assets.enemyRigBlob,
        assets.characterAtlas,
        pos,
        Vec2{0.58f, 0.98f},
        Color{0.90f, 0.72f, 0.25f, 1.0f},
        kHeavyEliteEnemyBaseHp,
        TargetCategory::HeavyElite,
        flags,
        EnemyArchetype::TuningHuskElite,
        EnemyAttackId::BruteShoulderBreak,
        2.00f,
        75U,
        roomId,
        spawnSlotBit);
}

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnAshSumpWardenBoss(
    en::World& world,
    const GameAssetHandles& assets,
    Vec2 pos,
    u8 roomId) noexcept {
    const u8 flags = BreakAndCcComp::kFlagDisplacementResistant |
                     BreakAndCcComp::kFlagAnchored |
                     BreakAndCcComp::kFlagInterruptibleWindup;
    return SpawnEnemyCommon(
        world,
        assets.bossRigBlob,
        assets.characterAtlas,
        pos,
        Vec2{0.85f, 1.35f},
        Color{0.96f, 0.85f, 0.38f, 1.0f},
        kBossPhaseBaseHp,
        TargetCategory::Boss,
        flags,
        EnemyArchetype::AshSumpWardenBoss,
        EnemyAttackId::WardenResonantCleave,
        2.40f,
        500U,
        roomId,
        0x80U);
}

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnHarmonicProjectile(
    en::World& world,
    EntityId owner,
    Vec2 pos,
    Vec2 velocity,
    i16 damage,
    bool protectedFromClear,
    AttackTelegraphKind telegraph,
    EchoId counterEcho,
    u16 lifetimeTicks,
    f32 heightMeters) noexcept {
    en::EntitySpawnDesc sp{};
    sp.local.x = pos.x;
    sp.local.y = pos.y;
    sp.timeChannel = en::TimeChannel::Projectile;

    auto idRes = world.Spawn(sp);
    if (!idRes.has_value()) return idRes;
    const EntityId id = *idRes;

    en::Velocity2D vel{velocity.x, velocity.y, 0.0f, 0.0f};
    (void)world.Emplace<en::Velocity2D>(id, vel);

    en::Sprite2D spr{};
    spr.width = (heightMeters < 0.40f) ? 0.85f : 0.45f;
    spr.height = heightMeters;
    spr.layer = 14U;
    spr.tint = (telegraph == AttackTelegraphKind::CrimsonOverloadWave)
                   ? Color{1.0f, 0.15f, 0.25f, 1.0f}
                   : Color{1.0f, 0.85f, 0.20f, 1.0f};
    (void)world.Emplace<en::Sprite2D>(id, spr);

    en::Lifetime2D life{};
    life.remainingTicks = lifetimeTicks;
    life.initialTicks = lifetimeTicks;
    (void)world.Emplace<en::Lifetime2D>(id, life);

    ProjectileComp proj{};
    proj.owner = owner;
    proj.damage = damage;
    proj.protectedFromClear = protectedFromClear ? 1U : 0U;
    proj.telegraph = telegraph;
    proj.counterEcho = counterEcho;
    (void)world.Emplace<ProjectileComp>(id, proj);

    en::Tag2D tag{};
    tag.name = en::StringId::Hash("HarmonicProjectile");
    tag.categoryMask = en::kRoomTransientCategoryBit;
    (void)world.Emplace<en::Tag2D>(id, tag);

    return id;
}

void EnemyAiSystem::StepEnemiesAndProjectiles(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline,
    MetroidvaniaSystem* optionalMetroidvania) noexcept {
    const bool playerValid = world.IsAlive(playerId);
    Vec2 playerPos{0.0f, 0.0f};
    bool playerDashing = false;
    bool playerParrying = false;
    if (playerValid) {
        const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
        playerPos = {pxf.x, pxf.y};
        if (const auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
            playerDashing = ps->IsDashing();
            playerParrying = (ps->parryWindowTicks > 0U);
        }
    }

    // Dual-Pool Attack Token Coordinator (Melee Token Pool + Ranged Token Pool):
    // Prevents overlapping unreactable melee attacks while allowing 1 melee + 1 ranged enemy to coordinate.
    u32 activeMeleeTokens = 0U;
    u32 activeRangedTokens = 0U;
    for (auto [id, ai, recv, brk] : world.View<EnemyAiComp, en::DamageReceiver2D, BreakAndCcComp>()) {
        if (!recv.HasFlag(en::ReceiverFlags::Dead) && recv.hp > 0 &&
            brk.category != TargetCategory::Boss && ai.actionTimerTicks > 0U) {
            if (GetEnemyAttackSpec(ai.activeAttackId).IsRanged()) {
                ++activeRangedTokens;
            } else {
                ++activeMeleeTokens;
            }
        }
    }

    for (auto [id, ai, recv, brk, motion, atk] :
         world.View<EnemyAiComp, en::DamageReceiver2D, BreakAndCcComp, en::MotionStack2D, WeaponAttackComp>()) {
        if (ai.phaseTransitionTicks > 0U) {
            --ai.phaseTransitionTicks;
            motion.targetWalkVx = 0.0f;
            ai.actionTimerTicks = 0U;
            continue;
        }

        if (recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) {
            if (brk.category == TargetCategory::Boss && ai.bossPhase < kBossTotalPhases) {
                ++ai.bossPhase;
                ai.phaseTransitionTicks = kBossPhaseTransitionLockTicks;
                ai.actionTimerTicks = 0U;
                ai.cooldownTicks = kWardenPostAttackCooldownTicks;
                recv.hp = kBossPhaseBaseHp;
                recv.maxHp = kBossPhaseBaseHp;
                recv.RemoveFlags(en::ReceiverFlags::Dead);

                brk.breakMeter = 0U;
                brk.breakPhase = BreakPhase::Normal;
                brk.phaseTicks = 0U;
                brk.lockKind = ActionLockKind::None;
                brk.actionLockTicks = 0U;
                if (auto* mark = world.TryGet<ResonanceMarkComp>(id)) {
                    mark->ClearMark();
                }
                ClearEncounterProjectiles(world);

                pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::BossPhase2Reached), true);
                motion.targetWalkVx = 0.0f;
                continue;
            }

            if (ai.shardReward > 0U && playerValid) {
                if (auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
                    ps->carriedTuningShards += ai.shardReward;
                }
                ai.shardReward = 0U;
            }

            if (brk.category == TargetCategory::Boss) {
                ClearEncounterProjectiles(world);
                pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated), true);
            } else if (optionalMetroidvania != nullptr && ai.spawnSlotBit != 0U) {
                optionalMetroidvania->MarkEnemyCleared(ai.roomId, ai.spawnSlotBit);
            }

            if (optionalMetroidvania != nullptr && ai.spawnSlotBit != 0U) {
                (void)world.DeferDestroy(id);
            }
            motion.targetWalkVx = 0.0f;
            continue;
        }

        if (brk.HasRuleFlag(BreakAndCcComp::kFlagBreakableGuard) &&
            brk.guardBrokenTicks == 0U &&
            !brk.IsActionLocked()) {
            brk.flags |= BreakAndCcComp::kFlagGuardActive;
        }

        const f32 speedMult = brk.MoveSpeedMultiplier();
        if (speedMult <= 0.0f) {
            motion.targetWalkVx = 0.0f;
            ai.actionTimerTicks = 0U;
            ai.turnDelayTicks = 0U;
            atk.SetHitboxTriggered(false);
            ai.cooldownTicks = std::max<u16>(ai.cooldownTicks, 54U);
            continue;
        }

        if (ai.actionTimerTicks == 0U && ai.cooldownTicks > 0U) {
            --ai.cooldownTicks;
        }

        const en::WorldTransform2D& exf = world.GetWorldByIndex(id.index);
        const f32 dx = playerPos.x - exf.x;
        const f32 dy = playerPos.y - exf.y;
        const f32 absDx = std::abs(dx);
        const f32 absDy = std::abs(dy);

        const bool isBoss = (brk.category == TargetCategory::Boss);
        const f32 maxAggroDy = isBoss ? kBossVerticalAggroRangeWu : kEnemyVerticalAggroRangeWu;
        const f32 maxAttackDy = isBoss ? kBossVerticalAttackRangeWu : kEnemyVerticalAttackRangeWu;

        const auto* motor = world.TryGet<en::CharacterMotor2D>(id);
        const bool hitWallLeft = (motor != nullptr && en::HasPhysicsFlag(motor->contacts, en::ContactFlags::WallLeft));
        const bool hitWallRight = (motor != nullptr && en::HasPhysicsFlag(motor->contacts, en::ContactFlags::WallRight));

        if (playerValid && ((absDx <= ai.aggroRadius && absDy <= maxAggroDy) || ai.actionTimerTicks > 0U)) {
            const EnemyAttackSpec& curSpec = GetEnemyAttackSpec(ai.activeAttackId);

            // Strict Facing Commitment & Turn-Around Reaction Delay:
            // 1. Once an attack begins (actionTimerTicks > 0), facing is 100% LOCKED until recovery finishes!
            // 2. In neutral (actionTimerTicks == 0), if Kaelen crosses behind the enemy, the enemy must wait
            //    turnDelayTicks (18-22 ticks) before flipping around and being allowed to swing.
            if (ai.actionTimerTicks == 0U) {
                const i8 desiredFacing = (dx < 0.0f) ? -1 : 1;
                if (desiredFacing != motion.FacingSign() && absDx > 0.18f) {
                    if (ai.turnDelayTicks == 0U) {
                        ai.turnDelayTicks = isBoss ? kBossTurnDelayTicks : kEnemyTurnDelayTicks;
                    }
                    if (--ai.turnDelayTicks == 0U) {
                        motion.SetFacingSign(desiredFacing);
                        ai.cooldownTicks = std::max<u16>(ai.cooldownTicks, 12U);
                    } else {
                        motion.targetWalkVx = 0.0f;
                        continue;
                    }
                } else {
                    ai.turnDelayTicks = 0U;
                }
            }

            const f32 facing = static_cast<f32>(motion.FacingSign());
            const f32 forwardDx = dx * facing;

            if (ai.actionTimerTicks == 0U && (absDx > ai.attackRange || absDy > maxAttackDy)) {
                const f32 baseWalk = isBoss ? 3.2f : 2.3f;
                if ((facing < 0.0f && hitWallLeft) || (facing > 0.0f && hitWallRight) || absDx < 0.35f) {
                    motion.targetWalkVx = 0.0f;
                } else {
                    motion.targetWalkVx = facing * baseWalk * speedMult;
                }
            } else {
                motion.targetWalkVx = 0.0f;

                if (ai.actionTimerTicks == 0U && ai.cooldownTicks > 0U) {
                    if (!isBoss && absDx < kEnemyTacticalRetreatDistWu &&
                        ai.cooldownTicks + kEnemyTacticalRetreatTicks >= curSpec.postCooldownTicks) {
                        const bool blockedBehind = (facing > 0.0f && hitWallLeft) || (facing < 0.0f && hitWallRight);
                        if (!blockedBehind) {
                            motion.targetWalkVx = -facing * 1.35f * speedMult;
                        }
                    }
                    continue;
                }

                // Dual-Pool Attack Token Check:
                if (ai.actionTimerTicks == 0U) {
                    const EnemyAttackId candidateId = SelectNextEnemyAttack(ai, brk.category);
                    const EnemyAttackSpec& nextSpec = GetEnemyAttackSpec(candidateId);
                    if (!isBoss) {
                        const bool tokenAvailable = nextSpec.IsRanged()
                                                        ? (activeRangedTokens < kMaxSimultaneousRangedAttackers)
                                                        : (activeMeleeTokens < kMaxSimultaneousCommonAttackers);
                        if (!tokenAvailable) {
                            ai.cooldownTicks = kAttackTokenBusyRetryTicks;
                            continue;
                        }
                        if (nextSpec.IsRanged()) ++activeRangedTokens;
                        else                     ++activeMeleeTokens;
                    }
                    ai.activeAttackId = candidateId;
                    ai.activeTelegraph = nextSpec.telegraph;
                    ai.counterEcho = nextSpec.counterEcho;
                    atk.baseHpDamage = nextSpec.hitDamage;
                    atk.baseBreakDamage = nextSpec.breakDamage;
                    atk.telegraph = nextSpec.telegraph;
                    atk.counterEchoId = nextSpec.counterEcho;
                    atk.SetHitboxTriggered(false);
                    if (nextSpec.interruptible) {
                        brk.flags |= BreakAndCcComp::kFlagInterruptibleWindup;
                    } else {
                        brk.flags = static_cast<u8>(brk.flags & ~BreakAndCcComp::kFlagInterruptibleWindup);
                    }
                }

                ++ai.actionTimerTicks;
                const EnemyAttackSpec& spec = GetEnemyAttackSpec(ai.activeAttackId);
                const f32 invSpeed = 1.0f / std::max(0.25f, speedMult);
                const u16 windupTicks = static_cast<u16>(static_cast<f32>(spec.windupTicks) * invSpeed);
                const u16 activeEndTicks = static_cast<u16>(windupTicks + spec.activeTicks);
                const u16 totalAnimTicks = static_cast<u16>(
                    activeEndTicks + static_cast<u16>(static_cast<f32>(spec.recoveryTicks) * invSpeed));

                const u16 parryLeadStart = (windupTicks > kParryStrikeInterceptLeadTicks)
                                               ? static_cast<u16>(windupTicks - kParryStrikeInterceptLeadTicks)
                                               : 1U;
                const bool isProjectileAttack =
                    (ai.activeAttackId == EnemyAttackId::SentrySingleBolt ||
                     ai.activeAttackId == EnemyAttackId::WardenChoirBolts ||
                     ai.activeAttackId == EnemyAttackId::WardenCrimsonCollapse);

                if (!isProjectileAttack &&
                    !atk.HitboxTriggered() &&
                    spec.IsParryable() &&
                    playerParrying &&
                    ai.actionTimerTicks >= parryLeadStart &&
                    ai.actionTimerTicks <= activeEndTicks &&
                    forwardDx >= -0.25f &&
                    forwardDx <= (spec.reachWidthWu + kParryMeleeReachBonusWu) &&
                    absDy <= (spec.reachHeightWu + 0.65f)) {
                    atk.SetHitboxTriggered(true);
                    brk.flags = static_cast<u8>(brk.flags & ~BreakAndCcComp::kFlagInterruptibleWindup);
                    (void)CombatSystem::ResolveEnemyAttackAgainstPlayer(
                        id,
                        playerId,
                        spec.hitDamage,
                        spec.breakDamage,
                        spec.telegraph,
                        spec.counterEcho,
                        world,
                        pipeline);
                    playerParrying = false;
                    ai.actionTimerTicks = activeEndTicks;
                    motion.targetWalkVx = 0.0f;
                }

                if (ai.activeAttackId == EnemyAttackId::BruteShoulderBreak &&
                    ai.actionTimerTicks >= windupTicks &&
                    ai.actionTimerTicks < activeEndTicks &&
                    !atk.HitboxTriggered()) {
                    motion.targetWalkVx = facing * 10.0f * speedMult;
                }

                if (ai.actionTimerTicks == windupTicks && isProjectileAttack) {
                    brk.flags = static_cast<u8>(brk.flags & ~BreakAndCcComp::kFlagInterruptibleWindup);
                    atk.SetHitboxTriggered(true);

                    if (ai.activeAttackId == EnemyAttackId::SentrySingleBolt) {
                        (void)SpawnHarmonicProjectile(
                            world, id, Vec2{exf.x + facing * 0.55f, exf.y + 0.15f},
                            Vec2{facing * spec.projectileSpeedWu, 0.0f},
                            spec.hitDamage, false, spec.telegraph, spec.counterEcho,
                            spec.projectileLifetimeTicks, 0.36f);
                    } else if (ai.activeAttackId == EnemyAttackId::WardenChoirBolts) {
                        constexpr std::array<f32, 3> kAnglesRad{{-0.261799f, 0.0f, 0.261799f}};
                        for (const f32 ang : kAnglesRad) {
                            const f32 vx = facing * std::cos(ang) * spec.projectileSpeedWu;
                            const f32 vy = std::sin(ang) * spec.projectileSpeedWu;
                            (void)SpawnHarmonicProjectile(
                                world, id, Vec2{exf.x + facing * 0.75f, exf.y + 0.25f},
                                Vec2{vx, vy},
                                spec.hitDamage, false, spec.telegraph, spec.counterEcho,
                                spec.projectileLifetimeTicks, 0.40f);
                        }
                    } else if (ai.activeAttackId == EnemyAttackId::WardenCrimsonCollapse) {
                        (void)SpawnHarmonicProjectile(
                            world, id, Vec2{exf.x + facing * 0.80f, exf.y - 0.82f},
                            Vec2{facing * spec.projectileSpeedWu, 0.0f},
                            spec.hitDamage, true, AttackTelegraphKind::CrimsonOverloadWave,
                            EchoId::None, spec.activeTicks, 0.35f);
                    }
                }

                if (!isProjectileAttack &&
                    !atk.HitboxTriggered() &&
                    ai.actionTimerTicks >= windupTicks &&
                    ai.actionTimerTicks < activeEndTicks) {
                    brk.flags = static_cast<u8>(brk.flags & ~BreakAndCcComp::kFlagInterruptibleWindup);
                    if (!playerDashing) {
                        // Directional Front-Arc Check:
                        // Melee slashes only hit targets in front of the locked facing direction (forwardDx >= -0.15f).
                        // Dashing behind the enemy during windup completely evades the front slash!
                        const bool isRadialSlam = (ai.activeAttackId == EnemyAttackId::BruteHeavySlam);
                        const bool inHorizontalRange = isRadialSlam
                                                           ? (absDx <= spec.reachWidthWu + 0.25f)
                                                           : (forwardDx >= -0.15f && forwardDx <= spec.reachWidthWu + 0.28f);
                        const bool inVerticalRange = isRadialSlam
                                                         ? (absDy <= 0.65f)
                                                         : (absDy <= spec.reachHeightWu);
                        if (inHorizontalRange && inVerticalRange) {
                            atk.SetHitboxTriggered(true);
                            (void)CombatSystem::ResolveEnemyAttackAgainstPlayer(
                                id,
                                playerId,
                                spec.hitDamage,
                                spec.breakDamage,
                                spec.telegraph,
                                spec.counterEcho,
                                world,
                                pipeline);
                        }
                    }
                }

                if (ai.actionTimerTicks >= totalAnimTicks) {
                    ai.actionTimerTicks = 0U;
                    atk.SetHitboxTriggered(false);
                    ai.cooldownTicks = spec.postCooldownTicks;
                    ++ai.patternIndex;
                }
            }
        } else {
            ai.turnDelayTicks = 0U;
            const f32 offsetFromOrigin = exf.x - ai.patrolOriginX;
            if (offsetFromOrigin > ai.patrolRange || hitWallRight) {
                motion.SetFacingSign(-1);
            } else if (offsetFromOrigin < -ai.patrolRange || hitWallLeft) {
                motion.SetFacingSign(1);
            }
            motion.targetWalkVx = static_cast<f32>(motion.FacingSign()) * 1.2f * speedMult;
        }
    }

    if (playerValid && !playerDashing) {
        for (auto [projId, proj] : world.View<ProjectileComp>()) {
            if (proj.active == 0U) continue;
            const en::WorldTransform2D& pxf = world.GetWorldByIndex(projId.index);
            const auto* spr = world.TryGet<en::Sprite2D>(projId);
            const bool isGroundWave = (spr != nullptr && spr->height <= 0.36f);
            const bool canParryProj = (proj.telegraph != AttackTelegraphKind::CrimsonOverloadWave);

            const f32 dx = std::abs(pxf.x - playerPos.x);
            const f32 dy = std::abs(pxf.y - playerPos.y);

            if (playerParrying && canParryProj &&
                (dx * dx + dy * dy <= kParryProjectileCatchRadiusWu * kParryProjectileCatchRadiusWu)) {
                proj.active = 0U;
                (void)CombatSystem::ResolveEnemyAttackAgainstPlayer(
                    proj.owner,
                    playerId,
                    proj.damage,
                    proj.breakDamage,
                    proj.telegraph,
                    proj.counterEcho,
                    world,
                    pipeline);
                playerParrying = false;
                (void)world.DeferDestroy(projId);
                continue;
            }

            const bool hit = isGroundWave
                                 ? (dx <= 0.65f && (playerPos.y - pxf.y) <= 1.00f)
                                 : (dx * dx + dy * dy <= 0.65f * 0.65f);
            if (hit) {
                proj.active = 0U;
                (void)CombatSystem::ResolveEnemyAttackAgainstPlayer(
                    proj.owner,
                    playerId,
                    proj.damage,
                    proj.breakDamage,
                    proj.telegraph,
                    proj.counterEcho,
                    world,
                    pipeline);
                (void)world.DeferDestroy(projId);
            }
        }
    }
}

} // namespace vots
