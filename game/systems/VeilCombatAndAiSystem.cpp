#include "game/systems/VeilCombatAndAiSystem.h"
#include <algorithm>
#include <cmath>

namespace en::veil {

void VeilAiAndWorldSystem::BuildEnemyBehaviorTree(BehaviorTree2D& outTree) noexcept {
    outTree.Clear();
    const u8 root = outTree.AddNode(BtNodeKind::Selector);

    const u8 meleeSeq = outTree.AddNode(BtNodeKind::Sequence);
    const u8 inAttackRange = outTree.AddNode(BtNodeKind::Condition, [](BtBlackboard& bb) noexcept {
        return (bb.targetPos - bb.selfPos).Length() <= bb.attackRange ? BtStatus::Success : BtStatus::Failure;
    });
    const u8 doAttack = outTree.AddNode(BtNodeKind::Action, [](BtBlackboard& bb) noexcept {
        bb.desiredAction = static_cast<u8>(EnemyState::Attack);
        return BtStatus::Success;
    });
    outTree.AddChild(meleeSeq, inAttackRange);
    outTree.AddChild(meleeSeq, doAttack);

    const u8 chaseSeq = outTree.AddNode(BtNodeKind::Sequence);
    const u8 inAggro = outTree.AddNode(BtNodeKind::Condition, [](BtBlackboard& bb) noexcept {
        return (bb.targetPos - bb.selfPos).Length() <= bb.aggroRange ? BtStatus::Success : BtStatus::Failure;
    });
    const u8 doChase = outTree.AddNode(BtNodeKind::Action, [](BtBlackboard& bb) noexcept {
        bb.desiredAction = static_cast<u8>(EnemyState::Chase);
        return BtStatus::Success;
    });
    outTree.AddChild(chaseSeq, inAggro);
    outTree.AddChild(chaseSeq, doChase);

    const u8 doPatrol = outTree.AddNode(BtNodeKind::Action, [](BtBlackboard& bb) noexcept {
        bb.desiredAction = static_cast<u8>(EnemyState::Patrol);
        return BtStatus::Success;
    });

    outTree.AddChild(root, meleeSeq);
    outTree.AddChild(root, chaseSeq);
    outTree.AddChild(root, doPatrol);
}

EntityId VeilAiAndWorldSystem::SpawnEnemy(
    World& world, Vec2 pos, EnemyKind kind, AtlasHandle mainAtlas, BlobHandle enemyRigBlob) noexcept {
    EntitySpawnDesc sdesc{};
    sdesc.local.x = pos.x;
    sdesc.local.y = pos.y;
    sdesc.timeChannel = TimeChannel::Enemy;
    auto res = world.Spawn(sdesc);
    if (!res) return kNullEntity;
    const EntityId id = *res;

    Sprite2D sp{};
    sp.tint = Color{1.0f, 1.0f, 1.0f, 1.0f};
    sp.width = (kind == EnemyKind::WardenBoss) ? 1.5f : 1.0f;
    sp.height = (kind == EnemyKind::WardenBoss) ? 2.15f : 1.6f;
    sp.layer = 11;
    (void)world.Emplace<Sprite2D>(id, sp);

    SpriteAnimator2D anim{};
    anim.atlas = mainAtlas;
    anim.firstFrame = (kind == EnemyKind::WardenBoss) ? 4 : (kind == EnemyKind::AshArcher ? 3 : 2);
    anim.frameCount = 1;
    (void)world.Emplace<SpriteAnimator2D>(id, anim);

    KinematicMotor2D motor{};
    motor.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, sp.width * 0.45f, sp.height * 0.48f);
    motor.bodyLayer = 2;
    (void)world.Emplace<KinematicMotor2D>(id, motor);
    (void)world.Emplace<MotionStack2D>(id, MotionStack2D{});

    StateMachine2D fsm{};
    fsm.rigBlob = enemyRigBlob;
    fsm.stateId = static_cast<u8>(EnemyState::Patrol);
    (void)world.Emplace<StateMachine2D>(id, fsm);

