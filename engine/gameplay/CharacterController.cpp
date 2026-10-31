#include "gameplay/GameplaySystem.h"

namespace en {

void GameplaySystem::BeginTick(const TickInput& input, ActionBuffer& actions, bool playerFrozen) noexcept {
    m_hitCount = 0U;
    actions.Feed(input, playerFrozen);
}

void GameplaySystem::StepStatesAndHealth(
    World& world,
    const TickStep& step,
    const TickInput& input,
    ActionBuffer& actions) noexcept {
    if (!m_cues || !step) return;

    auto& healthPool = world.GetPool<Health2D>();
    for (usize i = 0U; i < healthPool.Size(); ++i) {
        const EntityId id = healthPool.Entities()[i];
        if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index)))) {
            continue;
        }
        Health2D& h = healthPool.ComponentAtDense(i);
        if (h.iframeTicks > 0U) --h.iframeTicks;
        if (h.stunTicks > 0U && --h.stunTicks == 0U) h.poise = h.maxPoise;
    }

    auto&      fsmPool      = world.GetPool<StateMachine2D>();
    const u64  regMask      = world.RegisteredPoolsMask();
    const bool hasAnim      = (regMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL;
    const bool hasTimeline  = (regMask & (1ULL << ActionTimeline2D::kTypeId)) != 0ULL;
    const bool hasCombatBox = (regMask & (1ULL << CombatBox2D::kTypeId)) != 0ULL;

    for (usize i = 0U; i < fsmPool.Size(); ++i) {
        const EntityId id = fsmPool.Entities()[i];
        if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index)))) {
            continue;
        }

        StateMachine2D& fsm = fsmPool.ComponentAtDense(i);
        if (fsm.bufferTicks > 0U && --fsm.bufferTicks == 0U) {
            fsm.bufferedAction = 0xFFU;
        }

        if (fsm.hitstopTicks > 0U) {
            --fsm.hitstopTicks;
            if (hasAnim && world.Has<SpriteAnimator2D>(id)) {
                world.GetPool<SpriteAnimator2D>().DirectAtEntityIndex(id.index).playing =
                    (fsm.hitstopTicks == 0U);
            }
            continue;
        }

        if (hasTimeline && world.Has<ActionTimeline2D>(id)) {
            ActionTimeline2D& tl = world.GetPool<ActionTimeline2D>().DirectAtEntityIndex(id.index);
            if (tl.active != 0U) {
                const u16 t = fsm.stateTicks;
                fsm.cancelMask = tl.InCancelWindow(t) ? tl.cancelMask : 0U;
                if (hasCombatBox && world.Has<CombatBox2D>(id)) {
                    CombatBox2D& cb = world.GetPool<CombatBox2D>().DirectAtEntityIndex(id.index);
                    if (t == tl.startupTicks) {
                        cb.BeginSwing(tl.hitbox, tl.damage, tl.knockback, cb.hitstopTicks, cb.poiseDamage);
                    } else if (t == static_cast<u16>(tl.startupTicks) + tl.activeTicks) {
                        cb.EndSwing();
                    }
                }
                if (t + 1U >= tl.TotalTicks()) {
                    tl.active = 0U;
                }
            }
        }

        fsm.entered = 0U;
        if (m_callbacks.onStateStep) {
            m_callbacks.onStateStep(id, fsm, world, input, actions, *this);
        }

        ++fsm.stateTicks;
        if (fsm.durationTicks > 0U && fsm.stateTicks >= fsm.durationTicks) {
            const u8 from = fsm.state;
            const u8 to   = fsm.nextState;
            fsm.TransitionTo(to);
            if (m_callbacks.onStateTransition) {
                m_callbacks.onStateTransition(id, from, to, world, *this);
            }
        }
    }
}

