#include "game/GamePolicies.h"

#include <cmath>

namespace game {

namespace {

constexpr i16 MetersToQ8(f32 meters) noexcept {
    return static_cast<i16>(meters * 256.0f);
}

constexpr f32 AbsF32(f32 v) noexcept {
    return v < 0.0f ? -v : v;
}

constexpr f32 ClampF32(f32 v, f32 lo, f32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr bool HasContact(gameplay::ContactFlags flags, gameplay::ContactFlags bit) noexcept {
    return (static_cast<u8>(flags) & static_cast<u8>(bit)) != 0;
}

constexpr gameplay::CancelCategory operator|(gameplay::CancelCategory a, gameplay::CancelCategory b) noexcept {
    return static_cast<gameplay::CancelCategory>(static_cast<u16>(a) | static_cast<u16>(b));
}

constexpr bool CanCancelInto(u16 activeCancelMask, gameplay::CancelCategory cat) noexcept {
    return (activeCancelMask & static_cast<u16>(cat)) != 0 ||
           (activeCancelMask & static_cast<u16>(gameplay::CancelCategory::AnyAction)) != 0;
}

void TransitionPlayerState(
    gameplay::StateMachine2D& fsm,
    world::SpriteAnimator2D* anim,
    gameplay::HitboxRig2D& rig,
    PlayerState nextState,
    u8 clipId,
    u8 ticksPerFrame,
    u8 frameCount,
    bool loop) noexcept
{
    fsm.prevStateId       = fsm.stateId;
    fsm.stateId           = static_cast<u8>(nextState);
    fsm.stateTicks        = 0;
    fsm.animClipId        = clipId;
    fsm.cancelMask        = 0;
    fsm.onHitCancelMask   = 0;
    rig.activeSliceIndex  = 0xFF; // Disable active hitbox until frame directive or active window opens

    if (anim != nullptr) {
        anim->firstFrame    = static_cast<u16>(clipId * 8u);
        anim->frameCount    = frameCount;
        anim->currentFrame  = 0;
        anim->ticksPerFrame = ticksPerFrame;
        anim->subTickAccum  = 0;
        anim->loop          = loop ? 1u : 0u;
        anim->playing       = 1u;
    }
}

void SpawnHitSparkVfx(
    world::World& world,
    f32 x,
    f32 y,
    renderer::Color tint,
    f32 scale = 1.0f,
    u16 durationTicks = 18) noexcept
{
    auto entRes = world.Spawn({
        .local = {.x = x, .y = y, .scaleX = scale, .scaleY = scale, .rotationRad = 0.0f, .depthOffset = 0.1f},
        .flags = world::EntityFlags::Spatial
    });
    if (!entRes) return;
    const world::EntityId id = *entRes;

    (void)world.Emplace<world::Sprite2D>(id, world::Sprite2D{
        .tint   = tint,
        .sprite = assets::SpriteHandle{},
        .baseDepth = 0.8f,
        .layer  = 2
    });
    (void)world.Emplace<TransientVfx2D>(id, TransientVfx2D{
        .velX = 0.0f,
        .velY = 0.6f,
        .remainingTicks = durationTicks,
        .initialTicks   = durationTicks,
        .fadeAlpha      = 1
    });
    (void)world.Emplace<RoomTransientTag>(id, RoomTransientTag{.roomSalt = 1});
}

// ============================================================================
// Authoritative Combat Overlap Callback (Invoked by StateAndCombatSolver Stage 5)
// ============================================================================
void OnResolveCombatHitPolicy(
    void* userContext,
    world::World& world,
    time::SimStepView step,
    const gameplay::ResolvedHitEvent& hit,
    gameplay::GameplayPipeline& pipeline) noexcept
{
    auto* session = static_cast<GameSessionState*>(userContext);
    if (session == nullptr) return;

    auto* attackerFsm = world.TryGet<gameplay::StateMachine2D>(hit.attacker);
    auto* defenderDmg = world.TryGet<gameplay::DamageReceiver2D>(hit.defender);
    auto* defenderMot = world.TryGet<gameplay::KinematicMotor2D>(hit.defender);
    auto* defenderStk = world.TryGet<gameplay::MotionStack2D>(hit.defender);
    if (defenderDmg == nullptr || defenderDmg->iframeTicks > 0 || defenderDmg->hp <= 0) {
        return;
    }

    const auto& atkTransform = world.GetWorldTransform(hit.attacker);
    const auto& defTransform = world.GetWorldTransform(hit.defender);
    const f32 hitDirX = (defTransform.x >= atkTransform.x) ? 1.0f : -1.0f;
    const f32 midX = 0.5f * (atkTransform.x + defTransform.x);
    const f32 midY = 0.5f * (atkTransform.y + defTransform.y);

    // 1. Check Defender Parry Stance (Player parrying an Enemy attack)
    if (auto* defPlayer = world.TryGet<PlayerController2D>(hit.defender)) {
        if (session->tuning.godMode) return;

        if (defPlayer->parryWindowTicks > 0) {
            // Successful Parry! Grant iframes, refill Soul, stun attacker, trigger heavy hitstop
            defenderDmg->iframeTicks = 36;
            defPlayer->soulEnergy = static_cast<u16>(
                (defPlayer->soulEnergy + 35u > 100u) ? 100u : (defPlayer->soulEnergy + 35u));

            if (auto* defFsm = world.TryGet<gameplay::StateMachine2D>(hit.defender);
                defFsm != nullptr) {
                auto* defRig = world.TryGet<gameplay::HitboxRig2D>(hit.defender);
                auto* defAnim = world.TryGet<world::SpriteAnimator2D>(hit.defender);
                if (defRig != nullptr) {
                    TransitionPlayerState(*defFsm, defAnim, *defRig, PlayerState::ParryRiposte, 10, 4, 6, false);
                }
            }

            if (auto* enemyBrain = world.TryGet<EnemyBrain2D>(hit.attacker)) {
                enemyBrain->aiState = EnemyState::Parried;
                enemyBrain->aiTimerTicks = 90; // 750ms vulnerable window
            }
            if (auto* atkStk = world.TryGet<gameplay::MotionStack2D>(hit.attacker)) {
                atkStk->impulseVx = -hitDirX * 8.0f;
                atkStk->impulseVy = 4.0f;
            }

            pipeline.EmitCue(gameplay::SimCue{
                .assetId = Assets::kCueParryClang.value,
                .worldX  = midX,
                .worldY  = midY,
                .param0  = static_cast<u16>(session->tuning.hitstopHeavyTicks),
                .kind    = gameplay::SimCueKind::AudioOneShot
            });
            return;
        }
    }

    // 2. Check Frontal Shield Block (LancerGuard blocking un-parried frontal slashes)
    if (auto* defEnemy = world.TryGet<EnemyBrain2D>(hit.defender)) {
        const bool attackerInFront = (defEnemy->moveDir > 0 && atkTransform.x > defTransform.x) ||
                                     (defEnemy->moveDir < 0 && atkTransform.x < defTransform.x);
        if (defEnemy->kind == EnemyKind::LancerGuard &&
            defEnemy->aiState == EnemyState::Patrol &&
            attackerInFront)
        {
            if ( auto* atkStk = world.TryGet<gameplay::MotionStack2D>(hit.attacker)) {
                atkStk->impulseVx = -hitDirX * 6.5f;
            }
            pipeline.EmitCue(gameplay::SimCue{
                .assetId = Assets::kCueParryClang.value,
                .worldX  = midX,
                .worldY  = midY,
                .param0  = 4,
                .kind    = gameplay::SimCueKind::AudioOneShot
            });
            return;
        }
    }

    // 3. Determine Damage, Knockback, and Hitstop Magnitude
    i16 damage = 15;
    f32 knockbackX = hitDirX * 6.0f;
    f32 knockbackY = 3.5f;
    u16 hitstopTicks = static_cast<u16>(session->tuning.hitstopLightTicks);

    if (auto* atkPlayer = world.TryGet<PlayerController2D>(hit.attacker)) {
        if (attackerFsm != nullptr) {
            const auto st = static_cast<PlayerState>(attackerFsm->stateId);
            if (st == PlayerState::AttackSlash2) {
                damage = 20;
                knockbackX = hitDirX * 7.5f;
            } else if (st == PlayerState::AttackSlam3 || st == PlayerState::ParryRiposte) {
                damage = 35;
                knockbackX = hitDirX * 11.0f;
                knockbackY = 6.5f;
                hitstopTicks = static_cast<u16>(session->tuning.hitstopHeavyTicks);
            } else if (st == PlayerState::AirSlash) {
                damage = 18;
                // Pogo bounce if striking downward
                if (atkTransform.y > defTransform.y + 0.3f &&
                    world.TryGet<gameplay::MotionStack2D>(hit.attacker) != nullptr) {
                    auto* pStk = world.TryGet<gameplay::MotionStack2D>(hit.attacker);
                    pStk->impulseVy = 11.5f;
                    atkPlayer->airDashesRemaining = 1;
                    atkPlayer->airJumpsRemaining  = atkPlayer->maxAirJumps;
                }
            }
        }
        // Grant soul energy and promote onHitCancelMask into live cancelMask
        atkPlayer->soulEnergy = static_cast<u16>(
            (atkPlayer->soulEnergy + 12u > 100u) ? 100u : (atkPlayer->soulEnergy + 12u));
        if (attackerFsm != nullptr) {
            attackerFsm->cancelMask |= attackerFsm->onHitCancelMask;
        }
    } else {
        // Enemy or Hazard striking Player
        damage = 1; // Hollow-Knight style 1-mask discrete health pips for Player
        knockbackX = hitDirX * 9.0f;
        knockbackY = 7.5f;
        hitstopTicks = static_cast<u16>(session->tuning.hitstopHeavyTicks);
    }

    // 4. Apply Damage, IFrames, Poise Break & Knockback
    defenderDmg->hp = static_cast<i16>(defenderDmg->hp - damage);
    defenderDmg->iframeTicks = (defenderDmg->teamId == TeamIds::kPlayer) ? 90u : 14u;
    defenderDmg->stunTicks   = 18u;

    if (defenderStk != nullptr) {
        defenderStk->impulseVx = knockbackX;
        defenderStk->impulseVy = knockbackY;
        defenderStk->overrideVx = knockbackX;
        defenderStk->overrideVy = knockbackY;
        defenderStk->overrideTicks = 10;
        defenderStk->activePriority = 10;
    }
    if (defenderMot != nullptr) {
        defenderMot->vx = knockbackX;
        defenderMot->vy = knockbackY;
    }

    // 5. Update Defender State Machine (Player Hurt/Death or Enemy Stagger/Death)
    if (auto* defPlayer = world.TryGet<PlayerController2D>(hit.defender)) {
        auto* defFsm  = world.TryGet<gameplay::StateMachine2D>(hit.defender);
        auto* defAnim = world.TryGet<world::SpriteAnimator2D>(hit.defender);
        auto* defRig  = world.TryGet<gameplay::HitboxRig2D>(hit.defender);
        if (defFsm != nullptr && defRig != nullptr) {
            const PlayerState targetSt = (defenderDmg->hp <= 0) ? PlayerState::Dead : PlayerState::HurtStun;
            TransitionPlayerState(*defFsm, defAnim, *defRig, targetSt, 11, 6, 4, false);
        }
        pipeline.EmitCue(gameplay::SimCue{
            .assetId = Assets::kCuePlayerHurt.value,
            .worldX  = midX,
            .worldY  = midY,
            .param0  = hitstopTicks,
            .kind    = gameplay::SimCueKind::AudioOneShot
        });
    } else if (auto* defEnemy = world.TryGet<EnemyBrain2D>(hit.defender)) {
        if (defenderDmg->hp <= 0) {
            defEnemy->aiState = EnemyState::Dead;
            defEnemy->aiTimerTicks = 30;
            if (defEnemy->worldFlagOnDeath != 0) {
                pipeline.SetWorldFlag(defEnemy->worldFlagOnDeath, true);
                session->ShowBanner("CHAMBER GUARDIAN VANQUISHED", 240);
            }
        } else if (defEnemy->aiState != EnemyState::Parried) {
            defEnemy->aiState = EnemyState::Hurt;
            defEnemy->aiTimerTicks = 18;
        }
        pipeline.EmitCue(gameplay::SimCue{
            .assetId = Assets::kCueHitFlesh.value,
            .worldX  = midX,
            .worldY  = midY,
            .param0  = hitstopTicks,
            .kind    = gameplay::SimCueKind::AudioOneShot
        });
    }
    (void)step;
}

} // namespace

// ============================================================================
// Component Registration
// ============================================================================
Result<void, u8> RegisterClientComponents(world::World& world, void* /*appState*/) noexcept {
    if (auto r = world.RegisterComponentPool<PlayerController2D>(8); !r) {
        return Err(static_cast<u8>(r.error()));
    }
    if (auto r = world.RegisterComponentPool<EnemyBrain2D>(64); !r) {
        return Err(static_cast<u8>(r.error()));
    }
    if (auto r = world.RegisterComponentPool<RoomTriggerVolume2D>(32); !r) {
        return Err(static_cast<u8>(r.error()));
    }
    if (auto r = world.RegisterComponentPool<TransientVfx2D>(128); !r) {
        return Err(static_cast<u8>(r.error()));
    }
    if (auto r = world.RegisterComponentPool<RoomTransientTag>(256); !r) {
        return Err(static_cast<u8>(r.error()));
    }
    if (auto r = world.RegisterComponentPool<WaypointCarrierDriver2D>(16); !r) {
        return Err(static_cast<u8>(r.error()));
    }
    return Ok;
}

// ============================================================================
// Default Input Map (Keyboard + SDL3 Standard Gamepad)
// ============================================================================
void ConfigureDefaultInputBindings(platform::input::InputPipeline& input) noexcept {
    using platform::input::Source;
    auto& map = input.Map();

    // Standard USB HID Scancodes (A=4, D=7, W=26, S=22, Space=44, J=13, K=14, L=15, I=12, E=8, Esc=41, Enter=40)
    // Arrow keys: Right=79, Left=80, Down=81, Up=82; Shift=225
    map.Bind(GameAction::MoveLeft,       {Source::Key(4),   Source::Key(80), Source::Pad(13)});
    map.Bind(GameAction::MoveRight,      {Source::Key(7),   Source::Key(79), Source::Pad(14)});
    map.Bind(GameAction::LookUp,         {Source::Key(26),  Source::Key(82), Source::Pad(11)});
    map.Bind(GameAction::Crouch,         {Source::Key(22),  Source::Key(81), Source::Pad(12)});
    map.Bind(GameAction::Jump,           {Source::Key(44),  Source::Key(29), Source::Pad(0)});  // Space / Z / South
    map.Bind(GameAction::AttackPrimary,  {Source::Key(13),  Source::Key(27), Source::Mouse(1), Source::Pad(2)}); // J / X / LMB / West
    map.Bind(GameAction::SkillSecondary, {Source::Key(12),  Source::Pad(3)});                   // I / North
    map.Bind(GameAction::Dash,           {Source::Key(15),  Source::Key(225), Source::Pad(10)}); // L / LShift / RB
    map.Bind(GameAction::Parry,          {Source::Key(14),  Source::Mouse(3), Source::Pad(9)});  // K / RMB / LB
    map.Bind(GameAction::Interact,       {Source::Key(8),   Source::Key(26),  Source::Pad(1)});  // E / W / East
    map.Bind(GameAction::Pause,          {Source::Key(41),  Source::Pad(6)});                   // Esc / Start
    map.Bind(GameAction::MenuConfirm,    {Source::Key(40),  Source::Key(44),  Source::Pad(0)});  // Enter / Space / South
    map.Bind(GameAction::MenuCancel,     {Source::Key(41),  Source::Pad(1)});                   // Esc / East
    map.Bind(GameAction::QuickRestart,   {Source::Key(21),  Source::Pad(4)});                   // R / Back
}

// ============================================================================
// GameplayPolicyTable Wiring
// ============================================================================
gameplay::GameplayPolicyTable MakeGameplayPolicyTable(GameSessionState* session) noexcept {
    gameplay::GameplayPolicyTable table{};
    table.userContext  = session;
    table.onCombatHit  = &OnResolveCombatHitPolicy;
    return table;
}

// ============================================================================
// SimCue Presentation Consumer (Audio, Camera Trauma, Hit Sparks)
// ============================================================================
void ConsumeClientSimCue(
    const gameplay::SimCue& cue,
    world::World& world,
    const assets::AssetBank& /*assetBank*/,
    void* userAppState) noexcept
{
    auto* session = static_cast<GameSessionState*>(userAppState);
    if (session == nullptr) return;

    // Apply directional camera kick & trauma on heavy combat impacts
    if (cue.param0 > 0) {
        if (auto* cam = world.TryGet<gameplay::CameraController2D>(session->cameraEntity)) {
            const u16 addTrauma = static_cast<u16>(cue.param0 * 2800u);
            cam->trauma_q16 = static_cast<u16>(
                (cam->trauma_q16 + addTrauma > 65535u) ? 65535u : (cam->trauma_q16 + addTrauma));
            cam->kickVelY_q8 = 512;
        }
        SpawnHitSparkVfx(world, cue.worldX, cue.worldY, renderer::Color{1.0f, 0.85f, 0.35f, 1.0f}, 1.4f, 14);
    }
}

// ============================================================================
// Player Kinematics, Coyote/Buffer Forgiveness, Wall Tech & Combat FSM
// ============================================================================
void StepPlayerController(
    GameSessionState& session,
    world::World& world,
    gameplay::GameplayPipeline& gameplay,
    audio::AudioSystem& audio,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    const platform::input::TickInput& tickInput,
    platform::input::ActionBuffer& actionBuffer) noexcept
{
    if (step.IsFrozen(time::TimeChannel::Player)) {
        return;
    }

    const world::EntityId playerId = session.playerEntity;
    if (!world.IsAlive(playerId)) return;

    auto* ctrl = world.TryGet<PlayerController2D>(playerId);
    auto* mot  = world.TryGet<gameplay::KinematicMotor2D>(playerId);
    auto* stk  = world.TryGet<gameplay::MotionStack2D>(playerId);
    auto* fsm  = world.TryGet<gameplay::StateMachine2D>(playerId);
    auto* rig  = world.TryGet<gameplay::HitboxRig2D>(playerId);
    auto* dmg  = world.TryGet<gameplay::DamageReceiver2D>(playerId);
    auto* anim = world.TryGet<world::SpriteAnimator2D>(playerId);
    if (!ctrl || !mot || !stk || !fsm || !rig || !dmg) return;

    const auto& tuning = session.tuning;

    // 1. Decrement Timers & Cooldowns
    if (ctrl->dashCooldownTicks > 0)   --ctrl->dashCooldownTicks;
    if (ctrl->dashActiveTicks > 0)     --ctrl->dashActiveTicks;
    if (ctrl->wallJumpLockTicks > 0)   --ctrl->wallJumpLockTicks;
    if (ctrl->comboResetTicks > 0)     --ctrl->comboResetTicks;
    if (ctrl->parryWindowTicks > 0)    --ctrl->parryWindowTicks;
    if (ctrl->portalCooldownTicks > 0) --ctrl->portalCooldownTicks;
    if (dmg->iframeTicks > 0)          --dmg->iframeTicks;
    if (dmg->stunTicks > 0)            --dmg->stunTicks;
    ++fsm->stateTicks;

    // 2. Check Crushed / Hazard Pit Recovery via SafeGroundBreadcrumb2D
    if ((static_cast<u8>(mot->flags) & static_cast<u8>(gameplay::MotorFlags::Crushed)) != 0) {
        if (auto* crumb = world.TryGet<gameplay::SafeGroundBreadcrumb2D>(playerId)) {
            world::LocalTransform2D safeLoc = world.GetLocalTransform(playerId);
            safeLoc.x = crumb->safeX;
            safeLoc.y = crumb->safeY;
            world.Teleport(playerId, safeLoc);
            mot->vx = 0.0f;
            mot->vy = 0.0f;
            dmg->hp = static_cast<i16>(dmg->hp - 1);
            dmg->iframeTicks = 90;
        }
    }

    // 3. Evaluate Ground & Wall Contacts + Coyote Forgiveness Counters
    const bool onGround    = HasContact(mot->contacts, gameplay::ContactFlags::Ground);
    const bool onWallLeft  = HasContact(mot->contacts, gameplay::ContactFlags::WallLeft);
    const bool onWallRight = HasContact(mot->contacts, gameplay::ContactFlags::WallRight);

    if (onGround) {
        stk->groundCoyoteTicks   = static_cast<u8>(tuning.coyoteTicks);
        ctrl->airJumpsRemaining  = HasAbility(ctrl->unlockedAbilities, AbilityMask::DoubleJump) ? ctrl->maxAirJumps : 0u;
        ctrl->airDashesRemaining = 1;
        if (!ctrl->wasGrounded && mot->vy <= 0.0f) {
            const auto& wt = world.GetWorldTransform(playerId);
            (void)audio.PlayCue(Assets::kCueLand, assetBank, wt.x, wt.y, playerId);
        }
    } else if (stk->groundCoyoteTicks > 0) {
        --stk->groundCoyoteTicks;
    }

    if ((onWallLeft || onWallRight) && !onGround && HasAbility(ctrl->unlockedAbilities, AbilityMask::WallCling)) {
        stk->wallCoyoteTicks = static_cast<u8>(tuning.coyoteTicks);
        // Pack wall sign into bit 2..3 of packedSigns (0 = Left/-1, 2 = Right/+1)
        const u8 wallSignBits = onWallRight ? 2u : 0u;
        stk->packedSigns = static_cast<u8>((stk->packedSigns & ~0x0Cu) | (wallSignBits << 2));
    } else if (stk->wallCoyoteTicks > 0) {
        --stk->wallCoyoteTicks;
    }
    ctrl->wasGrounded = onGround ? 1u : 0u;

    // 4. Handle Dead or HurtStun States
    const auto curState = static_cast<PlayerState>(fsm->stateId);
    if (curState == PlayerState::Dead) {
        stk->targetWalkVx = 0.0f;
        return;
    }
    if (curState == PlayerState::HurtStun) {
        if (fsm->stateTicks >= 20) {
            TransitionPlayerState(*fsm, anim, *rig, onGround ? PlayerState::Idle : PlayerState::Fall, 0, 8, 4, true);
        } else {
            return;
        }
    }

    // 5. Evaluate Directional Input
    f32 moveInputX = 0.0f;
    if (tickInput.Held(GameAction::MoveRight)) moveInputX += 1.0f;
    if (tickInput.Held(GameAction::MoveLeft))  moveInputX -= 1.0f;
    if (moveInputX == 0.0f) {
        moveInputX = tickInput.Axis(GameAxis::MoveX);
    }

    // Drop through one-way platforms on Crouch + Jump
    if (onGround && HasContact(mot->contacts, gameplay::ContactFlags::OnOneWay) &&
        tickInput.Held(GameAction::Crouch) && actionBuffer.Peek(GameAction::Jump, 6))
    {
        (void)actionBuffer.Consume(GameAction::Jump, 6);
        mot->oneWayIgnoreTicks = 18;
        mot->vy = -2.0f;
        return;
    }

    // 6. Combat & Evasion Cancel Window Evaluation
    const bool inAttackOrParry =
        curState == PlayerState::AttackSlash1 ||
        curState == PlayerState::AttackSlash2 ||
        curState == PlayerState::AttackSlam3  ||
        curState == PlayerState::AirSlash     ||
        curState == PlayerState::ParryStance  ||
        curState == PlayerState::ParryRiposte;

    const bool freeToAct = !inAttackOrParry && curState != PlayerState::Dash;

    // 6a. Shadow Dash Execution (Ground or Air, cancels attacks if EvadeDash bit set)
    if (HasAbility(ctrl->unlockedAbilities, AbilityMask::ShadowDash) &&
        ctrl->dashCooldownTicks == 0 &&
        (onGround || ctrl->airDashesRemaining > 0) &&
        (freeToAct || CanCancelInto(fsm->cancelMask, gameplay::CancelCategory::EvadeDash)) &&
        actionBuffer.Consume(GameAction::Dash, 10))
    {
        if (!onGround) --ctrl->airDashesRemaining;
        if (moveInputX > 0.1f)       ctrl->facingSign = 2;
        else if (moveInputX < -0.1f) ctrl->facingSign = 0;

        const f32 dashDir = (ctrl->facingSign > 0) ? 1.0f : -1.0f;
        ctrl->dashActiveTicks   = static_cast<u16>(tuning.dashDurationTicks);
        ctrl->dashCooldownTicks = static_cast<u16>(tuning.dashCooldownTicks);
        dmg->iframeTicks        = static_cast<u16>(tuning.dashDurationTicks); // Shadow-dash through enemy bodies

        stk->overrideVx     = dashDir * tuning.dashSpeed;
        stk->overrideVy     = 0.0f;
        stk->overrideTicks  = static_cast<u8>(tuning.dashDurationTicks);
        stk->activePriority = 8;
        stk->gravityScale_q8 = 0;

        TransitionPlayerState(*fsm, anim, *rig, PlayerState::Dash, 6, 3, 5, false);
        const auto& wt = world.GetWorldTransform(playerId);
        (void)audio.PlayCue(Assets::kCueDash, assetBank, wt.x, wt.y, playerId);
        SpawnHitSparkVfx(world, wt.x, wt.y, renderer::Color{0.35f, 0.75f, 1.0f, 0.85f}, 1.0f, 14);
        return;
    }

    if (curState == PlayerState::Dash) {
        if (ctrl->dashActiveTicks <= 4) {
            // Open dash-jump and dash-attack cancel window in final 4 ticks
            fsm->cancelMask = static_cast<u16>(
                gameplay::CancelCategory::Jump | gameplay::CancelCategory::PrimaryAttack);
        }
        if (ctrl->dashActiveTicks == 0) {
            stk->gravityScale_q8 = 256;
            TransitionPlayerState(*fsm, anim, *rig, onGround ? PlayerState::Idle : PlayerState::Fall, 0, 8, 4, true);
        } else if (!CanCancelInto(fsm->cancelMask, gameplay::CancelCategory::Jump)) {
            return;
        }
    }

    // 6b. Defensive Parry Stance (Ground only)
    if (onGround &&
        (freeToAct || CanCancelInto(fsm->cancelMask, gameplay::CancelCategory::DefensiveParry)) &&
        actionBuffer.Consume(GameAction::Parry, 8))
    {
        ctrl->parryWindowTicks = 16; // 133ms active parry window
        stk->targetWalkVx = 0.0f;
        TransitionPlayerState(*fsm, anim, *rig, PlayerState::ParryStance, 9, 4, 5, false);
        return;
    }

    // 6c. Melee Combo Chain & Air Slash
    if ((freeToAct || CanCancelInto(fsm->cancelMask, gameplay::CancelCategory::PrimaryAttack)) &&
        actionBuffer.Consume(GameAction::AttackPrimary, static_cast<u8>(tuning.jumpBufferTicks)))
    {
        if (moveInputX > 0.1f)       ctrl->facingSign = 2;
        else if (moveInputX < -0.1f) ctrl->facingSign = 0;
        const f32 faceDir = (ctrl->facingSign > 0) ? 1.0f : -1.0f;

        // Increment swingId and clear SplitMix hit bloom filter so new swing hits cleanly
        ++rig->swingId;
        rig->hitBloomMask = 0;

        if (!onGround) {
            TransitionPlayerState(*fsm, anim, *rig, PlayerState::AirSlash, 8, 3, 6, false);
        } else {
            if (ctrl->comboResetTicks == 0) ctrl->comboStep = 0;
            if (ctrl->comboStep == 0) {
                TransitionPlayerState(*fsm, anim, *rig, PlayerState::AttackSlash1, 5, 3, 6, false);
                stk->impulseVx = faceDir * 3.5f;
                ctrl->comboStep = 1;
            } else if (ctrl->comboStep == 1) {
                TransitionPlayerState(*fsm, anim, *rig, PlayerState::AttackSlash2, 6, 3, 6, false);
                stk->impulseVx = faceDir * 4.5f;
                ctrl->comboStep = 2;
            } else {
                TransitionPlayerState(*fsm, anim, *rig, PlayerState::AttackSlam3, 7, 4, 8, false);
                stk->impulseVx = faceDir * 7.0f;
                ctrl->comboStep = 0;
            }
            ctrl->comboResetTicks = 48; // 400ms to chain next combo step
        }

        const auto& wt = world.GetWorldTransform(playerId);
        (void)audio.PlayCue(Assets::kCueSlash, assetBank, wt.x, wt.y, playerId);
    }

    // 6d. Step Active Attack / Parry Frame Windows (Fallback alongside 'ERIG' directives)
    if (inAttackOrParry) {
        const f32 faceDir = (ctrl->facingSign > 0) ? 1.0f : -1.0f;
        const u16 t = fsm->stateTicks;
        const u16 totalDuration = (curState == PlayerState::AttackSlam3) ? 30u : 20u;

        if (onGround) {
            stk->targetWalkVx = 0.0f;
        } else {
            stk->targetWalkVx = moveInputX * tuning.runSpeed * 0.75f;
        }

        // Active hitbox window on ticks [5..11]
        if (curState != PlayerState::ParryStance && t >= 5 && t <= 11) {
            rig->activeSliceIndex = 0;
            const i16 reachQ8 = (curState == PlayerState::AttackSlam3) ? MetersToQ8(1.45f) : MetersToQ8(1.15f);
            rig->primaryHitbox = gameplay::CompactAabb2D{
                .offsetX_q8 = static_cast<i16>(faceDir * reachQ8),
                .offsetY_q8 = MetersToQ8(0.1f),
                .halfW_q8   = MetersToQ8(0.95f),
                .halfH_q8   = MetersToQ8(0.65f)
            };
            fsm->onHitCancelMask = static_cast<u16>(
                gameplay::CancelCategory::PrimaryAttack |
                gameplay::CancelCategory::EvadeDash |
                gameplay::CancelCategory::Jump);
        } else {
            rig->activeSliceIndex = 0xFF;
        }

        // Open natural recovery cancel window on ticks [12..end]
        if (t >= 12) {
            fsm->cancelMask |= static_cast<u16>(
                gameplay::CancelCategory::PrimaryAttack |
                gameplay::CancelCategory::EvadeDash |
                gameplay::CancelCategory::Jump);
        }

        if (t >= totalDuration) {
            TransitionPlayerState(*fsm, anim, *rig, onGround ? PlayerState::Idle : PlayerState::Fall, 0, 8, 4, true);
        } else {
            return;
        }
    }

    // 7. Jump Execution (Coyote Ground Jump, Wall Jump, Air Double Jump)
    if (actionBuffer.Peek(GameAction::Jump, static_cast<u8>(tuning.jumpBufferTicks))) {
        if (stk->groundCoyoteTicks > 0) {
            // Ground / Coyote Jump
            (void)actionBuffer.Consume(GameAction::Jump, static_cast<u8>(tuning.jumpBufferTicks));
            stk->groundCoyoteTicks = 0;
            stk->gravityScale_q8   = 256;
            stk->impulseVy         = tuning.jumpImpulse;
            mot->vy                = tuning.jumpImpulse;
            TransitionPlayerState(*fsm, anim, *rig, PlayerState::JumpRise, 2, 6, 3, false);

            const auto& wt = world.GetWorldTransform(playerId);
            (void)audio.PlayCue(Assets::kCueJump, assetBank, wt.x, wt.y, playerId);
        } else if (stk->wallCoyoteTicks > 0) {
            // Wall Jump Kick-Off
            (void)actionBuffer.Consume(GameAction::Jump, static_cast<u8>(tuning.jumpBufferTicks));
            const bool wallIsRight = ((stk->packedSigns >> 2) & 0x03u) != 0;
            const f32 kickDirX     = wallIsRight ? -1.0f : 1.0f;

            stk->wallCoyoteTicks   = 0;
            stk->gravityScale_q8   = 256;
            ctrl->facingSign       = kickDirX > 0.0f ? 2u : 0u;
            ctrl->wallJumpLockTicks = 14; // Lock horizontal input briefly so player clears the wall

            stk->overrideVx     = kickDirX * tuning.wallJumpVelX;
            stk->overrideVy     = tuning.wallJumpVelY;
            stk->overrideTicks  = 10;
            stk->activePriority = 6;
            mot->vx             = kickDirX * tuning.wallJumpVelX;
            mot->vy             = tuning.wallJumpVelY;

            TransitionPlayerState(*fsm, anim, *rig, PlayerState::WallJump, 2, 5, 3, false);
            const auto& wt = world.GetWorldTransform(playerId);
            (void)audio.PlayCue(Assets::kCueJump, assetBank, wt.x, wt.y, playerId);
        } else if (ctrl->airJumpsRemaining > 0) {
            // Monarch Wings Double Jump
            (void)actionBuffer.Consume(GameAction::Jump, static_cast<u8>(tuning.jumpBufferTicks));
            --ctrl->airJumpsRemaining;
            stk->gravityScale_q8 = 256;
            stk->impulseVy       = tuning.doubleJumpImpulse;
            mot->vy              = tuning.doubleJumpImpulse;
            TransitionPlayerState(*fsm, anim, *rig, PlayerState::JumpRise, 2, 5, 3, false);

            const auto& wt = world.GetWorldTransform(playerId);
            (void)audio.PlayCue(Assets::kCueJump, assetBank, wt.x, wt.y, playerId);
            SpawnHitSparkVfx(world, wt.x, wt.y - 0.5f, renderer::Color{0.8f, 0.9f, 1.0f, 0.9f}, 1.2f, 12);
        }
    }

    // Variable-Height Jump Release Cut
    if (tickInput.Released(GameAction::Jump) && mot->vy > 2.5f) {
        mot->vy *= tuning.jumpReleaseCutFactor;
    }

    // 8. Horizontal Walk Speed, Wall Slide Clamping & Visual Facing
    if (ctrl->wallJumpLockTicks == 0) {
        stk->targetWalkVx = moveInputX * tuning.runSpeed;
        if (moveInputX > 0.1f)       ctrl->facingSign = 2;
        else if (moveInputX < -0.1f) ctrl->facingSign = 0;
    }

    // Sync facing sign into MotionStack2D::packedSigns (bits 0..1) and LocalTransform2D::scaleX
    stk->packedSigns = static_cast<u8>((stk->packedSigns & ~0x03u) | (ctrl->facingSign & 0x03u));
    world::LocalTransform2D loc = world.GetLocalTransform(playerId);
    const f32 absScaleX = AbsF32(loc.scaleX) > 0.01f ? AbsF32(loc.scaleX) : 1.0f;
    loc.scaleX = (ctrl->facingSign > 0) ? absScaleX : -absScaleX;
    world.SetLocalTransform(playerId, loc);

    // Wall Slide Clamping when pressing toward wall while falling
    const bool pressingTowardWall = (onWallLeft && moveInputX < -0.1f) || (onWallRight && moveInputX > 0.1f);
    if (!onGround && pressingTowardWall && mot->vy < 0.0f &&
        HasAbility(ctrl->unlockedAbilities, AbilityMask::WallCling))
    {
        if (mot->vy < tuning.wallSlideMaxFallSpeed) {
            mot->vy = tuning.wallSlideMaxFallSpeed;
        }
        if (curState != PlayerState::WallSlide) {
            TransitionPlayerState(*fsm, anim, *rig, PlayerState::WallSlide, 4, 8, 2, true);
        }
    } else if (onGround) {
        if (AbsF32(moveInputX) > 0.1f) {
            if (curState != PlayerState::Run) {
                TransitionPlayerState(*fsm, anim, *rig, PlayerState::Run, 1, 6, 6, true);
            }
        } else if (tickInput.Held(GameAction::Crouch)) {
            stk->targetWalkVx = 0.0f;
            if (curState != PlayerState::Crouch) {
                TransitionPlayerState(*fsm, anim, *rig, PlayerState::Crouch, 0, 10, 2, true);
            }
        } else if (curState != PlayerState::Idle) {
            TransitionPlayerState(*fsm, anim, *rig, PlayerState::Idle, 0, 10, 4, true);
        }
    } else {
        if (mot->vy > 0.2f && curState != PlayerState::JumpRise && curState != PlayerState::WallJump) {
            TransitionPlayerState(*fsm, anim, *rig, PlayerState::JumpRise, 2, 6, 3, false);
        } else if (mot->vy <= 0.2f && curState != PlayerState::Fall) {
            TransitionPlayerState(*fsm, anim, *rig, PlayerState::Fall, 3, 6, 3, true);
        }
    }

    // 9. Update Camera Lookahead & Vertical Peek
    if (auto* cam = world.TryGet<gameplay::CameraController2D>(session.cameraEntity)) {
        const f32 lookX = (ctrl->facingSign > 0) ? 2.2f : -2.2f;
        cam->lookaheadX_q8 = MetersToQ8(lookX);

        f32 peekY = 0.0f;
        if (onGround && AbsF32(moveInputX) < 0.1f) {
            if (tickInput.HeldTicks(GameAction::LookUp) > 35)      peekY = 3.5f;
            else if (tickInput.HeldTicks(GameAction::Crouch) > 35) peekY = -3.5f;
        }
        cam->peekOffsetY_q8 = MetersToQ8(peekY);
    }

    (void)gameplay;
}

// ============================================================================
// Enemy AI Behaviors & Moving Platform Carriers
// ============================================================================
void StepEnemiesAndCarriers(
    GameSessionState& session,
    world::World& world,
    gameplay::GameplayPipeline& gameplay,
    audio::AudioSystem& audio,
    const assets::AssetBank& assetBank,
    time::SimStepView step) noexcept
{
    // 1. Step Moving Platform Carriers (WaypointCarrierDriver2D -> KinematicCarrier2D)
    if (!step.IsFrozen(time::TimeChannel::World)) {
        for (auto [ent, driver, carrier] : world.View<WaypointCarrierDriver2D, gameplay::KinematicCarrier2D>()) {
            if (driver.requiredWorldFlag != 0 && !gameplay.TestWorldFlag(driver.requiredWorldFlag)) {
                carrier.deltaX = 0.0f;
                carrier.deltaY = 0.0f;
                continue;
            }
            driver.phaseTicks = (driver.phaseTicks + 1u) % driver.cycleTicks;
            const f32 halfCycle = 0.5f * static_cast<f32>(driver.cycleTicks);
            const f32 rawT = static_cast<f32>(driver.phaseTicks) / halfCycle;
            const f32 pingPong = (rawT <= 1.0f) ? rawT : (2.0f - rawT);
            // Smoothstep easing at endpoints
            const f32 smoothT = pingPong * pingPong * (3.0f - 2.0f * pingPong);

            const f32 targetX = driver.startX + (driver.endX - driver.startX) * smoothT;
            const f32 targetY = driver.startY + (driver.endY - driver.startY) * smoothT;

            world::LocalTransform2D loc = world.GetLocalTransform(ent);
            carrier.deltaX = targetX - loc.x;
            carrier.deltaY = targetY - loc.y;
            loc.x = targetX;
            loc.y = targetY;
            world.SetLocalTransform(ent, loc);
        }
    }

    // 2. Step Enemy AI State Machines
    if (step.IsFrozen(time::TimeChannel::Enemy)) {
        return;
    }

    const bool playerAlive = world.IsAlive(session.playerEntity);
    const world::WorldTransform2D playerWt = playerAlive
        ? world.GetWorldTransform(session.playerEntity)
        : world::WorldTransform2D{};

    const auto& spatial = gameplay.Spatial();

    for (auto [ent, brain, mot, stk, rig, dmg] :
         world.View<EnemyBrain2D, gameplay::KinematicMotor2D, gameplay::MotionStack2D,
                    gameplay::HitboxRig2D, gameplay::DamageReceiver2D>())
    {
        if (dmg.iframeTicks > 0) --dmg.iframeTicks;
        if (dmg.stunTicks > 0)   --dmg.stunTicks;
        if (brain.attackCooldownTicks > 0) --brain.attackCooldownTicks;
        if (brain.aiTimerTicks > 0)        --brain.aiTimerTicks;

        const auto& wt = world.GetWorldTransform(ent);
        const f32 dxToPlayer = playerWt.x - wt.x;
        const f32 dyToPlayer = playerWt.y - wt.y;
        const f32 distSq     = dxToPlayer * dxToPlayer + dyToPlayer * dyToPlayer;

        // Handle Death State
        if (brain.aiState == EnemyState::Dead) {
            stk.targetWalkVx     = 0.0f;
            rig.activeSliceIndex = 0xFF;
            if (brain.aiTimerTicks == 0) {
                SpawnHitSparkVfx(world, wt.x, wt.y, renderer::Color{0.9f, 0.25f, 0.2f, 1.0f}, 1.8f, 22);
                (void)world.DeferDestroy(ent);
            }
            continue;
        }

        // Handle Hurt / Parried Stagger States
        if (brain.aiState == EnemyState::Hurt || brain.aiState == EnemyState::Parried) {
            stk.targetWalkVx     = 0.0f;
            rig.activeSliceIndex = 0xFF;
            if (brain.aiTimerTicks == 0) {
                brain.aiState = EnemyState::Patrol;
            }
            continue;
        }

        // Ledge & Wall Detection via SpatialIndex2D::Raycast
        const f32 lookAheadX = wt.x + static_cast<f32>(brain.moveDir) * 0.75f;
        const auto groundRay = spatial.Raycast(
            lookAheadX, wt.y, 0.0f, -1.0f, 1.5f,
            CollisionLayers::kStaticWorld | CollisionLayers::kOneWay, ent);
        const bool hitWall =
            (brain.moveDir > 0 && HasContact(mot.contacts, gameplay::ContactFlags::WallRight)) ||
            (brain.moveDir < 0 && HasContact(mot.contacts, gameplay::ContactFlags::WallLeft));

        switch (brain.kind) {
            case EnemyKind::CrawlerHusk: {
                // Simple Ledge-Respecting Patrol Crawler (Constant contact hurtbox)
                if (hitWall || !groundRay.hit || AbsF32(wt.x - brain.homeX) > brain.patrolRadius) {
                    brain.moveDir = static_cast<i8>(-brain.moveDir);
                }
                stk.targetWalkVx     = static_cast<f32>(brain.moveDir) * 2.6f;
                rig.activeSliceIndex = 0;
                rig.primaryHitbox    = rig.hurtbox;
                break;
            }

            case EnemyKind::LancerGuard:
            case EnemyKind::WardenBoss: {
                const f32 moveSpeed = (brain.kind == EnemyKind::WardenBoss) ? 4.8f : 3.2f;
                const f32 lungeVx   = (brain.kind == EnemyKind::WardenBoss) ? 14.5f : 10.5f;

                if (brain.aiState == EnemyState::Patrol) {
                    rig.activeSliceIndex = 0xFF;
                    if (playerAlive && distSq <= brain.aggroRange * brain.aggroRange && AbsF32(dyToPlayer) < 3.0f) {
                        brain.moveDir = (dxToPlayer >= 0.0f) ? 1 : -1;
                        if (AbsF32(dxToPlayer) <= brain.attackRange && brain.attackCooldownTicks == 0) {
                            brain.aiState      = EnemyState::Windup;
                            brain.aiTimerTicks = (brain.kind == EnemyKind::WardenBoss) ? 28u : 36u;
                            stk.targetWalkVx   = 0.0f;
                            SpawnHitSparkVfx(world, wt.x, wt.y + 0.9f, renderer::Color{1.0f, 0.2f, 0.2f, 1.0f}, 0.7f, 16);
                        } else if (groundRay.hit) {
                            stk.targetWalkVx = static_cast<f32>(brain.moveDir) * moveSpeed;
                        } else {
                            stk.targetWalkVx = 0.0f;
                        }
                    } else {
                        if (hitWall || !groundRay.hit || AbsF32(wt.x - brain.homeX) > brain.patrolRadius) {
                            brain.moveDir = static_cast<i8>(-brain.moveDir);
                        }
                        stk.targetWalkVx = static_cast<f32>(brain.moveDir) * (moveSpeed * 0.6f);
                    }
                } else if (brain.aiState == EnemyState::Windup) {
                    stk.targetWalkVx = 0.0f;
                    if (brain.aiTimerTicks == 0) {
                        brain.aiState      = EnemyState::Attack;
                        brain.aiTimerTicks = 16;
                        ++rig.swingId;
                        rig.hitBloomMask     = 0;
                        rig.activeSliceIndex = 0;
                        rig.primaryHitbox    = gameplay::CompactAabb2D{
                            .offsetX_q8 = MetersToQ8(static_cast<f32>(brain.moveDir) * 1.2f),
                            .offsetY_q8 = MetersToQ8(0.0f),
                            .halfW_q8   = MetersToQ8(1.1f),
                            .halfH_q8   = MetersToQ8(0.6f)
                        };
                        stk.overrideVx     = static_cast<f32>(brain.moveDir) * lungeVx;
                        stk.overrideVy     = (brain.kind == EnemyKind::WardenBoss && brain.phase == 1) ? 6.5f : 0.0f;
                        stk.overrideTicks  = 12;
                        stk.activePriority = 6;
                        brain.phase        = static_cast<u8>((brain.phase + 1u) & 1u);
                        (void)audio.PlayCue(Assets::kCueSlash, assetBank, wt.x, wt.y, ent);
                    }
                } else if (brain.aiState == EnemyState::Attack) {
                    if (brain.aiTimerTicks == 0) {
                        rig.activeSliceIndex      = 0xFF;
                        brain.aiState             = EnemyState::Recovery;
                        brain.aiTimerTicks        = 30;
                        brain.attackCooldownTicks = 60;
                    }
                } else if (brain.aiState == EnemyState::Recovery) {
                    stk.targetWalkVx     = 0.0f;
                    rig.activeSliceIndex = 0xFF;
                    if (brain.aiTimerTicks == 0) {
                        brain.aiState = EnemyState::Patrol;
                    }
                }
                break;
            }
        }

        // Sync visual facing scaleX
        world::LocalTransform2D loc = world.GetLocalTransform(ent);
        const f32 absScaleX = AbsF32(loc.scaleX) > 0.01f ? AbsF32(loc.scaleX) : 1.0f;
        loc.scaleX = (brain.moveDir > 0) ? absScaleX : -absScaleX;
        world.SetLocalTransform(ent, loc);
    }
}

// ============================================================================
// Room Portals, Save Shrines, Ability Relics & Transient VFX Stepper
// ============================================================================
void StepTriggersAndVfx(
    GameSessionState& session,
    world::World& world,
    gameplay::GameplayPipeline& gameplay,
    assets::AssetSystem& assets,
    audio::AudioSystem& audio,
    time::SimStepView step,
    const platform::input::TickInput& tickInput) noexcept
{
    // 1. Step Transient VFX particles
    if (!step.IsFrozen(time::TimeChannel::Vfx)) {
        for (auto [ent, vfx, spr] : world.View<TransientVfx2D, world::Sprite2D>()) {
            if (vfx.remainingTicks <= 1) {
                (void)world.DeferDestroy(ent);
                continue;
            }
            --vfx.remainingTicks;
            world::LocalTransform2D loc = world.GetLocalTransform(ent);
            loc.x += vfx.velX * time::kFixedDtSec;
            loc.y += vfx.velY * time::kFixedDtSec;
            world.SetLocalTransform(ent, loc);

            if (vfx.fadeAlpha != 0 && vfx.initialTicks > 0) {
                spr.tint.a = static_cast<f32>(vfx.remainingTicks) / static_cast<f32>(vfx.initialTicks);
            }
        }
    }

    // 2. Evaluate Player Overlap with RoomTriggerVolume2D
    if (!world.IsAlive(session.playerEntity)) return;
    auto* playerCtrl = world.TryGet<PlayerController2D>(session.playerEntity);
    auto* playerDmg  = world.TryGet<gameplay::DamageReceiver2D>(session.playerEntity);
    if (!playerCtrl || !playerDmg) return;

    const auto& pWt = world.GetWorldTransform(session.playerEntity);

    for (auto [trigEnt, trig] : world.View<RoomTriggerVolume2D>()) {
        const auto& tWt = world.GetWorldTransform(trigEnt);
        const bool overlaps =
            AbsF32(pWt.x - tWt.x) <= (trig.halfW + 0.45f) &&
            AbsF32(pWt.y - tWt.y) <= (trig.halfH + 0.85f);

        if (!overlaps) continue;

        switch (trig.kind) {
            case TriggerKind::RoomPortal: {
                if (playerCtrl->portalCooldownTicks == 0 &&
                    session.director != nullptr &&
                    session.director->Phase() == framework::TransitionPhase::Idle)
                {
                    const auto* roomDesc = session.FindRoom(trig.targetSceneId);
                    if (roomDesc != nullptr) {
                        session.pendingSpawnX        = trig.destSpawnX;
                        session.pendingSpawnY        = trig.destSpawnY;
                        session.pendingSpawnFacing   = static_cast<i8>(trig.destSpawnFacing);
                        session.hasPendingRoomTeleport = true;
                        session.activeRoomId         = roomDesc->sceneId;

                        (void)session.director->RequestSceneTransition(framework::SceneTransitionDesc{
                            .sceneId              = roomDesc->sceneId,
                            .manifestBlobId       = roomDesc->manifestId,
                            .collisionMapId       = roomDesc->collisionId,
                            .rootPrefabId         = roomDesc->prefabId,
                            .cueBankId            = roomDesc->audioBankId,
                            .musicTrackId         = roomDesc->musicTrackId,
                            .musicCrossfadeBlocks = 94,
                            .mode                 = framework::SceneTransitionMode::RoomSwap,
                            .waitPolicy           = framework::SceneWaitPolicy::StreamNonBlocking
                        }, assets);
                    }
                }
                break;
            }

            case TriggerKind::SaveShrine: {
                if (tickInput.Pressed(GameAction::Interact)) {
                    playerDmg->hp            = playerDmg->maxHp;
                    playerCtrl->soulEnergy   = 100;
                    session.checkpointRoomId = session.activeRoomId;
                    session.checkpointX      = tWt.x;
                    session.checkpointY      = tWt.y + 0.5f;
                    if (trig.worldFlagId != 0) {
                        gameplay.SetWorldFlag(trig.worldFlagId, true);
                    }
                    session.ShowBanner("SOUL RESTORED & ANCHORED", 180);
                    (void)audio.PlayCue(Assets::kCueRelicGet, assets.Bank(), tWt.x, tWt.y);
                }
                break;
            }

            case TriggerKind::AbilityRelic: {
                if (trig.worldFlagId == 0 || !gameplay.TestWorldFlag(trig.worldFlagId)) {
                    if (trig.worldFlagId != 0) {
                        gameplay.SetWorldFlag(trig.worldFlagId, true);
                    }
                    playerCtrl->unlockedAbilities =
                        playerCtrl->unlockedAbilities | AbilityMask::DoubleJump | AbilityMask::ShadowDash | AbilityMask::WallCling;
                    playerCtrl->maxAirJumps = 1;
                    session.ShowBanner("RELIC ACQUIRED: ABYSSAL WINGS", 240);
                    (void)audio.PlayCue(Assets::kCueRelicGet, assets.Bank(), tWt.x, tWt.y);
                    (void)world.DeferDestroy(trigEnt);
                }
                break;
            }

            case TriggerKind::SpikeHazard: {
                if (playerDmg->iframeTicks == 0 && !session.tuning.godMode) {
                    playerDmg->hp = static_cast<i16>(playerDmg->hp - 1);
                    playerDmg->iframeTicks = 90;
                    if (playerDmg->hp > 0) {
                        if (auto* crumb = world.TryGet<gameplay::SafeGroundBreadcrumb2D>(session.playerEntity)) {
                            world::LocalTransform2D safeLoc = world.GetLocalTransform(session.playerEntity);
                            safeLoc.x = crumb->safeX;
                            safeLoc.y = crumb->safeY;
                            world.Teleport(session.playerEntity, safeLoc);
                        }
                    }
                }
                break;
            }

            case TriggerKind::LockedGate: {
                if (trig.worldFlagId != 0 && gameplay.TestWorldFlag(trig.worldFlagId)) {
                    (void)world.DeferDestroy(trigEnt);
                }
                break;
            }
        }
    }
}

// ============================================================================
// Persistent Player & Camera Spawner + Room Population
// ============================================================================
world::EntityId EnsurePersistentPlayerAndCamera(
    GameSessionState& session,
    world::World& world,
    const assets::AssetBank& assetBank) noexcept
{
    if (!world.IsAlive(session.playerEntity)) {
        auto pRes = world.Spawn({
            .local = {.x = session.pendingSpawnX, .y = session.pendingSpawnY, .scaleX = 1.0f, .scaleY = 1.0f},
            .flags = static_cast<world::EntityFlags>(
                static_cast<u8>(world::EntityFlags::Spatial) | static_cast<u8>(world::EntityFlags::Teleported))
        });
        if (pRes) {
            const world::EntityId pid = *pRes;
            session.playerEntity = pid;

            (void)world.Emplace<PlayerController2D>(pid, PlayerController2D{
                .spawnX = session.pendingSpawnX,
                .spawnY = session.pendingSpawnY
            });

            (void)world.Emplace<gameplay::KinematicMotor2D>(pid, gameplay::KinematicMotor2D{
                .bounds       = {.offsetX_q8 = 0, .offsetY_q8 = 0, .halfW_q8 = MetersToQ8(0.4f), .halfH_q8 = MetersToQ8(0.85f)},
                .solidMask    = CollisionLayers::kStaticWorld | CollisionLayers::kOneWay,
                .bodyLayer    = CollisionLayers::kPlayerBody,
                .pushboxGroup = 1,
                .flags        = static_cast<gameplay::MotorFlags>(
                    static_cast<u8>(gameplay::MotorFlags::SnapToGround) |
                    static_cast<u8>(gameplay::MotorFlags::CornerCorrection) |
                    static_cast<u8>(gameplay::MotorFlags::ProjectSlopes) |
                    static_cast<u8>(gameplay::MotorFlags::TrackSafeAnchor)),
                .timeChannel  = static_cast<u8>(time::TimeChannel::Player)
            });

            (void)world.Emplace<gameplay::MotionStack2D>(pid, gameplay::MotionStack2D{
                .gravityScale_q8 = 256,
                .packedSigns     = 2 // Facing right (+1)
            });

            (void)world.Emplace<gameplay::StateMachine2D>(pid, gameplay::StateMachine2D{
                .rigBlob = assetBank.FindBlob(Assets::kPlayerAnimRig)
            });

            (void)world.Emplace<gameplay::HitboxRig2D>(pid, gameplay::HitboxRig2D{
                .hurtbox          = {.offsetX_q8 = 0, .offsetY_q8 = 0, .halfW_q8 = MetersToQ8(0.35f), .halfH_q8 = MetersToQ8(0.8f)},
                .hitLayerMask     = CollisionLayers::kEnemyBody,
                .hurtLayerMask    = CollisionLayers::kEnemyHitbox | CollisionLayers::kHazardHitbox,
                .activeSliceIndex = 0xFF
            });

            (void)world.Emplace<gameplay::DamageReceiver2D>(pid, gameplay::DamageReceiver2D{
                .hp     = 5,
                .maxHp  = 5,
                .poise  = 100,
                .teamId = TeamIds::kPlayer
            });

            (void)world.Emplace<gameplay::SafeGroundBreadcrumb2D>(pid, gameplay::SafeGroundBreadcrumb2D{
                .safeX     = session.pendingSpawnX,
                .safeY     = session.pendingSpawnY,
                .prevSafeX = session.pendingSpawnX,
                .prevSafeY = session.pendingSpawnY
            });

            (void)world.Emplace<world::Sprite2D>(pid, world::Sprite2D{
                .tint      = {0.85f, 0.92f, 1.0f, 1.0f},
                .baseDepth = 0.5f,
                .layer     = 1
            });

            (void)world.Emplace<world::SpriteAnimator2D>(pid, world::SpriteAnimator2D{
                .atlas         = assetBank.FindAtlas(Assets::kHeroAtlas),
                .firstFrame    = 0,
                .frameCount    = 4,
                .ticksPerFrame = 8,
                .loop          = 1,
                .playing       = 1
            });
        }
    }

    if (!world.IsAlive(session.cameraEntity)) {
        auto cRes = world.Spawn({
            .local = {.x = session.pendingSpawnX, .y = session.pendingSpawnY, .scaleX = 1.0f, .scaleY = 1.0f},
            .flags = static_cast<world::EntityFlags>(
                static_cast<u8>(world::EntityFlags::Spatial) | static_cast<u8>(world::EntityFlags::Teleported))
        });
        if (cRes) {
            const world::EntityId cid = *cRes;
            session.cameraEntity = cid;

            const auto* roomDesc = session.FindRoom(session.activeRoomId);
            (void)world.Emplace<gameplay::CameraController2D>(cid, gameplay::CameraController2D{
                .primaryTarget     = session.playerEntity,
                .latchedGroundY    = session.pendingSpawnY,
                .activeRoomBounds  = roomDesc ? roomDesc->cameraBounds : gameplay::CompactAabb2D{},
                .prevRoomBounds    = roomDesc ? roomDesc->cameraBounds : gameplay::CompactAabb2D{},
                .roomBlendDuration = 36,
                .deadzoneHalfW_q4  = 12, // 0.75m horizontal deadzone
                .deadzoneHalfH_q4  = 20, // 1.25m vertical deadzone
                .springFreqX_q4    = 96, // 6.0 Hz horizontal tracking
                .springFreqY_q4    = 80  // 5.0 Hz vertical tracking
            });

            (void)world.Emplace<world::CameraRig2D>(cid, world::CameraRig2D{
                .zoom     = 1.0f,
                .viewSlot = 0,
                .active   = true
            });
        }
    }

    return session.playerEntity;
}

namespace {

void SpawnRoomEnemy(
    world::World& world,
    const assets::AssetBank& assetBank,
    EnemyKind kind,
    f32 x,
    f32 y,
    f32 patrolRadius,
    u16 flagOnDeath = 0) noexcept
{
    auto eRes = world.Spawn({
        .local = {.x = x, .y = y, .scaleX = 1.0f, .scaleY = 1.0f},
        .flags = static_cast<world::EntityFlags>(
            static_cast<u8>(world::EntityFlags::Spatial) | static_cast<u8>(world::EntityFlags::Teleported))
    });
    if (!eRes) return;
    const world::EntityId id = *eRes;

    const i16 hp = (kind == EnemyKind::WardenBoss) ? 160 : ((kind == EnemyKind::LancerGuard) ? 55 : 30);
    const f32 halfW = (kind == EnemyKind::WardenBoss) ? 0.85f : 0.45f;
    const f32 halfH = (kind == EnemyKind::WardenBoss) ? 1.35f : 0.80f;

    (void)world.Emplace<RoomTransientTag>(id, RoomTransientTag{.roomSalt = 1});
    (void)world.Emplace<EnemyBrain2D>(id, EnemyBrain2D{
        .homeX            = x,
        .homeY            = y,
        .patrolRadius     = patrolRadius,
        .aggroRange       = (kind == EnemyKind::WardenBoss) ? 14.0f : 8.0f,
        .attackRange      = (kind == EnemyKind::WardenBoss) ? 2.6f : 1.8f,
        .kind             = kind,
        .aiState          = EnemyState::Patrol,
        .moveDir          = -1,
        .worldFlagOnDeath = flagOnDeath
    });
    (void)world.Emplace<gameplay::KinematicMotor2D>(id, gameplay::KinematicMotor2D{
        .bounds       = {.offsetX_q8 = 0, .offsetY_q8 = 0, .halfW_q8 = MetersToQ8(halfW), .halfH_q8 = MetersToQ8(halfH)},
        .solidMask    = CollisionLayers::kStaticWorld | CollisionLayers::kOneWay,
        .bodyLayer    = CollisionLayers::kEnemyBody,
        .pushboxGroup = 2,
        .flags        = gameplay::MotorFlags::SnapToGround,
        .timeChannel  = static_cast<u8>(time::TimeChannel::Enemy)
    });
    (void)world.Emplace<gameplay::MotionStack2D>(id, gameplay::MotionStack2D{.gravityScale_q8 = 256});
    (void)world.Emplace<gameplay::StateMachine2D>(id, gameplay::StateMachine2D{
        .rigBlob = assetBank.FindBlob(Assets::kEnemyAnimRig)
    });
    (void)world.Emplace<gameplay::HitboxRig2D>(id, gameplay::HitboxRig2D{
        .hurtbox          = {.offsetX_q8 = 0, .offsetY_q8 = 0, .halfW_q8 = MetersToQ8(halfW), .halfH_q8 = MetersToQ8(halfH)},
        .hitLayerMask     = CollisionLayers::kPlayerBody,
        .hurtLayerMask    = CollisionLayers::kPlayerHitbox,
        .activeSliceIndex = 0xFF
    });
    (void)world.Emplace<gameplay::DamageReceiver2D>(id, gameplay::DamageReceiver2D{
        .hp     = hp,
        .maxHp  = hp,
        .poise  = (kind == EnemyKind::WardenBoss) ? 80u : 25u,
        .teamId = TeamIds::kEnemy
    });
    (void)world.Emplace<world::Sprite2D>(id, world::Sprite2D{
        .tint      = (kind == EnemyKind::WardenBoss)
                       ? renderer::Color{0.95f, 0.30f, 0.45f, 1.0f}
                       : renderer::Color{0.75f, 0.45f, 0.35f, 1.0f},
        .baseDepth = 0.45f,
        .layer     = 1
    });
}

void SpawnRoomTrigger(world::World& world, f32 x, f32 y, const RoomTriggerVolume2D& desc) noexcept {
    auto tRes = world.Spawn({
        .local = {.x = x, .y = y, .scaleX = 1.0f, .scaleY = 1.0f},
        .flags = world::EntityFlags::Spatial
    });
    if (!tRes) return;
    (void)world.Emplace<RoomTransientTag>(*tRes, RoomTransientTag{.roomSalt = 1});
    (void)world.Emplace<RoomTriggerVolume2D>(*tRes, desc);
}

void SpawnMovingPlatform(
    world::World& world,
    f32 startX,
    f32 startY,
    f32 endX,
    f32 endY,
    u32 cycleTicks,
    u16 requiredFlag = 0) noexcept
{
    auto pRes = world.Spawn({
        .local = {.x = startX, .y = startY, .scaleX = 1.0f, .scaleY = 1.0f},
        .flags = static_cast<world::EntityFlags>(
            static_cast<u8>(world::EntityFlags::Spatial) | static_cast<u8>(world::EntityFlags::Teleported))
    });
    if (!pRes) return;
    (void)world.Emplace<RoomTransientTag>(*pRes, RoomTransientTag{.roomSalt = 1});
    (void)world.Emplace<gameplay::KinematicCarrier2D>(*pRes, gameplay::KinematicCarrier2D{
        .bounds = {.offsetX_q8 = 0, .offsetY_q8 = 0, .halfW_q8 = MetersToQ8(2.0f), .halfH_q8 = MetersToQ8(0.35f)}
    });
    (void)world.Emplace<WaypointCarrierDriver2D>(*pRes, WaypointCarrierDriver2D{
        .startX            = startX,
        .startY            = startY,
        .endX              = endX,
        .endY              = endY,
        .cycleTicks        = cycleTicks,
        .requiredWorldFlag = requiredFlag
    });
    (void)world.Emplace<world::Sprite2D>(*pRes, world::Sprite2D{
        .tint      = {0.45f, 0.55f, 0.65f, 1.0f},
        .baseDepth = 0.3f,
        .layer     = 0
    });
}

} // namespace

void PopulateRoomEntities(
    GameSessionState& session,
    world::World& world,
    gameplay::GameplayPipeline& gameplay,
    const assets::AssetBank& assetBank,
    assets::AssetId roomId) noexcept
{
    (void)EnsurePersistentPlayerAndCamera(session, world, assetBank);

    if (roomId.value == Assets::kRoom01Scene.value) {
        // Room 01: Forgotten Ancestral Hall
        SpawnRoomTrigger(world, 6.0f, 3.0f, RoomTriggerVolume2D{
            .halfW = 1.2f, .halfH = 1.5f, .worldFlagId = WorldFlags::kShrine01Activated, .kind = TriggerKind::SaveShrine
        });
        SpawnMovingPlatform(world, 18.0f, 3.5f, 18.0f, 9.5f, 360);
        SpawnRoomEnemy(world, assetBank, EnemyKind::CrawlerHusk, 14.0f, 3.0f, 4.0f);
        SpawnRoomEnemy(world, assetBank, EnemyKind::LancerGuard, 29.0f, 5.0f, 5.5f);
        SpawnRoomEnemy(world, assetBank, EnemyKind::CrawlerHusk, 38.0f, 3.0f, 3.5f);

        // Spike Pit Hazard Volume
        SpawnRoomTrigger(world, 23.0f, 1.0f, RoomTriggerVolume2D{
            .halfW = 3.0f, .halfH = 0.6f, .kind = TriggerKind::SpikeHazard
        });

        // East Exit Portal -> Room 02 (Sunken Reliquary)
        SpawnRoomTrigger(world, 48.5f, 3.5f, RoomTriggerVolume2D{
            .targetSceneId   = Assets::kRoom02Scene,
            .halfW           = 1.0f,
            .halfH           = 2.5f,
            .destSpawnX      = 3.0f,
            .destSpawnY      = 4.0f,
            .kind            = TriggerKind::RoomPortal,
            .destSpawnFacing = 2 // Right (+1)
        });
    } else {
        // Room 02: Sunken Reliquary (Vertical Shaft + Chamber Guardian Mini-Boss)
        // West Entrance Portal -> Room 01 (Ancestral Hall)
        SpawnRoomTrigger(world, 1.2f, 4.0f, RoomTriggerVolume2D{
            .targetSceneId   = Assets::kRoom01Scene,
            .halfW           = 1.0f,
            .halfH           = 2.5f,
            .destSpawnX      = 46.0f,
            .destSpawnY      = 3.5f,
            .kind            = TriggerKind::RoomPortal,
            .destSpawnFacing = 0 // Left (-1)
        });

        SpawnMovingPlatform(world, 12.0f, 4.0f, 22.0f, 11.0f, 480);
        SpawnRoomEnemy(world, assetBank, EnemyKind::LancerGuard, 16.0f, 4.0f, 4.0f);

        if (!gameplay.TestWorldFlag(WorldFlags::kRoom02MiniBossSlain)) {
            SpawnRoomEnemy(
                world, assetBank, EnemyKind::WardenBoss, 36.0f, 4.5f, 8.0f,
                WorldFlags::kRoom02MiniBossSlain);
        }

        // Elevator unlocked after defeating the Chamber Guardian
        SpawnMovingPlatform(world, 46.0f, 4.0f, 46.0f, 16.0f, 360, WorldFlags::kRoom02MiniBossSlain);

        // Relic Pedestal at top of Reliquary shaft
        if (!gameplay.TestWorldFlag(WorldFlags::kUnlockedDoubleJump)) {
            SpawnRoomTrigger(world, 52.0f, 17.0f, RoomTriggerVolume2D{
                .halfW = 1.0f, .halfH = 1.5f, .worldFlagId = WorldFlags::kUnlockedDoubleJump, .kind = TriggerKind::AbilityRelic
            });
        }
    }
}

} // namespace game