    HitboxRig2D rig{};
    rig.hurtbox = motor.bounds;
    rig.hitLayerMask = 1;
    rig.hurtLayerMask = 2;
    (void)world.Emplace<HitboxRig2D>(id, rig);

    DamageReceiver2D dmg{};
    dmg.hp = (kind == EnemyKind::WardenBoss) ? 220 : 50;
    dmg.maxHp = dmg.hp;
    dmg.poise = (kind == EnemyKind::WardenBoss) ? 60 : 20;
    dmg.maxPoise = dmg.poise;
    dmg.teamId = 2;
    (void)world.Emplace<DamageReceiver2D>(id, dmg);

    EnemyBrain2D brain{};
    brain.patrolOriginX = pos.x;
    brain.kind = kind;
    brain.attackRange = (kind == EnemyKind::AshArcher) ? 7.5f : 1.65f;
    (void)world.Emplace<EnemyBrain2D>(id, brain);
    return id;
}

void VeilAiAndWorldSystem::StepEnemiesAndProjectiles(
    World& world, const SimStepView& step, GameplayPipeline& pipeline,
    VeilSessionState& session) noexcept {
    Vec2 playerPos{0.0f, 0.0f};
    const bool hasPlayer = world.IsAlive(session.playerEntity);
    if (hasPlayer) {
        const WorldTransform2D& pwt = world.GetWorldByIndex(session.playerEntity.index);
        playerPos = {pwt.x, pwt.y};
    }

    struct SpawnBoltReq {
        Vec2 pos{};
        Vec2 vel{};
        EntityId owner{kNullEntity};
        bool valid{false};
    } boltReq{};

    world.Each<EnemyBrain2D, KinematicMotor2D, MotionStack2D, StateMachine2D, HitboxRig2D, DamageReceiver2D>(
        [&](EntityId id, EnemyBrain2D& brain, const KinematicMotor2D&,
            MotionStack2D& ms, StateMachine2D& fsm, HitboxRig2D& rig, DamageReceiver2D& dmg) noexcept {
            if (step.IsFrozen(TimeChannel::Enemy)) return;
            if (brain.actionCooldownTicks > 0) --brain.actionCooldownTicks;

            if (dmg.stunTicks > 0) {
                ms.targetWalkVx = 0.0f;
                rig.EndSwing();
                return;
            }

            if (brain.kind == EnemyKind::WardenBoss && dmg.hp * 2 <= dmg.maxHp && brain.bossPhase == 1) {
                brain.bossPhase = 2;
                pipeline.EmitCue(SimCue{{}, 0.0f, 0.0f, id, 45000, 18, SimCueKind::CameraTraumaPulse, TimeChannel::Camera, 0, 0});
            }

            const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
            BtBlackboard bb{};
            bb.selfPos = {wt.x, wt.y};
            bb.targetPos = hasPlayer ? playerPos : Vec2{wt.x + 100.0f, wt.y};
            bb.aggroRange = brain.aggroRange;
            bb.attackRange = brain.attackRange;
            (void)session.enemyBt.Evaluate(bb);

            if (bb.desiredAction == static_cast<u8>(EnemyState::Attack) && brain.actionCooldownTicks == 0) {
                ms.targetWalkVx = 0.0f;
                ms.facingSign = (playerPos.x >= wt.x) ? 1 : -1;
                fsm.TransitionTo(static_cast<u8>(EnemyState::Attack));
                brain.actionCooldownTicks = (brain.bossPhase == 2) ? 32 : 50;

                if (brain.kind == EnemyKind::AshArcher ||
                    (brain.kind == EnemyKind::WardenBoss && brain.bossPhase == 2)) {
                    const Vec2 dir = (playerPos - Vec2{wt.x, wt.y}).Normalized();
                    boltReq = SpawnBoltReq{{wt.x + dir.x * 0.7f, wt.y + 0.2f}, dir * 13.5f, id, true};
                }
            } else if (bb.desiredAction == static_cast<u8>(EnemyState::Chase)) {
                std::array<Vec2, 8> waypoints{};
                const u32 wpCount = NavGrid2D::FindPathAStar(
                    pipeline.Spatial(), {wt.x, wt.y}, playerPos, waypoints);
                const Vec2 target = (wpCount > 1) ? waypoints[1] : playerPos;
                const f32 dirX = (target.x >= wt.x) ? 1.0f : -1.0f;
                ms.targetWalkVx = dirX * ((brain.kind == EnemyKind::WardenBoss) ? 5.2f : 4.2f);
            } else {
                if (std::abs(wt.x - brain.patrolOriginX) > brain.patrolRange) {
                    brain.patrolDir = (wt.x > brain.patrolOriginX) ? -1 : 1;
                }
                ms.targetWalkVx = static_cast<f32>(brain.patrolDir) * 2.4f;
            }
        });

    if (boltReq.valid) {
        EntitySpawnDesc sdesc{};
        sdesc.local.x = boltReq.pos.x;
        sdesc.local.y = boltReq.pos.y;
        sdesc.timeChannel = TimeChannel::Projectile;
        if (auto pRes = world.Spawn(sdesc)) {
            const EntityId pId = *pRes;
            Sprite2D sp{};
            sp.tint = Color::FromRGBA8(255, 95, 195, 255);
            sp.width = 0.38f;
            sp.height = 0.22f;
            sp.layer = 14;
            (void)world.Emplace<Sprite2D>(pId, sp);
            (void)world.Emplace<Projectile2D>(
                pId, Projectile2D{boltReq.owner, boltReq.vel.x, boltReq.vel.y, 2, 16, false, 0});
            (void)world.Emplace<Lifetime2D>(pId, Lifetime2D{120, 120, true, {}});
        }
    }

    // Step active projectiles with tilemap raycast + parry reflection + damage.
    const f32 pDt = step.Dt(TimeChannel::Projectile);
    if (!step.IsFrozen(TimeChannel::Projectile) && pDt > 0.0f) {
        world.Each<Projectile2D>([&](EntityId pId, Projectile2D& proj) noexcept {
            LocalTransform2D& loc = world.ModifyLocalTransformByIndex(pId.index);
            const Vec2 cur{loc.x, loc.y};
            const Vec2 stepDelta{proj.vx * pDt, proj.vy * pDt};
            const RayHit2D wallHit = pipeline.Spatial().Raycast(cur, stepDelta, stepDelta.Length(), 1U, pId);
            if (wallHit.hit) {
                (void)world.DeferDestroy(pId);
                return;
            }
            loc.x += stepDelta.x;
            loc.y += stepDelta.y;

            world.Each<HitboxRig2D, DamageReceiver2D>(
                [&](EntityId tId, const HitboxRig2D& tRig, DamageReceiver2D& tDmg) noexcept {
                    if (tDmg.teamId == proj.teamId || tDmg.IsDead()) return;
                    const WorldTransform2D& twt = world.GetWorldByIndex(tId.index);
                    if (!tRig.hurtbox.WorldRect({twt.x, twt.y}).Contains({loc.x, loc.y})) return;

                    if (HasFlag(tRig.rigFlags, HitboxRigFlags::ReflectProjectiles) && !proj.reflected) {
                        proj.vx = -proj.vx * 1.35f;
                        proj.vy = -proj.vy * 1.35f;
                        proj.teamId = tDmg.teamId;
                        proj.reflected = true;
                        ++session.totalParries;
                        step.RequestMaskHitstop(kGameplayChannelsMask, 5, 0.8f);
                        pipeline.EmitCue(SimCue{kClipParryId, loc.x, loc.y, tId, 48000, 10, SimCueKind::AudioOneShot, TimeChannel::Player, 0, 0});
                    } else if (tDmg.iframeTicks == 0) {
                        tDmg.hp = static_cast<i16>(tDmg.hp - proj.damage);
                        tDmg.iframeTicks = 18;
                        (void)world.DeferDestroy(pId);
                        if (tDmg.IsDead() && tDmg.teamId == 2) {
                            ++session.totalKills;
                            session.soulCurrency += 15;
                            if (tId == session.bossEntity) session.bossDefeated = true;
                            (void)world.DeferDestroy(tId);
                        }
                    }
                });
        });
    }

    // Activate shrines and breakables near player.
    if (hasPlayer) {
        world.Each<ShrineCheckpoint2D>([&](EntityId sId, ShrineCheckpoint2D& shrine) noexcept {
            const WorldTransform2D& swt = world.GetWorldByIndex(sId.index);
            if (!shrine.activated && (Vec2{swt.x, swt.y} - playerPos).Length() < 1.5f) {
                shrine.activated = true;
                pipeline.SetWorldFlag(shrine.worldFlagId, true);
                if (DamageReceiver2D* pdmg = world.TryGet<DamageReceiver2D>(session.playerEntity)) {
                    pdmg->hp = pdmg->maxHp;
                }
            }
        });
    }
}

