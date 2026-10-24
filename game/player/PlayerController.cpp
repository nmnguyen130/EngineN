#include "game/player/PlayerController.h"
#include "game/combat/CombatSystem.h"

#include <algorithm>
#include <cmath>

namespace vots {

void PlayerController::ConfigureDefaultInputMap(
    en::InputMap& map,
    f32 analogDeadzone) noexcept {
    using en::AxisBinding;
    using en::InputSource;

    const f32 dz = std::clamp(analogDeadzone, 0.05f, 0.85f);

    AxisBinding moveX{};
    moveX.negSource = InputSource::Key(4U);       // Key 'A'
    moveX.posSource = InputSource::Key(7U);       // Key 'D'
    moveX.altNegSource = InputSource::Key(80U);   // Left Arrow
    moveX.altPosSource = InputSource::Key(79U);   // Right Arrow
    moveX.gamepadAxis = 0U;
    moveX.deadzone = dz;
    map.BindAxis(0U, moveX);

    AxisBinding moveY{};
    moveY.negSource = InputSource::Key(22U);      // Key 'S'
    moveY.posSource = InputSource::Key(26U);      // Key 'W'
    moveY.altNegSource = InputSource::Key(81U);   // Down Arrow
    moveY.altPosSource = InputSource::Key(82U);   // Up Arrow
    moveY.gamepadAxis = 1U;
    moveY.deadzone = dz;
    map.BindAxis(1U, moveY);

    // GDD v0.3–v0.5 Conflict-Free Keyboard-First + Gamepad Bindings (SDL_SCANCODE_SPACE = 44U):
    map.Bind(static_cast<u8>(GameAction::Jump),          {InputSource::Key(44U),  InputSource::Key(26U),  InputSource::Key(82U),  InputSource::Pad(0U)});  // Space / W / Up / A(Cross)
    map.Bind(static_cast<u8>(GameAction::AttackLight),   {InputSource::Key(13U),  InputSource::Key(29U),  InputSource::Mouse(1U), InputSource::Pad(2U)});  // J / Z / Mouse1 / X(Square)
    map.Bind(static_cast<u8>(GameAction::AttackHeavy),   {InputSource::Key(14U),  InputSource::Key(27U),  InputSource::Mouse(2U), InputSource::Pad(3U)});  // K / X / Mouse2 / Y(Triangle)
    map.Bind(static_cast<u8>(GameAction::Dash),          {InputSource::Key(225U), InputSource::Key(15U),  InputSource::Pad(1U)});                          // LShift / L / B(Circle)
    map.Bind(static_cast<u8>(GameAction::Parry),         {InputSource::Key(9U),   InputSource::Key(12U),  InputSource::Pad(9U)});                          // F / I / LB(L1)
    map.Bind(static_cast<u8>(GameAction::SelectEcho1),   {InputSource::Key(30U)});                                                                         // Key 1
    map.Bind(static_cast<u8>(GameAction::SelectEcho2),   {InputSource::Key(31U)});                                                                         // Key 2
    map.Bind(static_cast<u8>(GameAction::SelectEcho3),   {InputSource::Key(32U)});                                                                         // Key 3
    map.Bind(static_cast<u8>(GameAction::CycleEchoNext), {InputSource::Key(6U),   InputSource::PadTrigger(true)});                                         // C / RT
    map.Bind(static_cast<u8>(GameAction::CycleEchoPrev), {InputSource::Key(20U),  InputSource::PadTrigger(false)});                                        // Q / LT
    map.Bind(static_cast<u8>(GameAction::UnisonBurst),   {InputSource::Key(21U),  InputSource::Key(24U),  InputSource::Pad(10U)});                         // R / U / RB(R1)
    map.Bind(static_cast<u8>(GameAction::SacredChord),   {InputSource::Key(23U),  InputSource::Key(18U),  InputSource::Pad(12U)});                         // T / O / D-Pad Down
    map.Bind(static_cast<u8>(GameAction::Interact),      {InputSource::Key(8U),   InputSource::Key(40U),  InputSource::Pad(11U)});                         // E / Enter / D-Pad Up
    map.Bind(static_cast<u8>(GameAction::PlaceMapPin),   {InputSource::Key(19U),  InputSource::Pad(13U)});                                                 // P / L3
    map.Bind(static_cast<u8>(GameAction::ToggleMap),     {InputSource::Key(43U),  InputSource::Key(16U),  InputSource::Pad(4U)});                          // Tab / M / View
    map.Bind(static_cast<u8>(GameAction::Pause),         {InputSource::Key(41U),  InputSource::Key(42U),  InputSource::Pad(6U)});                          // Esc / Backspace / Start
}

bool PlayerController::TryRebindActionWithoutConflict(
    en::InputMap& map,
    GameAction action,
    u8 bindingSlot,
    en::InputSource newSource) noexcept {
    if (!newSource.IsValid() || bindingSlot >= en::kMaxBindingsPerSlot) return false;
    const u8 targetIdx = static_cast<u8>(action);
    const u16 otherActionsMask = static_cast<u16>(~(1U << targetIdx));
    if (map.FindConflictingActions(newSource, otherActionsMask) != 0U) {
        return false;
    }
    const auto cur = map.ActionBindings(targetIdx);
    std::array<en::InputSource, en::kMaxBindingsPerSlot> updated{cur[0], cur[1], cur[2], cur[3]};
    updated[bindingSlot] = newSource;
    map.Bind(targetIdx, {updated[0], updated[1], updated[2], updated[3]});
    return true;
}

void PlayerController::SetDoubleJumpEnabled(
    EntityId playerId,
    bool enabled,
    en::World& world) noexcept {
    if (!world.IsAlive(playerId)) return;
    if (auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
        ps->doubleJumpEnabled = enabled ? 1U : 0U;
        if (!enabled) {
            ps->airJumpsRemaining = 0U;
        } else if (const auto* motor = world.TryGet<en::CharacterMotor2D>(playerId)) {
            if (motor->IsGrounded()) {
                ps->airJumpsRemaining = 1U;
            }
        }
    }
}

void PlayerController::SetMaxDashCharges(
    EntityId playerId,
    u8 maxCharges,
    en::World& world) noexcept {
    if (!world.IsAlive(playerId)) return;
    if (auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
        ps->SetMaxDashCharges(maxCharges);
        ps->airDashCharges = ps->MaxDashCharges();
    }
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
    spr.width = kPlayerSpriteSizeWu;
    spr.height = kPlayerSpriteSizeWu;
    spr.layer = 12U;
    spr.tint = Color{1.0f, 1.0f, 1.0f, 1.0f};
    (void)world.Emplace<en::Sprite2D>(id, spr);

    en::SpriteAnimator2D anim{};
    anim.atlas = assets.characterAtlas;
    anim.firstFrame = 0U;
    anim.frameCount = 6U;
    anim.ticksPerFrame = 8U;
    (void)world.Emplace<en::SpriteAnimator2D>(id, anim);

    en::CharacterMotor2D motor{};
    motor.bounds = en::CompactAabb2D::FromMeters(
        0.0f, 0.0f, kPlayerColliderWidthWu * 0.5f, kPlayerColliderHeightWu * 0.5f);
    motor.solidMask = 0x0001U;
    (void)world.Emplace<en::CharacterMotor2D>(id, motor);

    en::MotionStack2D motion{};
    motion.groundCoyoteTicks = kPlayerCoyoteTicks;
    motion.pushboxGroup = 0U;
    (void)world.Emplace<en::MotionStack2D>(id, motion);

    en::StateMachine2D fsm{};
    fsm.rigBlob = assets.playerRigBlob;
    fsm.stateId = static_cast<u8>(ActorFsmState::Idle);
    (void)world.Emplace<en::StateMachine2D>(id, fsm);

    en::HitboxRig2D rig{};
    rig.hurtbox = en::CompactAabb2D::FromMeters(
        0.0f, 0.0f, kPlayerColliderWidthWu * 0.5f, kPlayerColliderHeightWu * 0.5f);
    rig.hitbox = en::CompactAabb2D::FromMeters(0.85f, 0.0f, 0.75f, 0.50f);
    rig.hitLayerMask = 0x02U;
    rig.SetHurtLayer(0U);
    (void)world.Emplace<en::HitboxRig2D>(id, rig);

    en::DamageReceiver2D recv{};
    recv.hp = kPlayerBaseMaxHp;
    recv.maxHp = kPlayerBaseMaxHp;
    recv.poise = 30U;
    recv.maxPoise = 30U;
    recv.autoIframeTicks = static_cast<u8>(kPlayerHitProtectionTicks);
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
    ps.SetMaxDashCharges(kPlayerDefaultMaxDashCharges);
    ps.airDashCharges = ps.MaxDashCharges();
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
    const auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (ps == nullptr || ps->LanternRelinquished()) return;
    if (recv != nullptr && recv->HasFlag(en::ReceiverFlags::Dead)) return;

    ps->deck.activeSlot = static_cast<u8>(targetSlot % kTriadDeckSize);
    const EchoIdentityDef& def = GetEchoDef(ps->deck.ActiveEcho());

    if (auto* spr = world.TryGet<en::Sprite2D>(playerId)) {
        spr->tint = Color{1.0f, 1.0f, 1.0f, 1.0f};
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
    if (ps == nullptr || atk == nullptr || rig == nullptr || ps->LanternRelinquished()) return;

    const WeaponMoveSpec& spec = GetWeaponMoveSpec(move);
    atk->instanceId = ++ioNextAttackInstanceId;
    atk->elapsedTicks = 0U;
    atk->totalTicks = spec.TotalTicks();
    atk->baseHpDamage = spec.hpDamage;
    atk->baseBreakDamage = spec.breakDamage;
    atk->moveKind = move;
    atk->capturedEchoId = ps->deck.ActiveEcho();
    atk->packedHitAndChain = 0U;
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
    const auto* atk = world.TryGet<WeaponAttackComp>(playerId);
    if (ps == nullptr || recv == nullptr || ps->LanternRelinquished()) return false;
    if (ps->parryWindowTicks > 0U || ps->unisonState != UnisonActionState::None) {
        return false;
    }

    if (atk != nullptr && atk->moveKind != WeaponMoveKind::None) {
        if (!CanCancelAttackWithDashOrParry(*atk)) {
            return false;
        }
        if (auto* mutAtk = world.TryGet<WeaponAttackComp>(playerId)) {
            mutAtk->moveKind = WeaponMoveKind::None;
            mutAtk->elapsedTicks = 0U;
            mutAtk->packedHitAndChain = 0U;
        }
        if (auto* rig = world.TryGet<en::HitboxRig2D>(playerId)) {
            rig->EndSwing();
        }
    }

    if (ps->IsInDashLockout()) {
        ps->dashActiveTicks = 0U;
        ps->dashLockoutTicks = 0U;
        if (auto* motion = world.TryGet<en::MotionStack2D>(playerId)) {
            motion->overrideTicks = 0U;
        }
    }

    ps->parryWindowTicks = kParryWindowTicks;
    ps->capturedParryEcho = ps->deck.ActiveEcho();
    recv->stunTicks = 0U;
    recv->AddFlags(en::ReceiverFlags::Parrying);

    if (auto* fsm = world.TryGet<en::StateMachine2D>(playerId)) {
        fsm->TransitionTo(static_cast<u8>(ActorFsmState::ParryStance));
    }
    return true;
}

bool PlayerController::TryStartDash(
    EntityId playerId,
    en::World& world,
    f32 moveInputX) noexcept {
    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    auto* motion = world.TryGet<en::MotionStack2D>(playerId);
    auto* motor = world.TryGet<en::CharacterMotor2D>(playerId);
    const auto* atk = world.TryGet<WeaponAttackComp>(playerId);
    if (ps == nullptr || motion == nullptr || motor == nullptr) return false;
    if (ps->IsInDashLockout() || ps->unisonState != UnisonActionState::None) return false;

    const bool grounded = motor->IsGrounded();
    if (grounded) {
        if (ps->dashCooldownTicks > 0U) return false;
        if (ps->airDashCharges > 0U) {
            --ps->airDashCharges;
        }
    } else {
        if (ps->airDashCharges == 0U || ps->dashCooldownTicks > 0U) return false;
        --ps->airDashCharges;
    }

    if (atk != nullptr && atk->moveKind != WeaponMoveKind::None) {
        if (!CanCancelAttackWithDashOrParry(*atk)) {
            ++ps->airDashCharges;
            return false;
        }
        if (auto* mutAtk = world.TryGet<WeaponAttackComp>(playerId)) {
            mutAtk->moveKind = WeaponMoveKind::None;
            mutAtk->elapsedTicks = 0U;
            mutAtk->packedHitAndChain = 0U;
        }
        if (auto* rig = world.TryGet<en::HitboxRig2D>(playerId)) {
            rig->EndSwing();
        }
    }

    if (auto* recv = world.TryGet<en::DamageReceiver2D>(playerId)) {
        recv->stunTicks = 0U;
    }

    if (std::abs(moveInputX) >= 0.18f) {
        motion->SetFacingSign(moveInputX < 0.0f ? -1 : 1);
    }
    const f32 dashDir = static_cast<f32>(motion->FacingSign());

    (void)motion->RequestVelocityOverride(dashDir * kPlayerDashSpeedWu, 0.0f, kPlayerDashDurationTicks, 2U);
    ps->dashActiveTicks = kPlayerDashDurationTicks;

    if (!grounded) {
        motor->vy = 0.0f;
        motion->currentWalkVx = dashDir * (kPlayerDashSpeedWu * kPlayerAirDashExitSpeedRatio);
    } else {
        motion->currentWalkVx = dashDir * kPlayerMaxGroundSpeedWu;
    }

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

    const RoomTopologySpec& roomSpec = GetRoomTopology(ps->activeRoomId);

    // Out-of-bounds safety recovery scaled dynamically to each room's variable width & height
    en::LocalTransform2D loc = world.GetLocalTransform(playerId);
    if (loc.y < roomSpec.MinWorldY() - 3.0f ||
        loc.y > roomSpec.MaxWorldY() + 4.0f ||
        loc.x < roomSpec.MinWorldX() - 4.0f ||
        loc.x > roomSpec.MaxWorldX() + 4.0f) {
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

    // Restore Dash and Double Jump charges on WalkableGround when cooldown completes
    if (motor->IsGrounded()) {
        if (ps->dashActiveTicks == 0U && ps->dashCooldownTicks == 0U) {
            ps->airDashCharges = ps->MaxDashCharges();
        }
        ps->airJumpsRemaining = (ps->doubleJumpEnabled != 0U) ? 1U : 0U;
    }
    if (motor->vy < -kPlayerMaxFallSpeedWu) {
        motor->vy = -kPlayerMaxFallSpeedWu;
    }

    // Step 20-tick Dash -> 3-tick Recovery Lockout & Multi-Charge Cooldown
    if (ps->dashActiveTicks > 0U) {
        if (!motor->IsGrounded()) {
            motor->vy = 0.0f;
        }
        if (--ps->dashActiveTicks == 0U) {
            ps->dashLockoutTicks = kPlayerDashRecoveryLockoutTicks;
            ps->dashCooldownTicks = (ps->airDashCharges > 0U)
                                        ? kPlayerChainDashCooldownTicks
                                        : kPlayerGroundDashCooldownTicks;
            if (!motor->IsGrounded()) {
                const f32 exitVx = static_cast<f32>(motion->FacingSign()) *
                                   (kPlayerDashSpeedWu * kPlayerAirDashExitSpeedRatio);
                motion->currentWalkVx = exitVx;
                motor->vx = exitVx;
                motor->vy = 0.0f;
            }
        }
    } else {
        if (ps->dashLockoutTicks > 0U) --ps->dashLockoutTicks;
        if (ps->dashCooldownTicks > 0U) {
            if (--ps->dashCooldownTicks == 0U && motor->IsGrounded()) {
                ps->airDashCharges = ps->MaxDashCharges();
            }
        }
    }

    if (ps->parryWindowTicks > 0U) {
        if (--ps->parryWindowTicks == 0U) {
            recv->RemoveFlags(en::ReceiverFlags::Parrying);
        }
    }
    if (ps->burstVfxTicks > 0U) --ps->burstVfxTicks;

    CombatSystem::StepActiveUnisonOrChord(playerId, world, pipeline);

    if (recv->HasFlag(en::ReceiverFlags::Dead) || recv->hp <= 0) {
        motion->targetWalkVx = 0.0f;
        return;
    }

    // Instant Defensive Escape-Cancel: Parry, Dash, or Jump immediately breaks hitstun
    if (recv->stunTicks > 0U) {
        const bool escapePressed =
            input.Pressed(static_cast<u8>(GameAction::Parry)) ||
            input.Pressed(static_cast<u8>(GameAction::Dash)) ||
            input.Pressed(static_cast<u8>(GameAction::Jump));
        if (escapePressed || recv->stunTicks <= (kPlayerHitstunTicks - kPlayerHardFlinchTicks)) {
            recv->stunTicks = 0U;
        } else {
            motion->targetWalkVx = 0.0f;
            return;
        }
    }

    if (input.Pressed(static_cast<u8>(GameAction::SelectEcho1))) {
        SwitchActiveEchoSlot(playerId, 0U, world);
    } else if (input.Pressed(static_cast<u8>(GameAction::SelectEcho2))) {
        SwitchActiveEchoSlot(playerId, 1U, world);
    } else if (input.Pressed(static_cast<u8>(GameAction::SelectEcho3))) {
        SwitchActiveEchoSlot(playerId, 2U, world);
    } else if (actions.Consume(static_cast<u8>(GameAction::CycleEchoNext), 6U)) {
        SwitchActiveEchoSlot(playerId, static_cast<u8>((ps->deck.activeSlot + 1U) % kTriadDeckSize), world);
    } else if (actions.Consume(static_cast<u8>(GameAction::CycleEchoPrev), 6U)) {
        SwitchActiveEchoSlot(playerId, static_cast<u8>((ps->deck.activeSlot + kTriadDeckSize - 1U) % kTriadDeckSize), world);
    }

    if (atk->moveKind != WeaponMoveKind::None) {
        const WeaponMoveSpec& spec = GetWeaponMoveSpec(atk->moveKind);
        ++atk->elapsedTicks;

        if (input.Pressed(static_cast<u8>(GameAction::AttackLight)) && atk->elapsedTicks >= 4U) {
            const WeaponMoveKind nextStep = WeaponAttackComp::NextChainStepFor(atk->moveKind);
            if (nextStep != WeaponMoveKind::None) {
                atk->SetBufferedChainMove(nextStep);
                (void)actions.Consume(static_cast<u8>(GameAction::AttackLight), kLightChainBufferTicks);
            }
        }

        if (atk->elapsedTicks == spec.startupTicks && !atk->HitboxTriggered()) {
            atk->SetHitboxTriggered(true);
            const en::CompactAabb2D box = en::CompactAabb2D::FromMeters(
                spec.offsetX, spec.offsetY, spec.reachWidthMeters * 0.5f, spec.reachHeightMeters * 0.5f);
            rig->BeginNewSwing(box, spec.hpDamage, spec.breakDamage, 4.0f, 1.5f, 1U, 4U);
        } else if (atk->elapsedTicks == static_cast<u16>(spec.startupTicks + spec.activeTicks)) {
            rig->EndSwing();
        }

        if (atk->elapsedTicks >= atk->totalTicks) {
            rig->EndSwing();
            const WeaponMoveKind finishedMove = atk->moveKind;
            const WeaponMoveKind buffered = atk->BufferedChainMove();
            atk->moveKind = WeaponMoveKind::None;
            atk->elapsedTicks = 0U;
            atk->packedHitAndChain = 0U;

            if (buffered != WeaponMoveKind::None) {
                StartWeaponAttack(playerId, buffered, world, pipeline, ioNextAttackInstanceId);
            } else {
                const WeaponMoveKind graceNext = WeaponAttackComp::NextChainStepFor(finishedMove);
                if (graceNext != WeaponMoveKind::None) {
                    atk->SetBufferedChainMove(graceNext);
                    atk->elapsedTicks = kLightComboGraceTicks;
                }
            }
        }
    } else if (atk->elapsedTicks > 0U) {
        if (--atk->elapsedTicks == 0U) {
            atk->packedHitAndChain = 0U;
        }
    }

    const f32 moveX = input.Axis(0U);
    const f32 moveY = input.Axis(1U);
    const bool grounded = motor->IsGrounded();
    const bool onWallLeft = en::HasPhysicsFlag(motor->contacts, en::ContactFlags::WallLeft);
    const bool onWallRight = en::HasPhysicsFlag(motor->contacts, en::ContactFlags::WallRight);

    const f32 maxSpeed = grounded ? kPlayerMaxGroundSpeedWu : kPlayerMaxAirSpeedWu;
    const bool inHeavyWindup = (atk->moveKind == WeaponMoveKind::Heavy &&
                                atk->elapsedTicks < kHeavyAttackStartupTicks);
    motion->targetWalkVx = inHeavyWindup ? (moveX * 2.0f) : (moveX * maxSpeed);

    // Prevent horizontal wall-sticking when holding Left against WallLeft or Right against WallRight,
    // and enable smooth downward Wall Slide when falling along a wall!
    motion->flags &= ~en::MotionFlags::WallSliding;
    if ((onWallLeft && moveX < -0.05f) || (onWallRight && moveX > 0.05f)) {
        if (!grounded && motor->vy < -kPlayerWallSlideSpeedWu) {
            motor->vy = -kPlayerWallSlideSpeedWu;
            motion->flags |= en::MotionFlags::WallSliding;
        }
    }

    if (grounded && ps->dashActiveTicks == 0U) {
        const bool stoppingOrReversing = (std::abs(moveX) < 0.05f) ||
                                         (moveX * motion->currentWalkVx < 0.0f);
        if (stoppingOrReversing) {
            const f32 extraBrake = (kPlayerGroundBrakingWu - kPlayerGroundAccelWu) * en::kFixedDtSec;
            if (motion->currentWalkVx > 0.0f) {
                motion->currentWalkVx = std::max(0.0f, motion->currentWalkVx - extraBrake);
            } else if (motion->currentWalkVx < 0.0f) {
                motion->currentWalkVx = std::min(0.0f, motion->currentWalkVx + extraBrake);
            }
        }
    }

    if (atk->moveKind == WeaponMoveKind::None && ps->dashActiveTicks == 0U && std::abs(moveX) >= 0.18f) {
        motion->SetFacingSign(moveX < 0.0f ? -1 : 1);
    }
    if (auto* spr = world.TryGet<en::Sprite2D>(playerId)) {
        spr->SetFlipX(motion->FacingSign() < 0);
        const u16 iframes = recv->iframeTicks;
        const bool hurtFlash = (iframes > (kPlayerHitProtectionTicks - kPlayerHurtFlashDurationTicks));
        const bool blinkGhost = (!hurtFlash && iframes > 0U && ((iframes >> 2U) & 1U) != 0U);
        spr->tint = hurtFlash
                        ? Color{1.0f, 0.35f, 0.42f, 1.0f}
                        : Color{1.0f, 1.0f, 1.0f, blinkGhost ? 0.35f : 1.0f};
    }
    if (auto* anim = world.TryGet<en::SpriteAnimator2D>(playerId)) {
        u16 wantFirst = 0U;
        u16 wantCount = 6U;
        u16 wantTicks = 8U;
        bool wantLoop = true;
        if (atk->moveKind != WeaponMoveKind::None) {
            if (atk->moveKind == WeaponMoveKind::Light1 || atk->moveKind == WeaponMoveKind::Air1) {
                wantFirst = 32U; wantCount = 8U; wantTicks = 3U; wantLoop = false;
            } else if (atk->moveKind == WeaponMoveKind::Light2 || atk->moveKind == WeaponMoveKind::Air2) {
                wantFirst = 40U; wantCount = 8U; wantTicks = 3U; wantLoop = false;
            } else {
                wantFirst = 48U; wantCount = 8U; wantTicks = 4U; wantLoop = false;
            }
        } else if (ps->parryWindowTicks > 0U) {
            wantFirst = 6U; wantCount = 2U; wantTicks = 5U; wantLoop = false;
        } else if (ps->IsDashing()) {
            wantFirst = 28U; wantCount = 4U; wantTicks = 5U; wantLoop = false;
        } else if (!grounded) {
            wantFirst = (motor->vy >= 0.0f) ? 24U : 26U;
            wantCount = 2U; wantTicks = 6U; wantLoop = (motor->vy < 0.0f);
        } else if (std::abs(motor->vx) > 4.0f) {
            wantFirst = 16U; wantCount = 8U; wantTicks = 4U; wantLoop = true;
        } else if (std::abs(motor->vx) > 0.25f) {
            wantFirst = 8U; wantCount = 8U; wantTicks = 5U; wantLoop = true;
        }
        if (anim->firstFrame != wantFirst) {
            anim->firstFrame = wantFirst;
            anim->frameCount = wantCount;
            anim->ticksPerFrame = wantTicks;
            anim->currentFrame = 0U;
            anim->tickAccum = 0U;
            anim->loop = wantLoop;
            anim->playing = true;
        }
        if (atk->moveKind != WeaponMoveKind::None) {
            const WeaponMoveSpec& spec = GetWeaponMoveSpec(atk->moveKind);
            const u16 su = std::max<u16>(1U, spec.startupTicks);
            const u16 ac = std::max<u16>(1U, spec.activeTicks);
            const u16 rc = std::max<u16>(1U, spec.recoveryTicks);
            const u16 el = atk->elapsedTicks;
            u16 mappedFrame = 0U;
            if (el < su) {
                mappedFrame = static_cast<u16>((el * 2U) / su);
            } else if (el < static_cast<u16>(su + ac)) {
                mappedFrame = static_cast<u16>(2U + ((el - su) * 3U) / ac);
            } else {
                const u16 rem = static_cast<u16>(el - su - ac);
                mappedFrame = static_cast<u16>(std::min<u16>(7U, 5U + (rem * 3U) / rc));
            }
            anim->currentFrame = mappedFrame;
            anim->tickAccum = 0U;
        }
    }

    if (input.Held(static_cast<u8>(GameAction::Jump))) {
        motion->flags |= en::MotionFlags::JumpHeld;
    } else {
        motion->flags &= ~en::MotionFlags::JumpHeld;
    }

    if (ps->unisonState != UnisonActionState::None) {
        return;
    }

    // 1. Parry (F / I / LB)
    if (input.Pressed(static_cast<u8>(GameAction::Parry)) ||
        actions.Consume(static_cast<u8>(GameAction::Parry), 4U)) {
        if (StartParryStance(playerId, world)) return;
    }

    // 2. Dash (LShift / L / B)
    if (input.Pressed(static_cast<u8>(GameAction::Dash)) ||
        actions.Consume(static_cast<u8>(GameAction::Dash), 5U)) {
        if (TryStartDash(playerId, world, moveX)) return;
    }

    // 3. Ground Jump, Wall Jump (when touching a wall), & Toggleable Air Double Jump
    if (atk->moveKind == WeaponMoveKind::None) {
        if (moveY < -0.5f && input.Pressed(static_cast<u8>(GameAction::Jump)) &&
            en::HasPhysicsFlag(motor->contacts, en::ContactFlags::OnOneWay)) {
            motion->oneWayDropTicks = 10U;
            (void)actions.Consume(static_cast<u8>(GameAction::Jump), kPlayerJumpBufferTicks);
            return;
        }
        if (actions.Peek(static_cast<u8>(GameAction::Jump), kPlayerJumpBufferTicks)) {
            if (motion->TryExecuteJump(kPlayerJumpVelocityWu, kPlayerWallJumpHorizontalWu)) {
                motor->vy = kPlayerJumpVelocityWu;
                if (!grounded) {
                    motion->targetWalkVx = motion->currentWalkVx;
                    motor->vx = motion->currentWalkVx;
                    (void)motion->RequestVelocityOverride(motion->currentWalkVx, kPlayerJumpVelocityWu, 8U, 10U);
                }
                (void)actions.Consume(static_cast<u8>(GameAction::Jump), kPlayerJumpBufferTicks);
                return;
            }
            if (input.Pressed(static_cast<u8>(GameAction::Jump)) &&
                !grounded &&
                ps->doubleJumpEnabled != 0U &&
                ps->airJumpsRemaining > 0U) {
                --ps->airJumpsRemaining;
                motion->pendingLaunchVy = kPlayerDoubleJumpVelocityWu;
                motion->flags |= en::MotionFlags::JumpCutApplied;
                motor->vy = kPlayerDoubleJumpVelocityWu;
                (void)actions.Consume(static_cast<u8>(GameAction::Jump), kPlayerJumpBufferTicks);
                return;
            }
        }
    }

    if (ps->IsInDashLockout()) {
        return;
    }

    // 4. Sacred Chord (T / O) & Standard Unison (R / U)
    if (input.Pressed(static_cast<u8>(GameAction::SacredChord))) {
        (void)actions.Consume(static_cast<u8>(GameAction::SacredChord), 1U);
        if (CombatSystem::BeginUnisonOrSacredChord(playerId, UnisonActionState::SacredChord, world)) {
            ps->burstVfxTicks = 28U;
            return;
        }
    }
    if (input.Pressed(static_cast<u8>(GameAction::UnisonBurst))) {
        (void)actions.Consume(static_cast<u8>(GameAction::UnisonBurst), 1U);
        if (CombatSystem::BeginUnisonOrSacredChord(playerId, UnisonActionState::StandardUnison, world)) {
            ps->burstVfxTicks = 20U;
            return;
        }
    }

    // 5. Heavy Attack (K / X) & 3-Hit Light Combo (J / Z)
    if (input.Pressed(static_cast<u8>(GameAction::AttackHeavy)) ||
        actions.Consume(static_cast<u8>(GameAction::AttackHeavy), 4U)) {
        if (atk->moveKind == WeaponMoveKind::None) {
            StartWeaponAttack(playerId, WeaponMoveKind::Heavy, world, pipeline, ioNextAttackInstanceId);
            return;
        }
    }
    if (input.Pressed(static_cast<u8>(GameAction::AttackLight)) ||
        (atk->moveKind == WeaponMoveKind::None && actions.Consume(static_cast<u8>(GameAction::AttackLight), kLightChainBufferTicks))) {
        if (atk->moveKind == WeaponMoveKind::None) {
            WeaponMoveKind nextMove = grounded ? WeaponMoveKind::Light1 : WeaponMoveKind::Air1;
            if (atk->elapsedTicks > 0U && atk->BufferedChainMove() != WeaponMoveKind::None) {
                const WeaponMoveKind cand = atk->BufferedChainMove();
                const bool candIsAir = (cand == WeaponMoveKind::Air1 || cand == WeaponMoveKind::Air2);
                if (candIsAir == !grounded) {
                    nextMove = cand;
                }
            }
            StartWeaponAttack(playerId, nextMove, world, pipeline, ioNextAttackInstanceId);
            return;
        }
    }
}

} // namespace vots
