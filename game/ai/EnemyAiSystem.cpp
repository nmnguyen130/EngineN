#include "game/ai/EnemyAiSystem.h"
#include "framework/FrameworkTypes.h"
#include "game/combat/CombatSystem.h"

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
    u16 shardReward) noexcept {
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

    en::Collider2D col{};
    col.localBounds = en::CompactAabb2D::FromMeters(0.0f, 0.0f, halfExtents.x, halfExtents.y);
    col.shape = en::ShapeKind2D::CapsuleY;
    col.filter.categoryBits = 0x0004U;
    col.filter.maskBits = 0xFFFFU;
    (void)world.Emplace<en::Collider2D>(id, col);

    en::CharacterMotor2D motor{};
    motor.bounds = en::CompactAabb2D::FromMeters(0.0f, 0.0f, halfExtents.x, halfExtents.y);
    motor.solidMask = 0x0001U;
    (void)world.Emplace<en::CharacterMotor2D>(id, motor);

    en::MotionStack2D motion{};
    motion.pushboxGroup = 1U;
    (void)world.Emplace<en::MotionStack2D>(id, motion);

    en::StateMachine2D fsm{};
    fsm.rigBlob = rigBlob;
    (void)world.Emplace<en::StateMachine2D>(id, fsm);

    en::HitboxRig2D rig{};
    rig.hurtbox = en::CompactAabb2D::FromMeters(0.0f, 0.0f, halfExtents.x, halfExtents.y);
    rig.hitbox = en::CompactAabb2D::FromMeters(0.9f, 0.0f, 0.7f, 0.5f);
    rig.hitLayerMask = 0x01U; // Hits hurtLayer 0 (Kaelen)
    rig.SetHurtLayer(1U);     // Enemy hurtLayer 1
    (void)world.Emplace<en::HitboxRig2D>(id, rig);

    en::DamageReceiver2D recv{};
    recv.hp = hp;
    recv.maxHp = hp;
    recv.poise = 100U;
    recv.maxPoise = 100U;
    recv.autoIframeTicks = 0U; // Enemies do not gain hidden auto-iframes between player combo swings
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
    Vec2 pos) noexcept {
    return SpawnEnemyCommon(
        world,
        assets.enemyRigBlob,
        assets.characterAtlas,
        pos,
        Vec2{0.4f, 0.75f},
        Color{0.75f, 0.35f, 0.45f, 1.0f},
        kStandardEnemyBaseHp,
        TargetCategory::Standard,
        BreakAndCcComp::kFlagInterruptibleWindup,
        0U,
        20U);
}

en::Result<EntityId, en::WorldErrc> EnemyAiSystem::SpawnChoirEnforcer(
    en::World& world,
    const GameAssetHandles& assets,
    Vec2 pos) noexcept {
    const u8 flags = BreakAndCcComp::kFlagDisplacementResistant |
                     BreakAndCcComp::kFlagBreakableGuard |
                     BreakAndCcComp::kFlagGuardActive |
                     BreakAndCcComp::kFlagInterruptibleWindup;
    return SpawnEnemyCommon(
        world,
        assets.enemyRigBlob,
        assets.characterAtlas,
        pos,
        Vec2{0.6f, 1.0f},
        Color{0.85f, 0.70f, 0.25f, 1.0f},
        kHeavyEliteEnemyBaseHp,
        TargetCategory::HeavyElite,
        flags,
        1U,
        65U);
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
        Color{0.95f, 0.85f, 0.40f, 1.0f},
        kBossPhaseBaseHp,
        TargetCategory::Boss,
        flags,
        2U,
        500U);
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
    en::GameplayPipeline& pipeline) noexcept {
    const bool playerValid = world.IsAlive(playerId);
    Vec2 playerPos{0.0f, 0.0f};
    if (playerValid) {
        const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
        playerPos = {pxf.x, pxf.y};
    }

    // 1. Step Enemy AI state machines & Boss phase transitions
    for (auto [id, ai, recv, brk, motion, atk] :
         world.View<EnemyAiComp, en::DamageReceiver2D, BreakAndCcComp, en::MotionStack2D, WeaponAttackComp>()) {
        if (recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) {
            if (ai.shardReward > 0U && playerValid) {
                if (auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
                    ps->carriedTuningShards += ai.shardReward;
                }
                ai.shardReward = 0U;
            }
            if (brk.category == TargetCategory::Boss && ai.bossPhase < 3U) {
                // Advance High Conductor to next phase (1200 HP per phase per GDD v0.2 Section 4.2)
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
            if (brk.category == TargetCategory::Boss) {
                pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated), true);
            }
            motion.targetWalkVx = 0.0f;
            continue;
        }

        // Re-enable BreakableGuard on Choir Enforcer once GuardBroken lockout expires
        if (brk.HasRuleFlag(BreakAndCcComp::kFlagBreakableGuard) &&
            brk.guardBrokenTicks == 0U &&
            !brk.IsActionLocked()) {
            brk.flags |= BreakAndCcComp::kFlagGuardActive;
        }

        const f32 speedMult = brk.MoveSpeedMultiplier();
        if (speedMult <= 0.0f) {
            motion.targetWalkVx = 0.0f;
            continue;
        }

        const en::WorldTransform2D& exf = world.GetWorldByIndex(id.index);
        const f32 dx = playerPos.x - exf.x;
        const f32 absDx = std::abs(dx);

        if (playerValid && absDx <= ai.aggroRadius) {
            const i8 dirSign = (dx < 0.0f) ? -1 : 1;
            motion.SetFacingSign(dirSign);
            if (absDx > ai.attackRange) {
                const f32 baseWalk = (brk.category == TargetCategory::Boss) ? 3.8f : 2.6f;
                motion.targetWalkVx = static_cast<f32>(dirSign) * baseWalk * speedMult;
            } else {
                motion.targetWalkVx = 0.0f;
                if (ai.cooldownTicks > 0U) {
                    --ai.cooldownTicks;
                } else {
                    ++ai.actionTimerTicks;
                    brk.flags |= BreakAndCcComp::kFlagInterruptibleWindup;

                    // Alternate Gold Harmonic Wave (Parryable with CounterEchoId) and Crimson Overload Wave (Unblockable)
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

                    const u16 windupTarget = static_cast<u16>(24.0f / std::max(0.25f, speedMult));
                    if (ai.actionTimerTicks >= windupTarget) {
                        ai.actionTimerTicks = 0U;
                        ai.cooldownTicks = (brk.category == TargetCategory::Boss) ? 45U : 60U;
                        ++ai.patternIndex;
                        if (absDx <= ai.attackRange + 0.4f) {
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
            motion.targetWalkVx = 0.0f;
        }
    }

    // 2. Step Harmonic Projectiles overlap against Kaelen
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