void VeilAiAndWorldSystem::ResolveCombatHits(
    World& world, const SimStepView& step, std::span<const ResolvedHitEvent> hits,
    GameplayPipeline& pipeline, VeilSessionState& session) noexcept {
    for (const ResolvedHitEvent& ev : hits) {
        if (!world.IsAlive(ev.defender)) continue;
        DamageReceiver2D* defDmg = world.TryGet<DamageReceiver2D>(ev.defender);
        if (!defDmg) continue;

        if (ev.parried) {
            ++session.totalParries;
            step.RequestMaskHitstop(kGameplayChannelsMask, 6, 0.9f);
            if (DamageReceiver2D* atkDmg = world.TryGet<DamageReceiver2D>(ev.attacker)) {
                atkDmg->stunTicks = 35;
                atkDmg->poise = 0;
            }
            pipeline.EmitCue(SimCue{kClipParryId, ev.contactPoint.x, ev.contactPoint.y, ev.defender, 48000, 10, SimCueKind::AudioOneShot, TimeChannel::Player, 0, 0});
            continue;
        }

        defDmg->hp = static_cast<i16>(defDmg->hp - ev.damage);
        defDmg->iframeTicks = 14;
        defDmg->poise = static_cast<i16>(defDmg->poise - ev.damage);
        if (defDmg->poise <= 0) {
            defDmg->stunTicks = 22;
            defDmg->poise = defDmg->maxPoise;
        }

        if (MotionStack2D* ms = world.TryGet<MotionStack2D>(ev.defender)) {
            ms->AddImpulse(ev.knockbackDir.x * 7.5f, ev.knockbackDir.y * 5.0f);
        }

        step.RequestMaskHitstop(kGameplayChannelsMask, ev.hitstopTicks, 0.65f);
        ParticleBurstDesc burst{};
        burst.origin = ev.contactPoint;
        burst.count = 16;
        pipeline.Particles().EmitBurst(burst);
        pipeline.EmitCue(SimCue{kClipHitId, ev.contactPoint.x, ev.contactPoint.y, ev.defender, 36000, 8, SimCueKind::AudioOneShot, TimeChannel::World, 0, 0});
        pipeline.EmitCue(SimCue{{}, ev.contactPoint.x, ev.contactPoint.y, ev.defender, 24000, 8, SimCueKind::CameraTraumaPulse, TimeChannel::Camera, 0, 0});

        if (defDmg->IsDead() && defDmg->teamId == 2) {
            ++session.totalKills;
            session.soulCurrency += 20;
            if (ev.defender == session.bossEntity) session.bossDefeated = true;
            if ( BreakableProp2D* brk = world.TryGet<BreakableProp2D>(ev.defender)) {
                brk->broken = true;
                pipeline.SetWorldFlag(brk->worldFlagId, true);
            }
            (void)world.DeferDestroy(ev.defender);
        }
    }
}