void GameplaySystem::StepCharactersPrePhysics(World& world, const TickStep& step) noexcept {
    if (!m_cues || !step) return;

    auto&      charPool = world.GetPool<CharacterController2D>();
    auto&      bodyPool = world.GetPool<PhysicsBody2D>();
    const bool hasSS    = (world.RegisteredPoolsMask() & (1ULL << SquashStretch2D::kTypeId)) != 0ULL;

    for (usize i = 0U; i < charPool.Size(); ++i) {
        const EntityId id = charPool.Entities()[i];
        if (!world.Has<PhysicsBody2D>(id)) continue;

        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) continue;
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) continue;

        CharacterController2D& cc   = charPool.ComponentAtDense(i);
        PhysicsBody2D&         body = bodyPool.DirectAtEntityIndex(id.index);

        if (world.Has<StateMachine2D>(id) &&
            world.GetPool<StateMachine2D>().DirectAtEntityIndex(id.index).hitstopTicks > 0U) {
            body.enabled = 0U;
            continue;
        }
        body.enabled = 1U;

        const SquashStretch2D* ss = (hasSS && world.Has<SquashStretch2D>(id))
            ? &world.GetPool<SquashStretch2D>().DirectAtEntityIndex(id.index)
            : nullptr;

        if (cc.oneWayDropTicks > 0U) {
            --cc.oneWayDropTicks;
            body.ignoreOneWay = 1U;
        } else {
            body.ignoreOneWay = 0U;
        }

        if (cc.dashCooldownTicks > 0U) --cc.dashCooldownTicks;

        if (cc.moveInputX < -0.05f) cc.SetFacing(-1);
        else if (cc.moveInputX > 0.05f) cc.SetFacing(1);

        const WorldTransform2D& wt = world.GetWorldByIndex(id.index);

        if (HasFlag(cc.flags, CharFlags::PogoRequested)) {
            cc.flags &= ~CharFlags::PogoRequested;
            body.velocity.y = cc.jumpSpeed * m_cfg.pogoJumpMultiplier;
            cc.RefillAirMobility();
            cc.flags &= ~(CharFlags::JumpCutApplied | CharFlags::WallSliding);
            if (ss) {
                LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
                loc.scaleX            = ss->jumpScale.x;
                loc.scaleY            = ss->jumpScale.y;
            }
        }

        if (HasFlag(cc.flags, CharFlags::DashRequested)) {
            cc.flags &= ~CharFlags::DashRequested;
            const bool canDash = (cc.dashCooldownTicks == 0U) &&
                                 (body.IsGrounded() || cc.airDashesLeft > 0U);
            if (canDash) {
                if (!body.IsGrounded() && cc.airDashesLeft > 0U) --cc.airDashesLeft;
                cc.dashTicks         = m_cfg.dashDurationTicks;
                cc.dashCooldownTicks = m_cfg.dashCooldownTicks;
                cc.flags            |= CharFlags::Dashing;
                if (ss) {
                    LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
                    loc.scaleX            = ss->dashScale.x;
                    loc.scaleY            = ss->dashScale.y;
                    if (ss->emitDust != 0U) {
                        EmitCue({
                            .position      = {wt.x, wt.y},
                            .emitter       = id,
                            .magnitude     = 1.0f,
                            .durationTicks = 12U,
                            .userTag       = 6U,
                            .kind          = CueKind::Effect,
                            .channel       = ch,
                        });
                    }
                }
            }
        }

        if (cc.dashTicks > 0U) {
            if (ss && ss->ghostIntervalTicks > 0U && (cc.dashTicks % ss->ghostIntervalTicks) == 0U) {
                EmitCue({
                    .position      = {wt.x, wt.y},
                    .emitter       = id,
                    .durationTicks = ss->ghostLifetimeTicks,
                    .userTag       = static_cast<u16>((cc.facing < 0) ? 1U : 0U),
                    .kind          = CueKind::DashGhost,
                    .channel       = ch,
                });
            }

            --cc.dashTicks;
            body.gravityScale = 0.0f;
            body.velocity.x   = static_cast<f32>(cc.facing) * cc.dashSpeed;
            body.velocity.y   = 0.0f;
            if (cc.dashTicks == 0U) {
                body.gravityScale = 1.0f;
                cc.flags         &= ~CharFlags::Dashing;
                cc.currentWalkVx  = static_cast<f32>(cc.facing) * cc.maxRunSpeed;
            }
            continue;
        }

        if (HasFlag(cc.flags, CharFlags::JumpRequested)) {
            cc.flags &= ~CharFlags::JumpRequested;
            bool jumped = false;
            if (cc.coyoteTicks > 0U) {
                cc.coyoteTicks  = 0U;
                body.velocity.y = cc.jumpSpeed;
                cc.flags       &= ~CharFlags::JumpCutApplied;
                jumped          = true;
            } else if (cc.wallCoyoteTicks > 0U && cc.wallDir != 0) {
                cc.wallCoyoteTicks = 0U;
                cc.currentWalkVx   = static_cast<f32>(-cc.wallDir) * cc.wallJumpSpeedX;
                body.velocity.x    = cc.currentWalkVx;
                body.velocity.y    = cc.jumpSpeed;
                cc.SetFacing(static_cast<i8>(-cc.wallDir));
                cc.flags &= ~(CharFlags::JumpCutApplied | CharFlags::WallSliding);
                jumped    = true;
            } else if (cc.airJumpsLeft > 0U) {
                --cc.airJumpsLeft;
                body.velocity.y = cc.jumpSpeed;
                cc.flags       &= ~CharFlags::JumpCutApplied;
                jumped          = true;
            }
            if (jumped && ss) {
                LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
                loc.scaleX            = ss->jumpScale.x;
                loc.scaleY            = ss->jumpScale.y;
                if (ss->emitDust != 0U) {
                    EmitCue({
                        .position      = {wt.x, wt.y - 0.4f},
                        .emitter       = id,
                        .magnitude     = 0.8f,
                        .durationTicks = 14U,
                        .userTag       = 6U,
                        .kind          = CueKind::Effect,
                        .channel       = ch,
                    });
                }
            }
        }

        if (!body.IsGrounded() && body.velocity.y > 0.0f &&
            !HasFlag(cc.flags, CharFlags::JumpHeld) &&
            !HasFlag(cc.flags, CharFlags::JumpCutApplied)) {
            body.velocity.y *= m_cfg.jumpCutMultiplier;
            cc.flags        |= CharFlags::JumpCutApplied;
        }

        // Data-driven Apex Hang Time: reduce gravity and boost air control when airborne near jump apex.
        const bool atApex = !body.IsGrounded() &&
                            HasFlag(cc.flags, CharFlags::JumpHeld) &&
                            std::abs(body.velocity.y) <= m_cfg.apexSpeedThreshold;
        body.gravityScale = atApex ? m_cfg.apexGravityScale : 1.0f;

        const f32 targetVx = std::clamp(cc.moveInputX, -1.0f, 1.0f) * cc.maxRunSpeed;
        const f32 accel    = body.IsGrounded()
            ? m_cfg.groundAccel
            : (atApex ? (m_cfg.airAccel * m_cfg.apexAirAccelScale) : m_cfg.airAccel);
        cc.currentWalkVx   = Approach(cc.currentWalkVx, targetVx, accel * dt);

        if (cc.knockbackVel.y != 0.0f) {
            body.velocity.y   = cc.knockbackVel.y;
            cc.knockbackVel.y = 0.0f;
        }
        body.velocity.x = cc.currentWalkVx + cc.knockbackVel.x;

        cc.knockbackVel.x *= 1.0f / (1.0f + m_cfg.knockbackDecay * dt);
        if (std::abs(cc.knockbackVel.x) < 0.02f) cc.knockbackVel.x = 0.0f;

        if (HasFlag(cc.flags, CharFlags::WallSliding) && body.velocity.y < -cc.wallSlideSpeed) {
            body.velocity.y = -cc.wallSlideSpeed;
        }
    }
}

