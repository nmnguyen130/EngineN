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
    u8 archetypeId,
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
    motor.solidMask = 0x0001U; // Collides with static tilemap/walls only, never blocks player movement
    (void)world.Emplace<en::CharacterMotor2D>(id, motor);

    en::MotionStack2D motion{};
    motion.pushboxGroup = 2U;  // Enemies separate from other enemies (group 2), while player is group 0
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

    EnemyAiComp ai{};
    ai.patrolOriginX = pos.x;
    ai.archetypeId = archetypeId;
    ai.shardReward = shardReward;
    ai.roomId = roomId;
    ai.spawnSlotBit = spawnSlotBit;
    (void)world.Emplace<EnemyAiComp>(id, ai);

    WeaponAttackComp atk{};
    atk.baseHpDamage = (category == TargetCategory::Boss) ? 28 : (category == TargetCategory::HeavyElite ? 20 : 12);
    atk.baseBreakDamage = 10U;
    (void)world.Emplace<WeaponAttackComp>(id, atk);

    en::Tag2D tag{};
    tag.name = en::StringId::Hash(category == TargetCategory::Boss ? "HighConductorBoss" : "Enemy");
    tag.categoryMask = en::kRoomTransientCategoryBit;
    (void)world.Emplace<en::Tag2D>(id, tag);

    return id;
}

} // namespace

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnDissonantHusk(
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
        Vec2{0.4f, 0.75f},
        Color{0.82f, 0.32f, 0.42f, 1.0f},
        kStandardEnemyBaseHp,
        TargetCategory::Standard,
        BreakAndCcComp::kFlagInterruptibleWindup,
        0U,
        20U,
        roomId,
        spawnSlotBit);
}

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnChoirEnforcer(
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
        1U,
        65U,
        roomId,
        spawnSlotBit);
}

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnHighConductorBoss(
    en::World& world,
    const GameAssetHandles& assets,
    Vec2 pos) noexcept {
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
        2U,
        500U,
        3U,
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
    EchoId counterEcho) noexcept {
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
    spr.width = 0.45f;
    spr.height = 0.45f;
    spr.layer = 14U;
    spr.tint = (telegraph == AttackTelegraphKind::CrimsonOverloadWave)
                   ? Color{1.0f, 0.15f, 0.25f, 1.0f}
                   : Color{1.0f, 0.85f, 0.20f, 1.0f};
    (void)world.Emplace<en::Sprite2D>(id, spr);

    en::Lifetime2D life{};
    life.remainingTicks = 360U;
    life.initialTicks = 360U;
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
    if (playerValid) {
        const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
        playerPos = {pxf.x, pxf.y};
    }

    for (auto [id, ai, recv, brk, motion, atk] :
         world.View<EnemyAiComp, en::DamageReceiver2D, BreakAndCcComp, en::MotionStack2D, WeaponAttackComp>()) {
        if (recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) {
            if (brk.category == TargetCategory::Boss && ai.bossPhase < 3U) {
                ++ai.bossPhase;
                recv.hp = kBossPhaseBaseHp;
                recv.maxHp = kBossPhaseBaseHp;
                recv.RemoveFlags(en::ReceiverFlags::Dead);
                brk.breakMeter = 0U;
                brk.breakPhase = BreakPhase::Normal;
                pipeline.SetWorldFlag(
                    static_cast<u16>(ai.bossPhase == 2U ? WorldFlagId::BossPhase2Reached : WorldFlagId::BossPhase3Reached),
                    true);
                continue;
            }

            if (ai.shardReward > 0U && playerValid) {
                if (auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
                    ps->carriedTuningShards += ai.shardReward;
                }
                ai.shardReward = 0U;
            }

            if (brk.category == TargetCategory::Boss) {
                pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated), true);
            } else if (optionalMetroidvania != nullptr && ai.spawnSlotBit != 0U) {
                optionalMetroidvania->MarkEnemyCleared(ai.roomId, ai.spawnSlotBit);
            }

            // Despawn dead enemy immediately when killed in live room gameplay
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
            continue;
        }

        const en::WorldTransform2D& exf = world.GetWorldByIndex(id.index);
        const f32 dx = playerPos.x - exf.x;
        const f32 absDx = std::abs(dx);

        if (playerValid && absDx <= ai.aggroRadius) {
            const i8 dirSign = (dx < 0.0f) ? -1 : 1;
            motion.SetFacingSign(dirSign);
            if (absDx > ai.attackRange) {
                const f32 baseWalk = (brk.category == TargetCategory::Boss) ? 3.6f : 2.5f;
                motion.targetWalkVx = static_cast<f32>(dirSign) * baseWalk * speedMult;
                ai.actionTimerTicks = 0U;
            } else {
                motion.targetWalkVx = 0.0f;
                if (ai.cooldownTicks > 0U) {
                    --ai.cooldownTicks;
                } else {
                    ++ai.actionTimerTicks;
                    brk.flags |= BreakAndCcComp::kFlagInterruptibleWindup;

                    if (brk.category == TargetCategory::Boss || brk.category == TargetCategory::HeavyElite) {
                        const bool useCrimson = ((ai.patternIndex & 1U) != 0U);
                        ai.activeTelegraph = useCrimson
                                                 ? AttackTelegraphKind::CrimsonOverloadWave
                                                 : AttackTelegraphKind::GoldHarmonicWave;
                        ai.counterEcho = useCrimson ? EchoId::None : EchoId::Glacies;
                    } else {
                        ai.activeTelegraph = AttackTelegraphKind::GoldHarmonicWave;
                        ai.counterEcho = EchoId::Ignis;
                    }
                    atk.telegraph = ai.activeTelegraph;
                    atk.counterEchoId = ai.counterEcho;

                    const u16 windupTarget = static_cast<u16>(30.0f / std::max(0.25f, speedMult));
                    if (ai.actionTimerTicks >= windupTarget) {
                        ai.actionTimerTicks = 0U;
                        ai.cooldownTicks = (brk.category == TargetCategory::Boss) ? 48U : 65U;
                        ++ai.patternIndex;
                        if (absDx <= ai.attackRange + 0.45f) {
                            (void)CombatSystem::ResolveEnemyAttackAgainstPlayer(
                                id,
                                playerId,
                                atk.baseHpDamage,
                                atk.baseBreakDamage,
                                atk.telegraph,
                                atk.counterEchoId,
                                world,
                                pipeline);
                        }
                    }
                }
            }
        } else {
            // Gentle patrol around origin when player is out of aggro range
            const f32 offsetFromOrigin = exf.x - ai.patrolOriginX;
            if (std::abs(offsetFromOrigin) > ai.patrolRange) {
                motion.SetFacingSign(offsetFromOrigin > 0.0f ? -1 : 1);
            }
            motion.targetWalkVx = static_cast<f32>(motion.FacingSign()) * 1.2f * speedMult;
        }
    }

    if (playerValid) {
        for (auto [projId, proj] : world.View<ProjectileComp>()) {
            if (proj.active == 0U) continue;
            const en::WorldTransform2D& pxf = world.GetWorldByIndex(projId.index);
            const f32 dx = pxf.x - playerPos.x;
            const f32 dy = pxf.y - playerPos.y;
            if (dx * dx + dy * dy <= 0.65f * 0.65f) {
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