void VeilAiAndWorldSystem::RenderHudAndEffects(
    RenderContext& ctx, VeilSessionState& session) noexcept {
    // 1. Render Moonlit Night Sky Backdrop, Stars, & Parallax Temple Pillars (screen/world layers 1..4)
    ctx.renderList.PushRect({0.0f, 0.0f, 1280.0f, 720.0f}, Color::FromRGBA8(14, 18, 30, 255), 0, 1, 1000, 15, BlendMode::Opaque);
    ctx.renderList.PushRect({0.0f, 440.0f, 1280.0f, 280.0f}, Color::FromRGBA8(20, 26, 44, 180), 0, 1, 1050, 15);
    // Crescent Moon & radial moonbeam glow
    LightingAndPostFx::EmitPointLightHalo(ctx.renderList, {964.0f, 108.0f}, 96.0f, Color::FromRGBA8(180, 220, 255, 255), 0.85f, 0, 2, 15);
    ctx.renderList.PushRect({940.0f, 84.0f, 48.0f, 48.0f}, Color::FromRGBA8(235, 245, 255, 230), 0, 2, 1200, 15);
    for (u32 st = 0; st < 24U; ++st) {
        const f32 sx = static_cast<f32>((st * 173U + 47U) % 1220U) + 30.0f;
        const f32 sy = static_cast<f32>((st * 97U + 23U) % 260U) + 20.0f;
        ctx.renderList.PushRect({sx, sy, 3.0f, 3.0f}, Color::FromRGBA8(200, 225, 255, 190), 0, 2, 1150, 15);
    }
    for (i32 p = 0; p < 16; ++p) {
        const f32 px = static_cast<f32>(p) * 4.0f + 2.0f;
        ctx.renderList.PushRect({px - 0.35f, 1.0f, 0.7f, 12.0f}, Color::FromRGBA8(24, 32, 50, 220), 0, 3, 2000, 0);
        ctx.renderList.PushRect({px - 0.50f, 8.8f, 1.0f, 0.35f}, Color::FromRGBA8(36, 48, 72, 230), 0, 3, 2050, 0);
    }

    // 2. Render Visible Tilemap (Solid Stone Bricks, One-Way Bridges, Slopes, Spikes, Water)
    const SpatialIndex2D& spatial = ctx.gameplay.Spatial();
    const u16 mapW = spatial.MapWidth();
    const u16 mapH = spatial.MapHeight();
    const f32 ts = spatial.TileSize();
    const SpriteFrame brickFrame = ctx.assetBank.GetAtlasFrame(session.mainAtlas, 7);

    for (u16 ty = 0; ty < mapH; ++ty) {
        for (u16 tx = 0; tx < mapW; ++tx) {
            const TileType t = spatial.GetTile(static_cast<i32>(tx), static_cast<i32>(ty));
            if (t == TileType::Empty) continue;
            const Rect tileRect{
                spatial.Origin().x + static_cast<f32>(tx) * ts,
                spatial.Origin().y + static_cast<f32>(ty) * ts,
                ts, ts};
            if (t == TileType::Solid) {
                QuadCmd q{};
                q.center = tileRect.Center();
                q.size = {ts, ts};
                q.uv = brickFrame.uv;
                q.texture = brickFrame.gpuTexture;
                q.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
                q.layer = 5;
                q.depth = 16000;
                ctx.renderList.PushQuad(q);
            } else if (t == TileType::OneWay) {
                ctx.renderList.PushRect(
                    {tileRect.x, tileRect.y + ts * 0.65f, ts, ts * 0.35f},
                    Color::FromRGBA8(165, 115, 65, 255), 0, 6, 16500, 0);
            } else if (t == TileType::SpikeHazard) {
                ctx.renderList.PushRect(
                    {tileRect.x + 0.1f, tileRect.y, ts - 0.2f, ts * 0.6f},
                    Color::FromRGBA8(240, 55, 75, 255), 0, 7, 17000, 0);
            } else if (t == TileType::WaterVolume) {
                ctx.renderList.PushRect(
                    tileRect, Color::FromRGBA8(45, 165, 235, 140), 0, 8, 17500, 0);
            } else {
                ctx.renderList.PushRect(
                    tileRect, Color::FromRGBA8(95, 115, 145, 255), 0, 5, 16000, 0);
            }
        }
    }

    // 3. Render Shrines, Grapple Anchors, and Moving Carrier Platforms
    const SpriteFrame shrineFrame = ctx.assetBank.GetAtlasFrame(session.mainAtlas, 5);
    ctx.world.Each<ShrineCheckpoint2D>([&](EntityId id, const ShrineCheckpoint2D& sh) noexcept {
        const WorldTransform2D& wt = ctx.world.GetWorldByIndex(id.index);
        QuadCmd q{};
        q.center = {wt.x, wt.y + 0.6f};
        q.size = {1.1f, 1.6f};
        q.uv = shrineFrame.uv;
        q.texture = shrineFrame.gpuTexture;
        q.color = sh.activated ? Color::FromRGBA8(120, 255, 220, 255) : Color{1.0f, 1.0f, 1.0f, 1.0f};
        q.layer = 9;
        ctx.renderList.PushQuad(q);
        LightingAndPostFx::EmitPointLightHalo(
            ctx.renderList, {wt.x, wt.y + 0.8f}, 3.5f,
            Color::FromRGBA8(80, 245, 210, 220), sh.activated ? 1.1f : 0.6f);
    });

    ctx.world.Each<GrappleAnchor2D>([&](EntityId id, const GrappleAnchor2D& ga) noexcept {
        if (!ga.enabled) return;
        const WorldTransform2D& wt = ctx.world.GetWorldByIndex(id.index);
        ctx.renderList.PushRing({wt.x, wt.y}, 0.18f, 0.34f, Color::FromRGBA8(90, 235, 255, 240), 0, 14, 0);
    });

    ctx.world.Each<KinematicCarrier2D>([&](EntityId id, const KinematicCarrier2D& car) noexcept {
        const WorldTransform2D& wt = ctx.world.GetWorldByIndex(id.index);
        const Rect r = car.bounds.WorldRect({wt.x, wt.y});
        ctx.renderList.PushRect(r, Color::FromRGBA8(85, 115, 155, 255), 0, 6, 16200, 0);
        ctx.renderList.PushRect({r.x, r.y + r.h * 0.7f, r.w, r.h * 0.3f}, Color::FromRGBA8(135, 185, 230, 255), 0, 6, 16250, 0);
    });

    // Overhead health bars for enemies and boss
    ctx.world.Each<EnemyBrain2D, DamageReceiver2D>([&](EntityId id, const EnemyBrain2D& br, const DamageReceiver2D& dmg) noexcept {
        if (dmg.IsDead()) return;
        const WorldTransform2D& wt = ctx.world.GetWorldByIndex(id.index);
        const f32 barW = (br.kind == EnemyKind::WardenBoss) ? 1.8f : 1.0f;
        const f32 barY = wt.y + ((br.kind == EnemyKind::WardenBoss) ? 1.35f : 1.05f);
        const f32 ratio = std::clamp(static_cast<f32>(dmg.hp) / static_cast<f32>(std::max<i16>(1, dmg.maxHp)), 0.0f, 1.0f);
        ctx.renderList.PushRect({wt.x - barW * 0.5f, barY, barW, 0.14f}, Color::FromRGBA8(20, 24, 34, 230), 0, 17, 32000, 0);
        const Color hpCol = (br.kind == EnemyKind::WardenBoss)
                                ? Color::FromRGBA8(255, 195, 55, 255)
                                : Color::FromRGBA8(240, 65, 80, 255);
        ctx.renderList.PushRect({wt.x - barW * 0.5f, barY, barW * ratio, 0.14f}, hpCol, 0, 17, 32100, 0);
    });

    // 4. Emit katana slash arc & grapple beam in world space (viewSlot 0).
    if (ctx.world.IsAlive(session.playerEntity)) {
        const WorldTransform2D& pwt = ctx.world.GetWorldByIndex(session.playerEntity.index);
        if (const StateMachine2D* fsm = ctx.world.TryGet<StateMachine2D>(session.playerEntity)) {
            if (fsm->stateId == static_cast<u8>(PlayerState::Slash1) ||
                fsm->stateId == static_cast<u8>(PlayerState::Slash2) ||
                fsm->stateId == static_cast<u8>(PlayerState::Slash3)) {
                ctx.renderList.PushSlashArc(
                    {pwt.x, pwt.y}, 1.35f, -0.8f, 1.8f, 0.14f,
                    Color::FromRGBA8(140, 245, 255, 230), 0, 16, 0);
            }
        }
        if (const PlayerControl2D* pc = ctx.world.TryGet<PlayerControl2D>(session.playerEntity)) {
            if (pc->grappleActive) {
                ctx.renderList.PushBeam(
                    {pwt.x, pwt.y}, pc->grappleTarget, 0.08f,
                    Color::FromRGBA8(255, 255, 255, 255),
                    Color::FromRGBA8(90, 225, 255, 160), 0, 16, 0);
            }
        }
        if (const VerletRope2D* scarf = ctx.world.TryGet<VerletRope2D>(session.playerEntity)) {
            for (u8 i = 0; i + 1U < scarf->nodeCount; ++i) {
                ctx.renderList.PushLine(
                    scarf->currPos[i], scarf->currPos[i + 1], 0.09f,
                    Color::FromRGBA8(220, 50, 80, 240), 0, 11, 32768, 0);
            }
        }
    }

    LightingAndPostFx::EmitPostProcessOverlays(ctx.renderList, session.postFx, 0, 15);

    if (session.showUiPanel) {
        UiBuilder ui(ctx.renderList, ctx.frameInput, 0, 15);
        ui.BeginVerticalPanel({16.0f, 16.0f, 284.0f, 96.0f}, "VEIL OF THE SILENT");
        f32 hpRatio = 1.0f;
        if (ctx.world.IsAlive(session.playerEntity)) {
            if (const DamageReceiver2D* d = ctx.world.TryGet<DamageReceiver2D>(session.playerEntity)) {
                hpRatio = static_cast<f32>(d->hp) / static_cast<f32>(std::max<i16>(1, d->maxHp));
            }
        }
        ui.ProgressBar("VITALITY", hpRatio, Color::FromRGBA8(235, 65, 85, 255), 18.0f);
        ui.ProgressBar("FOCUS", 0.75f, Color::FromRGBA8(80, 210, 255, 255), 14.0f);
        ui.EndPanel();

        // Top-Right Minimap Radar Overlay (viewSlot 15)
        const Rect mmRect{1044.0f, 16.0f, 220.0f, 88.0f};
        ctx.renderList.PushRect(mmRect, Color::FromRGBA8(12, 16, 26, 220), 0, 200, 32768, 15);
        const f32 scaleX = mmRect.w / std::max(1.0f, static_cast<f32>(spatial.MapWidth()));
        const f32 scaleY = mmRect.h / std::max(1.0f, static_cast<f32>(spatial.MapHeight()));
        if (ctx.world.IsAlive(session.playerEntity)) {
            const WorldTransform2D& pwt = ctx.world.GetWorldByIndex(session.playerEntity.index);
            ctx.renderList.PushRect(
                {mmRect.x + pwt.x * scaleX - 3.0f, mmRect.Bottom() - pwt.y * scaleY - 3.0f, 6.0f, 6.0f},
                Color::FromRGBA8(70, 245, 255, 255), 0, 205, 32768, 15);
        }
        ctx.world.Each<EnemyBrain2D>([&](EntityId eid, const EnemyBrain2D& br) noexcept {
            const WorldTransform2D& ewt = ctx.world.GetWorldByIndex(eid.index);
            const Color blipCol = (br.kind == EnemyKind::WardenBoss)
                                      ? Color::FromRGBA8(255, 215, 70, 255)
                                      : Color::FromRGBA8(255, 75, 90, 255);
            ctx.renderList.PushRect(
                {mmRect.x + ewt.x * scaleX - 2.5f, mmRect.Bottom() - ewt.y * scaleY - 2.5f, 5.0f, 5.0f},
                blipCol, 0, 204, 32768, 15);
        });
        ctx.renderList.PushText({mmRect.x + 12.0f, mmRect.y + 6.0f}, "SANCTUARY RADAR", Color::FromRGBA8(190, 215, 245, 255), 1.0f, 0, 206, 15);
    }
}

} // namespace en::veil