void GameplaySystem::SyncCharactersPostPhysics(
    World& world,
    const PhysicsSystem& physics,
    const TickStep& step) noexcept {
    if (!m_cues || !step) return;

    auto&      charPool = world.GetPool<CharacterController2D>();
    auto&      bodyPool = world.GetPool<PhysicsBody2D>();
    const bool hasSS    = (world.RegisteredPoolsMask() & (1ULL << SquashStretch2D::kTypeId)) != 0ULL;

    for (usize i = 0U; i < charPool.Size(); ++i) {
        const EntityId id = charPool.Entities()[i];
        if (!world.Has<PhysicsBody2D>(id)) continue;
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) continue;

        CharacterController2D& cc   = charPool.ComponentAtDense(i);
        const PhysicsBody2D&   body = bodyPool.DirectAtEntityIndex(id.index);

        const i8 wDir = body.OnWallLeft()
            ? static_cast<i8>(-1)
            : (body.OnWallRight() ? static_cast<i8>(1) : static_cast<i8>(0));

        if (wDir != 0) {
            if (cc.currentWalkVx * static_cast<f32>(wDir) > 0.0f) cc.currentWalkVx = 0.0f;
            cc.wallCoyoteTicks = m_cfg.coyoteWindowTicks;
            cc.wallDir         = wDir;
            const bool pushingWall = (cc.moveInputX * static_cast<f32>(wDir)) > 0.1f;
            if (!body.IsGrounded() && body.velocity.y <= 0.0f && pushingWall) {
                cc.flags |= CharFlags::WallSliding;
            } else {
                cc.flags &= ~CharFlags::WallSliding;
            }
        } else {
            cc.flags &= ~CharFlags::WallSliding;
            if (cc.wallCoyoteTicks > 0U) --cc.wallCoyoteTicks;
            else cc.wallDir = 0;
        }

        if (body.IsGrounded()) {
            const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
            if (cc.coyoteTicks < m_cfg.coyoteWindowTicks && hasSS && world.Has<SquashStretch2D>(id)) {
                const SquashStretch2D& ss = world.GetPool<SquashStretch2D>().DirectAtEntityIndex(id.index);
                LocalTransform2D& loc     = world.ModifyLocalTransformByIndex(id.index);
                loc.scaleX                = ss.landScale.x;
                loc.scaleY                = ss.landScale.y;
                if (ss.emitDust != 0U) {
                    EmitCue({
                        .position      = {wt.x, wt.y - 0.45f},
                        .emitter       = id,
                        .magnitude     = 0.75f,
                        .durationTicks = 12U,
                        .userTag       = 6U,
                        .kind          = CueKind::Effect,
                        .channel       = ch,
                    });
                }
            }

            cc.coyoteTicks   = m_cfg.coyoteWindowTicks;
            cc.airDashesLeft = cc.maxAirDashes;
            cc.airJumpsLeft  = cc.maxAirJumps;
            cc.flags        &= ~(CharFlags::JumpCutApplied | CharFlags::WallSliding);

            if (!body.OnOneWay() && !body.carrier.IsValid() &&
                world.Has<SafeAnchor2D>(id) && world.Has<Collider2D>(id)) {
                const Collider2D& col = world.GetPool<Collider2D>().DirectAtEntityIndex(id.index);
                const Rect        box = col.WorldBounds({wt.x, wt.y});
                const Vec2        lPt{box.x + box.w * 0.15f, box.y + 0.05f};
                const Vec2        rPt{box.Right() - box.w * 0.15f, box.y + 0.05f};
                if (physics.Raycast(lPt, {0.0f, -1.0f}, 0.25f, col.queryMask, id) &&
                    physics.Raycast(rPt, {0.0f, -1.0f}, 0.25f, col.queryMask, id)) {
                    SafeAnchor2D& anchor = world.GetPool<SafeAnchor2D>().DirectAtEntityIndex(id.index);
                    const Vec2    curPos{wt.x, wt.y};
                    if ((curPos - anchor.safePos).LengthSq() >= 0.25f) {
                        anchor.prevSafePos = anchor.safePos;
                        anchor.safePos     = curPos;
                    }
                }
            }
        } else if (cc.coyoteTicks > 0U) {
            --cc.coyoteTicks;
        }
    }
}

} // namespace en
