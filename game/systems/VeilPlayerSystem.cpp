#include "game/systems/VeilPlayerSystem.h"
#include <algorithm>
#include <cmath>

namespace en::veil {

EntityId VeilPlayerSystem::SpawnPlayer(
    World& world, Vec2 spawnPos, AtlasHandle mainAtlas, BlobHandle playerRigBlob) noexcept {
    EntitySpawnDesc sdesc{};
    sdesc.local.x = spawnPos.x;
    sdesc.local.y = spawnPos.y;
    sdesc.timeChannel = TimeChannel::Player;
    auto res = world.Spawn(sdesc);
    if (!res) return kNullEntity;
    const EntityId id = *res;

    Sprite2D sp{};
    sp.tint = Color{1.0f, 1.0f, 1.0f, 1.0f};
    sp.width = 1.1f;
    sp.height = 1.65f;
    sp.layer = 12;
    (void)world.Emplace<Sprite2D>(id, sp);

    SpriteAnimator2D anim{};
    anim.atlas = mainAtlas;
    anim.firstFrame = 0;
    anim.frameCount = 2;
    anim.ticksPerFrame = 8;
    (void)world.Emplace<SpriteAnimator2D>(id, anim);
    (void)world.Emplace<ModifierStack2D>(id, ModifierStack2D{});

    KinematicMotor2D motor{};
    motor.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.38f, 0.78f);
    motor.bodyLayer = 1;
    (void)world.Emplace<KinematicMotor2D>(id, motor);

    MotionStack2D ms{};
    (void)world.Emplace<MotionStack2D>(id, ms);

    StateMachine2D fsm{};
    fsm.rigBlob = playerRigBlob;
    fsm.stateId = static_cast<u8>(PlayerState::Idle);
    (void)world.Emplace<StateMachine2D>(id, fsm);

    HitboxRig2D rig{};
    rig.hurtbox = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.35f, 0.75f);
    rig.hitLayerMask = 2;
    rig.hurtLayerMask = 1;
    (void)world.Emplace<HitboxRig2D>(id, rig);

    DamageReceiver2D dmg{};
    dmg.hp = 120;
    dmg.maxHp = 120;
    dmg.teamId = 1;
    (void)world.Emplace<DamageReceiver2D>(id, dmg);

    SafeGroundBreadcrumb2D bc{spawnPos.x, spawnPos.y, spawnPos.x, spawnPos.y};
    (void)world.Emplace<SafeGroundBreadcrumb2D>(id, bc);

    PlayerControl2D ctrl{};
    (void)world.Emplace<PlayerControl2D>(id, ctrl);

    PointLight2D light{};
    light.color = Color::FromRGBA8(110, 220, 255, 255);
    light.radius = 5.5f;
    light.intensity = 0.85f;
    light.pulseSpeed = 3.0f;
    (void)world.Emplace<PointLight2D>(id, light);

    VerletRope2D scarf{};
    for (Vec2& n : scarf.currPos) n = spawnPos;
    for (Vec2& n : scarf.prevPos) n = spawnPos;
    scarf.segmentLength = 0.22f;
    scarf.nodeCount = 5;
    (void)world.Emplace<VerletRope2D>(id, scarf);
    return id;
}

