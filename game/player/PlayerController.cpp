#include "game/player/PlayerController.h"
#include "game/combat/CombatSystem.h"

#include <algorithm>
#include <cmath>

namespace vots {

void PlayerController::ConfigureDefaultInputMap(en::InputMap& map) noexcept {
    using en::AxisBinding;
    using en::InputSource;
    using en::SocdPolicy;

    AxisBinding moveX{};
    moveX.negSource = InputSource::Key(4U);     // A
    moveX.posSource = InputSource::Key(7U);     // D
    moveX.altNegSource = InputSource::Key(80U); // Left
    moveX.altPosSource = InputSource::Key(79U); // Right
    moveX.gamepadAxis = 0U;
    moveX.socd = SocdPolicy::LastInputWins;
    map.BindAxis(0U, moveX);

    AxisBinding moveY{};
    moveY.negSource = InputSource::Key(22U);    // S
    moveY.posSource = InputSource::Key(26U);    // W
    moveY.altNegSource = InputSource::Key(81U); // Down
    moveY.altPosSource = InputSource::Key(82U); // Up
    moveY.gamepadAxis = 1U;
    moveX.socd = SocdPolicy::Neutral;
    map.BindAxis(1U, moveY);

    map.Bind(static_cast<u8>(GameAction::Jump),          {InputSource::Key(44U),  InputSource::Pad(0U)});
    map.Bind(static_cast<u8>(GameAction::AttackLight),   {InputSource::Key(13U),  InputSource::Mouse(1U), InputSource::Pad(2U)});
    map.Bind(static_cast<u8>(GameAction::AttackHeavy),   {InputSource::Key(14U),  InputSource::Mouse(3U), InputSource::Pad(3U)});
    map.Bind(static_cast<u8>(GameAction::Dash),          {InputSource::Key(225U), InputSource::Key(15U),  InputSource::Pad(5U)});
    map.Bind(static_cast<u8>(GameAction::Parry),         {InputSource::Key(12U),  InputSource::Key(9U),   InputSource::Pad(4U)});
    map.Bind(static_cast<u8>(GameAction::CycleEchoNext), {InputSource::Key(8U),   InputSource::PadTrigger(true)});
    map.Bind(static_cast<u8>(GameAction::CycleEchoPrev), {InputSource::Key(20U),  InputSource::PadTrigger(false)});
    map.Bind(static_cast<u8>(GameAction::UnisonBurst),   {InputSource::Key(24U),  InputSource::Pad(6U)});
    map.Bind(static_cast<u8>(GameAction::SacredChord),   {InputSource::Key(18U),  InputSource::Pad(7U)});
    map.Bind(static_cast<u8>(GameAction::Interact),      {InputSource::Key(21U),  InputSource::Pad(11U)});
    map.Bind(static_cast<u8>(GameAction::PlaceMapPin),   {InputSource::Key(19U),  InputSource::Pad(12U)});
    map.Bind(static_cast<u8>(GameAction::ToggleMap),     {InputSource::Key(16U),  InputSource::Key(43U)});
    map.Bind(static_cast<u8>(GameAction::Pause),         {InputSource::Key(41U)});
    map.Bind(static_cast<u8>(GameAction::MenuConfirm),   {InputSource::Key(40U)});
    map.Bind(static_cast<u8>(GameAction::MenuBack),      {InputSource::Key(42U)});
}

en::Result<EntityId, en::WorldErrc> PlayerController::SpawnKaelen(
    en::World& world,
    const GameAssetHandles& assets,
    Vec2 spawnPos,
    u8 roomId) noexcept {
    en::EntitySpawnDesc sp{};
    sp.local.x = spawnPos.x;
    sp.local.y = spawnPos.y;
    sp.timeChannel = en::TimeChannel::Player;

    auto idRes = world.Spawn(sp);
    if (!idRes.has_value()) return idRes;
    const EntityId id = *idRes;

    en::Sprite2D spr{};
    spr.width = 0.85f;
    spr.height = 1.55f;
    spr.layer = 12U;
    spr.tint = GetEchoDef(EchoId::Ignis).primaryColor;
    spr.sprite = en::SpriteHandle{0U, 1U};
    (void)world.Emplace<en::Sprite2D>(id, spr);

    en::SpriteAnimator2D anim{};
    anim.atlas = assets.characterAtlas;
    anim.firstFrame = 0U;
    anim.frameCount = 4U;
    anim.ticksPerFrame = 8U;
    (void)world.Emplace<en::SpriteAnimator2D>(id, anim);

    en::CharacterMotor2D motor{};
    motor.bounds = en::CompactAabb2D::FromMeters(0.0f, 0.0f, 0.36f, 0.76f);
    motor.solidMask = 0x0001U; // Collides only with world geometry (0x0001), passes freely through enemies!
    (void)world.Emplace<en::CharacterMotor2D>(id, motor);

    en::MotionStack2D motion{};
    motion.groundCoyoteTicks = 8U;
    motion.pushboxGroup = 0U; // Zero pushbox group so Kaelen can dash and move freely through enemies
    (void)world.Emplace<en::MotionStack2D>(id, motion);

    en::StateMachine2D fsm{};
    fsm.rigBlob = assets.playerRigBlob;
    fsm.stateId = static_cast<u8>(ActorFsmState::Idle);
    (void)world.Emplace<en::StateMachine2D>(id, fsm);

    en::HitboxRig2D rig{};
    rig.hurtbox = en::CompactAabb2D::FromMeters(0.0f, 0.0f, 0.36f, 0.76f);
    rig.hitbox = en::CompactAabb2D::FromMeters(0.8f, 0.0f, 0.7f, 0.45f);
    rig.hitLayerMask = 0x02U;
    rig.SetHurtLayer(0U);
    (void)world.Emplace<en::HitboxRig2D>(id, rig);

    en::DamageReceiver2D recv{};
    recv.hp = kPlayerBaseMaxHp;
    recv.maxHp = kPlayerBaseMaxHp;
    recv.poise = 30U;
    recv.maxPoise = 30U;
    recv.autoIframeTicks = 24U;
    recv.SetTeamId(1U);
    (void)world.Emplace<en::DamageReceiver2D>(id, recv);

    en::SafeAnchor2D anchor{spawnPos.x, spawnPos.y, spawnPos.x, spawnPos.y};
    (void)world.Emplace<en::SafeAnchor2D>(id, anchor);

    en::PointLight2D lanternLight{};
    lanternLight.color = GetEchoDef(EchoId::Ignis).primaryColor;
    lanternLight.radius = 4.5f;
    lanternLight.intensity = 0.85f;
    lanternLight.pulseSpeed = 2.0f;
    (void)world.Emplace<en::PointLight2D>(id, lanternLight);

    en::Tag2D tag{};
    tag.name = en::StringId::Hash("Kaelen");
    tag.categoryMask = 0x0001U;
    (void)world.Emplace<en::Tag2D>(id, tag);

    PlayerStateComp ps{};
    ps.activeRoomId = roomId;
    ps.lastResonatorRoomId = roomId;
    (void)world.Emplace<PlayerStateComp>(id, ps);

    WeaponAttackComp atk{};
    (void)world.Emplace<WeaponAttackComp>(id, atk);

    return id;
}

void PlayerController::SwitchActiveEchoSlot(
    EntityId playerId,
    u8 targetSlot,
    en::World& world) noexcept {
    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    if (ps == nullptr || ps->lanternRelinquished != 0U) return;
    ps->deck.activeSlot = static_cast<u8>(targetSlot % kTriadDeckSize);
    const EchoIdentityDef& def = GetEchoDef(ps->deck.ActiveEcho());

    if (auto* spr = world.TryGet<en::Sprite2D>(playerId)) {
        spr->tint = def.primaryColor;
    }
    if (auto* light = world.TryGet<en::PointLight2D>(playerId)) {
        light->color = def.primaryColor;
    }
}

void PlayerController::StartWeaponAttack(
    EntityId playerId,
    WeaponMoveKind move,
    en::World& world,
    en::GameplayPipeline& pipeline,
    u32& ioNextAttackInstanceId) noexcept {
    if (move == WeaponMoveKind::None) return;
    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    auto* atk = world.TryGet<WeaponAttackComp>(playerId);
    auto* rig = world.TryGet<en::HitboxRig2D>(playerId);
    if (ps == nullptr || atk == nullptr || rig == nullptr || ps->lanternRelinquished != 0U) return;

    const WeaponMoveSpec& spec = GetWeaponMoveSpec(move);
    atk->instanceId = ++ioNextAttackInstanceId;
    atk->elapsedTicks = 0U;
    atk->totalTicks = static_cast<u16>(spec.startupTicks + spec.activeTicks + spec.recoveryTicks);
    atk->baseHpDamage = spec.hpDamage;
    atk->baseBreakDamage = spec.breakDamage;
    atk->moveKind = move;
    atk->capturedEchoId = ps->deck.ActiveEcho();
    atk->hitboxTriggered = 0U;
    rig->EndSwing();

    if (auto* fsm = world.TryGet<en::StateMachine2D>(playerId)) {
        ActorFsmState st = ActorFsmState::Light1;
        switch (move) {
            case WeaponMoveKind::Light1: st = ActorFsmState::Light1; break;
            case WeaponMoveKind::Light2: st = ActorFsmState::Light2; break;
            case WeaponMoveKind::Light3: st = ActorFsmState::Light3; break;
            case WeaponMoveKind::Air1:   st = ActorFsmState::Air1;   break;
            case WeaponMoveKind::Air2:   st = ActorFsmState::Air2;   break;
            case WeaponMoveKind::Heavy:  st = ActorFsmState::HeavyWindup; break;
            case WeaponMoveKind::None:   break;
        }
        fsm->TransitionTo(static_cast<u8>(st));
    }

    en::SimCue slashCue{};
    slashCue.kind = en::SimCueKind::AudioOneShot;
    slashCue.assetId = en::AssetId::Hash(move == WeaponMoveKind::Heavy ? "sfx_heavy_impact" : "sfx_blade_slash");
    slashCue.emitter = playerId;
    pipeline.EmitCue(slashCue);
}

bool PlayerController::StartParryStance(
    EntityId playerId,
    en::World& world) noexcept {
    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (ps == nullptr || recv == nullptr || ps->parryCooldownTicks > 0U) return false;

    ps->parryWindowTicks = kParryWindowTicks;
    ps->parryCooldownTicks = 24U;
    ps->capturedParryEcho = ps->deck.ActiveEcho();
    recv->AddFlags(en::ReceiverFlags::Parrying);

    if (auto* fsm = world.TryGet<en::StateMachine2D>(playerId)) {
        fsm->TransitionTo(static_cast<u8>(ActorFsmState::ParryStance));
    }
    return true;
}

bool PlayerController::TryStartDash(
    EntityId playerId,
    en::World& world) noexcept {
    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    auto* motion = world.TryGet<en::MotionStack2D>(playerId);
    const auto* motor = world.TryGet<en::CharacterMotor2D>(playerId);
    if (ps == nullptr || motion == nullptr || motor == nullptr || ps->dashCooldownTicks > 0U) {
        return false;
    }

    if (!motor->IsGrounded()) {
        if (ps->airDashCharges == 0U) {
            return false;
        }
        --ps->airDashCharges;
        motion->flags |= en::MotionFlags::AirborneDash;
    }

    const f32 dashDir = static_cast<f32>(motion->FacingSign());
    (void)motion->RequestVelocityOverride(dashDir * 13.0f, 0.0f, 12U, 2U);
    ps->dashCooldownTicks = 22U;

    if (auto* fsm = world.TryGet<en::StateMachine2D>(playerId)) {
        fsm->TransitionTo(static_cast<u8>(ActorFsmState::Dash));
    }
    return true;
}

void PlayerController::StepPlayer(
    EntityId playerId,
    en::World& world,
    const en::PhysicsSystem& physics,
    en::GameplayPipeline& pipeline,
    const en::TickInput& input,
    en::ActionBuffer& actions,
    u32& ioNextAttackInstanceId) noexcept {
    (void)physics;
    if (!world.IsAlive(playerId)) return;

    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    auto* motion = world.TryGet<en::MotionStack2D>(playerId);
    auto* motor = world.TryGet<en::CharacterMotor2D>(playerId);
    auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    auto* atk = world.TryGet<WeaponAttackComp>(playerId);
    auto* rig = world.TryGet<en::HitboxRig2D>(playerId);
    if (!ps || !motion || !motor || !recv || !atk || !rig) return;

    // Out-of-bounds safety recovery via SafeAnchor2D
    en::LocalTransform2D loc = world.GetLocalTransform(playerId);
    if (loc.y < -3.0f || loc.y > 14.0f || loc.x < -15.0f || loc.x > 24.0f) {
        if (const auto* anchor = world.TryGet<en::SafeAnchor2D>(playerId)) {
            loc.x = anchor->safeX;
            loc.y = std::max(1.0f, anchor->safeY);
        } else {
            loc.x = 0.0f;
            loc.y = 1.0f;
        }
        motor->vx = 0.0f;
        motor->vy = 0.0f;
        motion->currentWalkVx = 0.0f;
        motion->impulseVx = 0.0f;
        world.Teleport(playerId, loc);
    }

    if (motor->IsGrounded()) {
        ps->airDashCharges = ps->maxAirDashCharges;
    }

    if (ps->parryWindowTicks > 0U) {
        if (--ps->parryWindowTicks == 0U) {
            recv->RemoveFlags(en::ReceiverFlags::Parrying);
        }
    }
    if (ps->parryCooldownTicks > 0U) --ps->parryCooldownTicks;
    if (ps->dashCooldownTicks > 0U) --ps->dashCooldownTicks;
    if (ps->burstVfxTicks > 0U) --ps->burstVfxTicks;

    if (actions.Consume(static_cast<u8>(GameAction::CycleEchoNext), 6U)) {
        SwitchActiveEchoSlot(playerId, static_cast<u8>((ps->deck.activeSlot + 1U) % kTriadDeckSize), world);
    } else if (actions.Consume(static_cast<u8>(GameAction::CycleEchoPrev), 6U)) {
        SwitchActiveEchoSlot(playerId, static_cast<u8>((ps->deck.activeSlot + kTriadDeckSize - 1U) % kTriadDeckSize), world);
    }

    if (atk->moveKind != WeaponMoveKind::None) {
        const WeaponMoveSpec& spec = GetWeaponMoveSpec(atk->moveKind);
        ++atk->elapsedTicks;

        if (atk->elapsedTicks == spec.startupTicks && atk->hitboxTriggered == 0U) {
            atk->hitboxTriggered = 1U;
            const en::CompactAabb2D box = en::CompactAabb2D::FromMeters(
                spec.offsetX, spec.offsetY, spec.reachWidthMeters * 0.5f, spec.reachHeightMeters * 0.5f);
            rig->BeginNewSwing(box, spec.hpDamage, spec.breakDamage, 4.0f, 1.5f, 1U, 4U);
        } else if (atk->elapsedTicks == static_cast<u16>(spec.startupTicks + spec.activeTicks)) {
            rig->EndSwing();
        }

        if (atk->elapsedTicks >= atk->totalTicks) {
            rig->EndSwing();
            atk->moveKind = WeaponMoveKind::None;
            atk->elapsedTicks = 0U;
        }
    }

    const f32 moveX = input.Axis(0U);
    const f32 moveY = input.Axis(1U);
    const bool inHeavyWindup = (atk->moveKind == WeaponMoveKind::Heavy &&
                                atk->elapsedTicks < kHeavyAttackStartupTicks);
    motion->targetWalkVx = inHeavyWindup ? (moveX * 2.0f) : (moveX * 6.8f);
    if (atk->moveKind == WeaponMoveKind::None && std::abs(moveX) > 0.1f) {
        motion->SetFacingSign(moveX < 0.0f ? -1 : 1);
        if (auto* spr = world.TryGet<en::Sprite2D>(playerId)) {
            spr->SetFlipX(moveX < 0.0f);
        }
    }

    if (input.Held(static_cast<u8>(GameAction::Jump))) {
        motion->flags |= en::MotionFlags::JumpHeld;
    } else {
        motion->flags &= ~en::MotionFlags::JumpHeld;
    }

    if (moveY < -0.5f && input.Pressed(static_cast<u8>(GameAction::Jump)) &&
        en::HasPhysicsFlag(motor->contacts, en::ContactFlags::OnOneWay)) {
        motion->oneWayDropTicks = 10U;
        (void)actions.Consume(static_cast<u8>(GameAction::Jump), 8U);
    } else if (actions.Peek(static_cast<u8>(GameAction::Jump), 8U)) {
        if (motion->TryExecuteJump(11.2f, 7.5f)) {
            (void)actions.Consume(static_cast<u8>(GameAction::Jump), 8U);
        }
    }

    if (actions.Consume(static_cast<u8>(GameAction::Parry), 6U)) {
        (void)StartParryStance(playerId, world);
    } else if (actions.Consume(static_cast<u8>(GameAction::Dash), 6U)) {
        (void)TryStartDash(playerId, world);
    } else if (actions.Consume(static_cast<u8>(GameAction::SacredChord), 6U)) {
        if (CombatSystem::TryActivateSacredChord(playerId, world, pipeline)) {
            ps->burstVfxTicks = 28U;
        }
    } else if (actions.Consume(static_cast<u8>(GameAction::UnisonBurst), 6U)) {
        if (CombatSystem::TryActivateUnisonBurst(playerId, world, pipeline)) {
            ps->burstVfxTicks = 20U;
        }
    } else if (actions.Consume(static_cast<u8>(GameAction::AttackHeavy), 8U)) {
        if (atk->moveKind == WeaponMoveKind::None) {
            StartWeaponAttack(playerId, WeaponMoveKind::Heavy, world, pipeline, ioNextAttackInstanceId);
        }
    } else if (actions.Consume(static_cast<u8>(GameAction::AttackLight), 8U)) {
        if (motor->IsGrounded()) {
            WeaponMoveKind nextMove = WeaponMoveKind::Light1;
            if (atk->moveKind == WeaponMoveKind::Light1 && atk->elapsedTicks >= 10U) {
                nextMove = WeaponMoveKind::Light2;
            } else if (atk->moveKind == WeaponMoveKind::Light2 && atk->elapsedTicks >= 10U) {
                nextMove = WeaponMoveKind::Light3;
            } else if (atk->moveKind != WeaponMoveKind::None) {
                nextMove = WeaponMoveKind::None;
            }
            if (nextMove != WeaponMoveKind::None) {
                StartWeaponAttack(playerId, nextMove, world, pipeline, ioNextAttackInstanceId);
            }
        } else {
            WeaponMoveKind nextAir = WeaponMoveKind::Air1;
            if (atk->moveKind == WeaponMoveKind::Air1 && atk->elapsedTicks >= 10U) {
                nextAir = WeaponMoveKind::Air2;
            } else if (atk->moveKind != WeaponMoveKind::None) {
                nextAir = WeaponMoveKind::None;
            }
            if (nextAir != WeaponMoveKind::None) {
                StartWeaponAttack(playerId, nextAir, world, pipeline, ioNextAttackInstanceId);
            }
        }
    }
}

} // namespace vots