void VeilPlayerSystem::StepPlayerController(
    World& world, const SimStepView& step, const TickInput& tickInput,
    ActionBuffer& actionBuffer, GameplayPipeline& pipeline, VeilSessionState& session) noexcept {
    if (step.IsFrozen(TimeChannel::Player)) return;

    struct PendingShuriken {
        Vec2 pos{};
        f32 dirX{1.0f};
        EntityId owner{kNullEntity};
        bool valid{false};
    } spawnShuriken{};

    world.Each<PlayerControl2D, KinematicMotor2D, MotionStack2D, StateMachine2D, HitboxRig2D, DamageReceiver2D>(
        [&](EntityId id, PlayerControl2D& pc, KinematicMotor2D& motor,
            MotionStack2D& ms, StateMachine2D& fsm, HitboxRig2D& rig, DamageReceiver2D& dmg) noexcept {
            if (pc.dashCooldownTicks > 0) --pc.dashCooldownTicks;
            if (pc.shurikenCooldownTicks > 0) --pc.shurikenCooldownTicks;

            if (motor.Grounded()) {
                pc.airDashesLeft = 1;
            }

            // Hazard respawn onto SafeGroundBreadcrumb2D.
            if (HasFlag(motor.contacts, ContactFlags::InHazard) && dmg.iframeTicks == 0) {
                dmg.hp = std::max<i16>(1, dmg.hp - 15);
                dmg.iframeTicks = 45;
                if (const SafeGroundBreadcrumb2D* bc = world.TryGet<SafeGroundBreadcrumb2D>(id)) {
                    LocalTransform2D loc = world.GetLocalTransform(id);
                    loc.x = bc->safeX;
                    loc.y = bc->safeY + 0.5f;
                    world.Teleport(id, loc);
                    motor.vx = 0.0f;
                    motor.vy = 0.0f;
                }
            }

            const f32 moveX = tickInput.Axis(VeilAxis::MoveX);
            ms.targetWalkVx = moveX * session.cvarPlayerSpeed;
            if (Sprite2D* sp = world.TryGet<Sprite2D>(id)) {
                sp->SetFlipX(ms.facingSign < 0);
            }

            // Grapple hook activation & pull.
            if (tickInput.Pressed(VeilAction::Grapple)) {
                const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
                f32 bestDist = 999.0f;
                world.Each<GrappleAnchor2D>([&](EntityId aId, const GrappleAnchor2D& anchor) noexcept {
                    if (!anchor.enabled) return;
                    const WorldTransform2D& awt = world.GetWorldByIndex(aId.index);
                    const f32 d = (Vec2{awt.x, awt.y} - Vec2{wt.x, wt.y}).Length();
                    if (d <= anchor.activationRadius && d < bestDist) {
                        bestDist = d;
                        pc.grappleTarget = {awt.x, awt.y};
                        pc.grappleActive = true;
                        fsm.TransitionTo(static_cast<u8>(PlayerState::GrapplePull));
                    }
                });
            }

            if (pc.grappleActive) {
                const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
                const Vec2 toAnchor = pc.grappleTarget - Vec2{wt.x, wt.y};
                if (toAnchor.Length() < 0.75f || fsm.stateTicks > 35) {
                    pc.grappleActive = false;
                    motor.vy = 7.5f;
                    fsm.TransitionTo(static_cast<u8>(PlayerState::Fall));
                } else {
                    const Vec2 dir = toAnchor.Normalized();
                    ms.SetDashOverride(dir.x * 20.0f, dir.y * 20.0f, 2, 20);
                    return;
                }
            }

            // Dash cancel.
            if (actionBuffer.Consume(VeilAction::Dash, 10) &&
                pc.dashCooldownTicks == 0 && (motor.Grounded() || pc.airDashesLeft > 0)) {
                if (!motor.Grounded() && pc.airDashesLeft > 0) --pc.airDashesLeft;
                pc.dashCooldownTicks = 28;
                dmg.iframeTicks = std::max<u16>(dmg.iframeTicks, 14);
                const f32 dashDir = (std::abs(moveX) > 0.1f) ? (moveX > 0.0f ? 1.0f : -1.0f)
                                                             : static_cast<f32>(ms.facingSign);
                ms.SetDashOverride(dashDir * 19.0f, 0.0f, 12, 15);
                rig.EndSwing();
                fsm.TransitionTo(static_cast<u8>(PlayerState::Dash));
                const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
                ParticleBurstDesc burst{};
                burst.origin = {wt.x, wt.y};
                burst.count = 12;
                pipeline.Particles().EmitBurst(burst);
                return;
            }

            // Parry stance.
            if (actionBuffer.Consume(VeilAction::Parry, 8)) {
                rig.EndSwing();
                rig.rigFlags |= (HitboxRigFlags::ParryWindow | HitboxRigFlags::ReflectProjectiles);
                fsm.TransitionTo(static_cast<u8>(PlayerState::ParryStance));
                return;
            }
            if (fsm.stateId == static_cast<u8>(PlayerState::ParryStance) && fsm.stateTicks >= 14) {
                rig.rigFlags &= ~(HitboxRigFlags::ParryWindow | HitboxRigFlags::ReflectProjectiles);
                fsm.TransitionTo(static_cast<u8>(PlayerState::Idle));
            }

            // Jump / Coyote jump / Wall jump.
            if (actionBuffer.Consume(VeilAction::Jump, 12)) {
                if (motor.Grounded() || ms.groundCoyoteTicks > 0) {
                    if (tickInput.Axis(VeilAxis::MoveY) < -0.6f) {
                        motor.oneWayIgnoreTicks = 15;
                    } else {
                        motor.vy = session.cvarJumpSpeed;
                        ms.groundCoyoteTicks = 0;
                        fsm.TransitionTo(static_cast<u8>(PlayerState::Jump));
                    }
                } else if (ms.wallCoyoteTicks > 0) {
                    motor.vy = session.cvarJumpSpeed * 0.92f;
                    motor.vx = static_cast<f32>(-ms.wallSign) * session.cvarPlayerSpeed * 1.15f;
                    ms.wallCoyoteTicks = 0;
                    fsm.TransitionTo(static_cast<u8>(PlayerState::Jump));
                }
            }

            // 3-hit Katana combo chain.
            if (actionBuffer.Consume(VeilAction::Attack, 14)) {
                const u8 cur = fsm.stateId;
                if (cur == static_cast<u8>(PlayerState::Slash1) && fsm.stateTicks >= 5) {
                    fsm.TransitionTo(static_cast<u8>(PlayerState::Slash2));
                } else if (cur == static_cast<u8>(PlayerState::Slash2) && fsm.stateTicks >= 5) {
                    fsm.TransitionTo(static_cast<u8>(PlayerState::Slash3));
                } else if (cur != static_cast<u8>(PlayerState::Slash1) &&
                           cur != static_cast<u8>(PlayerState::Slash2) &&
                           cur != static_cast<u8>(PlayerState::Slash3)) {
                    fsm.TransitionTo(static_cast<u8>(PlayerState::Slash1));
                }
                const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
                pipeline.EmitCue(SimCue{kClipSlashId, wt.x, wt.y, id, 32768, 8, SimCueKind::AudioOneShot, TimeChannel::Player, 0, 0});
            }

            if ((fsm.stateId == static_cast<u8>(PlayerState::Slash1) ||
                 fsm.stateId == static_cast<u8>(PlayerState::Slash2) ||
                 fsm.stateId == static_cast<u8>(PlayerState::Slash3)) &&
                fsm.stateTicks >= 14) {
                rig.EndSwing();
                fsm.TransitionTo(static_cast<u8>(PlayerState::Idle));
            }

            // Shuriken ranged throw.
            if (actionBuffer.Consume(VeilAction::ThrowShuriken, 10) &&
                pc.shurikenCooldownTicks == 0 && pc.shurikenAmmo > 0) {
                --pc.shurikenAmmo;
                pc.shurikenCooldownTicks = 18;
                const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
                const f32 dirX = static_cast<f32>(ms.facingSign >= 0 ? 1 : -1);
                spawnShuriken = PendingShuriken{{wt.x + dirX * 0.65f, wt.y + 0.1f}, dirX, id, true};
            }
        });

    if (spawnShuriken.valid) {
        EntitySpawnDesc sdesc{};
        sdesc.local.x = spawnShuriken.pos.x;
        sdesc.local.y = spawnShuriken.pos.y;
        sdesc.timeChannel = TimeChannel::Projectile;
        if (auto pRes = world.Spawn(sdesc)) {
            const EntityId pId = *pRes;
            Sprite2D sp{};
            sp.tint = Color::FromRGBA8(255, 235, 110, 255);
            sp.width = 0.35f;
            sp.height = 0.18f;
            sp.layer = 14;
            (void)world.Emplace<Sprite2D>(pId, sp);
            (void)world.Emplace<Projectile2D>(
                pId, Projectile2D{spawnShuriken.owner, spawnShuriken.dirX * 22.0f, 0.0f, 1, 14, false, 0});
            (void)world.Emplace<Lifetime2D>(pId, Lifetime2D{90, 90, true, {}});
        }
    }
}

} // namespace en::veil
