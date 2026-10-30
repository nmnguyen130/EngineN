#include "gameplay/GameplaySystem.h"

namespace en {

Result<void, GameplayErrc> GameplaySystem::Init(
    World& world,
    const GameplayConfig& cfg,
    const GameplayCallbacks& callbacks) noexcept {
    Shutdown();
    if (cfg.maxCuesPerTick == 0U || cfg.maxCombatHitsPerTick == 0U || cfg.arenaBytes < 8192U) {
        return Err(GameplayErrc::InvalidConfig);
    }

    if (!world.RegisterComponentPool<CharacterController2D>().has_value() ||
        !world.RegisterComponentPool<StateMachine2D>().has_value() ||
        !world.RegisterComponentPool<Health2D>().has_value() ||
        !world.RegisterComponentPool<CombatBox2D>().has_value() ||
        !world.RegisterComponentPool<SafeAnchor2D>(256U).has_value() ||
        !world.RegisterComponentPool<CameraFollow2D>(32U).has_value()) {
        return Err(GameplayErrc::PoolRegistrationFailed);
    }

    if (!m_arena.Init(cfg.arenaBytes)) {
        return Err(GameplayErrc::OutOfMemory);
    }

    auto cues = m_arena.Alloc<GameplayCue>(cfg.maxCuesPerTick);
    auto hits = m_arena.Alloc<CombatHitEvent>(cfg.maxCombatHitsPerTick);
    if (cues.empty() || hits.empty()) {
        Shutdown();
        return Err(GameplayErrc::OutOfMemory);
    }

    m_cfg       = cfg;
    m_callbacks = callbacks;
    m_cues      = cues.data();
    m_hits      = hits.data();
    return {};
}

void GameplaySystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_cues      = nullptr;
    m_hits      = nullptr;
    m_tickCount = 0ULL;
    m_totalHits = 0U;
    Clear();
}

void GameplaySystem::Clear() noexcept {
    ClearWorldFlags();
    m_cueCount = 0U;
    m_hitCount = 0U;
}

void GameplaySystem::Step(
    World& world,
    PhysicsSystem& physics,
    const TickStep& step,
    const TickInput& input,
    ActionBuffer& actions) noexcept {
    if (!m_cues || !step) return;

    m_hitCount = 0U;
    actions.Feed(input, step.IsFrozen(TimeChannel::Player));

    StepStatesAndHealth(world, step, input, actions);
    StepCharactersPrePhysics(world, step);
    physics.Step(world, step);
    SyncCharactersPostPhysics(world, physics, step);
    ResolveCombat(world, step);
    StepCameras(world, step);

    ++m_tickCount;
}

void GameplaySystem::StepStatesAndHealth(
    World& world,
    const TickStep& step,
    const TickInput& input,
    ActionBuffer& actions) noexcept {
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

    auto& fsmPool      = world.GetPool<StateMachine2D>();
    const bool hasAnim = (world.RegisteredPoolsMask() & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL;

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
    auto& charPool = world.GetPool<CharacterController2D>();
    auto& bodyPool = world.GetPool<PhysicsBody2D>();

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

        if (cc.oneWayDropTicks > 0U) {
            --cc.oneWayDropTicks;
            body.ignoreOneWay = 1U;
        } else {
            body.ignoreOneWay = 0U;
        }

        if (cc.dashCooldownTicks > 0U) --cc.dashCooldownTicks;

        if (cc.moveInputX < -0.05f) cc.SetFacing(-1);
        else if (cc.moveInputX > 0.05f) cc.SetFacing(1);

        if (HasFlag(cc.flags, CharFlags::DashRequested)) {
            cc.flags &= ~CharFlags::DashRequested;
            const bool canDash = (cc.dashCooldownTicks == 0U) &&
                                 (body.IsGrounded() || cc.airDashesLeft > 0U);
            if (canDash) {
                if (!body.IsGrounded() && cc.airDashesLeft > 0U) --cc.airDashesLeft;
                cc.dashTicks         = m_cfg.dashDurationTicks;
                cc.dashCooldownTicks = m_cfg.dashCooldownTicks;
                cc.flags            |= CharFlags::Dashing;
            }
        }

        if (cc.dashTicks > 0U) {
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
        body.gravityScale = 1.0f;

        if (HasFlag(cc.flags, CharFlags::JumpRequested)) {
            cc.flags &= ~CharFlags::JumpRequested;
            if (cc.coyoteTicks > 0U) {
                cc.coyoteTicks  = 0U;
                body.velocity.y = cc.jumpSpeed;
                cc.flags       &= ~CharFlags::JumpCutApplied;
            } else if (cc.wallCoyoteTicks > 0U && cc.wallDir != 0) {
                cc.wallCoyoteTicks = 0U;
                cc.currentWalkVx   = static_cast<f32>(-cc.wallDir) * cc.wallJumpSpeedX;
                body.velocity.x    = cc.currentWalkVx;
                body.velocity.y    = cc.jumpSpeed;
                cc.SetFacing(static_cast<i8>(-cc.wallDir));
                cc.flags &= ~(CharFlags::JumpCutApplied | CharFlags::WallSliding);
            } else if (cc.airJumpsLeft > 0U) {
                --cc.airJumpsLeft;
                body.velocity.y = cc.jumpSpeed;
                cc.flags       &= ~CharFlags::JumpCutApplied;
            }
        }

        if (!body.IsGrounded() && body.velocity.y > 0.0f &&
            !HasFlag(cc.flags, CharFlags::JumpHeld) &&
            !HasFlag(cc.flags, CharFlags::JumpCutApplied)) {
            body.velocity.y *= m_cfg.jumpCutMultiplier;
            cc.flags        |= CharFlags::JumpCutApplied;
        }

        const f32 targetVx = std::clamp(cc.moveInputX, -1.0f, 1.0f) * cc.maxRunSpeed;
        const f32 accel    = body.IsGrounded() ? m_cfg.groundAccel : m_cfg.airAccel;
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
    auto& charPool = world.GetPool<CharacterController2D>();
    auto& bodyPool = world.GetPool<PhysicsBody2D>();

    for (usize i = 0U; i < charPool.Size(); ++i) {
        const EntityId id = charPool.Entities()[i];
        if (!world.Has<PhysicsBody2D>(id)) continue;
        if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index)))) {
            continue;
        }

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
            cc.coyoteTicks   = m_cfg.coyoteWindowTicks;
            cc.airDashesLeft = cc.maxAirDashes;
            cc.airJumpsLeft  = cc.maxAirJumps;
            cc.flags        &= ~(CharFlags::JumpCutApplied | CharFlags::WallSliding);

            if (!body.OnOneWay() && !body.carrier.IsValid() &&
                world.Has<SafeAnchor2D>(id) && world.Has<Collider2D>(id)) {
                const WorldTransform2D& wt  = world.GetWorldByIndex(id.index);
                const Collider2D&       col = world.GetPool<Collider2D>().DirectAtEntityIndex(id.index);
                const Rect              box = col.WorldBounds({wt.x, wt.y}, {wt.scaleX, wt.scaleY});
                const Vec2              lPt{box.x + box.w * 0.15f, box.y + 0.05f};
                const Vec2              rPt{box.Right() - box.w * 0.15f, box.y + 0.05f};
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

void GameplaySystem::ResolveCombat(World& world, const TickStep& step) noexcept {
    auto& combatPool = world.GetPool<CombatBox2D>();
    if (combatPool.Size() < 2U) return;

    const auto entities = combatPool.Entities();
    auto       boxes    = combatPool.Components();

    const auto getFacing = [&](EntityId id) noexcept -> i8 {
        return world.Has<CharacterController2D>(id)
            ? world.GetPool<CharacterController2D>().DirectAtEntityIndex(id.index).facing
            : static_cast<i8>(1);
    };

    const auto applyHitstop = [&](EntityId a, EntityId b, Vec2 pt, u8 ticks) noexcept {
        if (ticks == 0U) return;
        if (world.Has<StateMachine2D>(a)) {
            world.GetPool<StateMachine2D>().DirectAtEntityIndex(a.index).hitstopTicks = ticks;
        }
        if (world.Has<StateMachine2D>(b)) {
            world.GetPool<StateMachine2D>().DirectAtEntityIndex(b.index).hitstopTicks = ticks;
        }
        EmitCue({
            .position      = pt,
            .emitter       = a,
            .durationTicks = ticks,
            .kind          = CueKind::Hitstop,
            .channel       = static_cast<TimeChannel>(world.GetTimeChannelByIndex(b.index)),
        });
    };

    for (usize i = 0U; i < entities.size() && m_hitCount < m_cfg.maxCombatHitsPerTick; ++i) {
        CombatBox2D& attBox = boxes[i];
        if (!HasFlag(attBox.flags, CombatFlags::HitboxActive)) continue;

        const EntityId attId = entities[i];
        if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(attId.index)))) continue;

        const WorldTransform2D& attWt    = world.GetWorldByIndex(attId.index);
        const i8                attFace  = getFacing(attId);
        const Rect              hitRect  = attBox.WorldHitbox({attWt.x, attWt.y}, attFace);
        const u16               swingTag =
            static_cast<u16>((attBox.swingId << 8U) ^ (attId.index & 0xFFU));

        for (usize j = 0U; j < entities.size() && m_hitCount < m_cfg.maxCombatHitsPerTick; ++j) {
            if (i == j) continue;
            const EntityId defId = entities[j];
            if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(defId.index)))) continue;

            CombatBox2D&            defBox = boxes[j];
            const WorldTransform2D& defWt  = world.GetWorldByIndex(defId.index);

            if (j > i && HasFlag(attBox.flags, CombatFlags::CanClash) &&
                HasFlag(defBox.flags, CombatFlags::HitboxActive) &&
                HasFlag(defBox.flags, CombatFlags::CanClash)) {
                const Rect defHitRect = defBox.WorldHitbox({defWt.x, defWt.y}, getFacing(defId));
                if (hitRect.Overlaps(defHitRect)) {
                    attBox.EndSwing();
                    defBox.EndSwing();
                    const Vec2 mid = hitRect.Intersection(defHitRect).Center();
                    const u8   hs  = std::max(attBox.hitstopTicks, defBox.hitstopTicks);
                    CombatHitEvent ev{
                        .attacker     = attId,
                        .defender     = defId,
                        .point        = mid,
                        .swingTag     = swingTag,
                        .outcome      = HitOutcome::Clashed,
                        .hitstopTicks = hs,
                    };
                    m_hits[m_hitCount++] = ev;
                    ++m_totalHits;
                    applyHitstop(attId, defId, mid, hs);
                    if (m_callbacks.onCombatHit) m_callbacks.onCombatHit(ev, world, *this);
                    break;
                }
            }

            if (!HasFlag(defBox.flags, CombatFlags::HurtboxActive)) continue;

            Health2D* defHealth = world.TryGet<Health2D>(defId);
            if (defHealth) {
                if (!defHealth->IsAlive() || defHealth->IsInvulnerable()) continue;
                if (defHealth->lastSwingTag == swingTag && swingTag != 0U) continue;
                if ((attBox.targetTeamMask & (1U << (defHealth->team & 7U))) == 0U) continue;
                if (const Health2D* attHealth = world.TryGet<Health2D>(attId)) {
                    if (attHealth->team != 0U && attHealth->team == defHealth->team) continue;
                }
            }

            const Rect hurtRect = defBox.WorldHurtbox({defWt.x, defWt.y}, getFacing(defId));
            if (!hitRect.Overlaps(hurtRect)) continue;

            const Vec2 kb{attBox.knockback.x * static_cast<f32>(attFace), attBox.knockback.y};
            CombatHitEvent ev{
                .attacker     = attId,
                .defender     = defId,
                .point        = hitRect.Intersection(hurtRect).Center(),
                .knockback    = kb,
                .swingTag     = swingTag,
                .hitstopTicks = attBox.hitstopTicks,
            };

            if (defHealth) {
                defHealth->lastSwingTag = swingTag;
                if (HasFlag(defHealth->flags, HealthFlags::Parrying)) {
                    ev.outcome = HitOutcome::Parried;
                    if (auto* attCc = world.TryGet<CharacterController2D>(attId)) {
                        attCc->ApplyKnockback({-kb.x * 0.6f, 0.0f});
                    }
                } else if (HasFlag(defHealth->flags, HealthFlags::SuperArmor) &&
                           defHealth->poise > attBox.poiseDamage) {
                    ev.outcome       = HitOutcome::ArmorAbsorbed;
                    ev.damage        = attBox.damage;
                    defHealth->poise = static_cast<u8>(defHealth->poise - attBox.poiseDamage);
                    ev.killed        = defHealth->ApplyDamage(attBox.damage) ? 1U : 0U;
                } else {
                    ev.outcome             = HitOutcome::Damaged;
                    ev.damage              = attBox.damage;
                    defHealth->iframeTicks = std::max(defHealth->iframeTicks, defHealth->autoIframeTicks);
                    if (defHealth->poise <= attBox.poiseDamage) {
                        defHealth->poise     = defHealth->maxPoise;
                        defHealth->stunTicks = m_cfg.defaultStunTicks;
                        ev.poiseBroken       = 1U;
                    } else {
                        defHealth->poise = static_cast<u8>(defHealth->poise - attBox.poiseDamage);
                    }
                    ev.killed = defHealth->ApplyDamage(attBox.damage) ? 1U : 0U;
                }
            } else {
                ev.outcome = HitOutcome::Damaged;
                ev.damage  = attBox.damage;
            }

            if (ev.outcome == HitOutcome::Damaged) {
                if (auto* defCc = world.TryGet<CharacterController2D>(defId)) {
                    defCc->ApplyKnockback(kb);
                } else if (auto* defBody = world.TryGet<PhysicsBody2D>(defId)) {
                    defBody->ApplyImpulse(kb);
                }
            }

            m_hits[m_hitCount++] = ev;
            ++m_totalHits;
            applyHitstop(attId, defId, ev.point, ev.hitstopTicks);
            if (m_callbacks.onCombatHit) m_callbacks.onCombatHit(ev, world, *this);
        }
    }
}

void GameplaySystem::StepCameras(World& world, const TickStep& step) noexcept {
    auto& camPool = world.GetPool<CameraFollow2D>();
    if (camPool.Size() == 0U) return;

    bool anyUpdated = false;
    for (usize i = 0U; i < camPool.Size(); ++i) {
        const EntityId camId = camPool.Entities()[i];
        CameraFollow2D& cam  = camPool.ComponentAtDense(i);
        if (!world.IsAlive(cam.target)) continue;

        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(camId.index));
        if (step.IsFrozen(ch)) continue;
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) continue;

        const WorldTransform2D& targetWt = world.GetWorldByIndex(cam.target.index);
        Vec2 focus{targetWt.x, targetWt.y};

        if (HasFlag(cam.flags, CameraFlags::LatchGroundY)) {
            if (const auto* body = world.TryGet<PhysicsBody2D>(cam.target)) {
                if (body->IsGrounded()) cam.groundLatchY = targetWt.y;
                if (std::abs(targetWt.y - cam.groundLatchY) <= 2.5f) {
                    focus.y = cam.groundLatchY;
                }
            }
        }

        if (HasFlag(cam.flags, CameraFlags::EnableLookahead)) {
            if (const auto* cc = world.TryGet<CharacterController2D>(cam.target)) {
                const f32 targetLook = static_cast<f32>(cc->facing) * cam.lookaheadDist;
                cam.lookaheadX       = Lerp(cam.lookaheadX, targetLook, std::min(1.0f, dt * 6.0f));
                focus.x             += cam.lookaheadX;
            }
        }

        if (HasFlag(cam.flags, CameraFlags::SnapNextStep)) {
            cam.anchor   = focus;
            cam.velocity = {};
            cam.flags   &= ~CameraFlags::SnapNextStep;
        } else {
            const f32 targetX = std::clamp(cam.anchor.x, focus.x - cam.deadzone.x, focus.x + cam.deadzone.x);
            const f32 targetY = std::clamp(cam.anchor.y, focus.y - cam.deadzone.y, focus.y + cam.deadzone.y);
            const f32 omega   = static_cast<f32>(cam.followOmega);
            StepSpring(cam.anchor.x, cam.velocity.x, targetX, omega, dt);
            StepSpring(cam.anchor.y, cam.velocity.y, targetY, omega * 1.2f, dt);
        }

        CameraRig2D* rig = world.TryGet<CameraRig2D>(camId);
        if (HasFlag(cam.flags, CameraFlags::ConfineToRoom) && !cam.roomBounds.IsEmpty()) {
            f32 halfW = 0.0f;
            f32 halfH = 0.0f;
            if (rig) {
                const f32 invZoom = (rig->zoom > 1e-4f) ? (1.0f / rig->zoom) : 1.0f;
                const f32 invPpm  = (rig->pixelsPerMeter > 1e-4f) ? (1.0f / rig->pixelsPerMeter) : (1.0f / 48.0f);
                halfW = 640.0f * rig->viewport.w * invPpm * invZoom;
                halfH = 360.0f * rig->viewport.h * invPpm * invZoom;
            }
            const f32 minX = cam.roomBounds.x + halfW;
            const f32 maxX = cam.roomBounds.Right() - halfW;
            const f32 minY = cam.roomBounds.y + halfH;
            const f32 maxY = cam.roomBounds.Bottom() - halfH;
            cam.anchor.x = (minX <= maxX) ? std::clamp(cam.anchor.x, minX, maxX) : cam.roomBounds.Center().x;
            cam.anchor.y = (minY <= maxY) ? std::clamp(cam.anchor.y, minY, maxY) : cam.roomBounds.Center().y;
        }

        if (cam.trauma > 0.0f) {
            cam.trauma = std::max(0.0f, cam.trauma - 1.5f * dt);
        }
        if (rig) rig->trauma = cam.trauma;

        LocalTransform2D& loc = world.ModifyLocalTransformByIndex(camId.index);
        loc.x = cam.anchor.x;
        loc.y = cam.anchor.y;
        anyUpdated = true;
    }

    if (anyUpdated) world.PropagateTransforms();
}

u64 GameplaySystem::ComputeAuditHash(const World& world) const noexcept {
    u64 h = HashCombine(0xCBF29CE484222325ULL, m_tickCount);
    h     = HashCombine(h, m_totalHits);
    for (const u64 w : m_worldFlags) {
        if (w != 0ULL) h = HashCombine(h, w);
    }

    const u64 reg = world.RegisteredPoolsMask();
    const auto mixPool = [&]<Component T>() noexcept {
        if ((reg & (1ULL << T::kTypeId)) != 0ULL) {
            const auto bytes = std::as_bytes(world.GetPool<T>().Components());
            h = HashCombine(h, Crc32({reinterpret_cast<const u8*>(bytes.data()), bytes.size()}));
        }
    };

    mixPool.template operator()<CharacterController2D>();
    mixPool.template operator()<StateMachine2D>();
    mixPool.template operator()<Health2D>();
    mixPool.template operator()<CombatBox2D>();
    mixPool.template operator()<SafeAnchor2D>();
    mixPool.template operator()<CameraFollow2D>();
    return h;
}

} // namespace en
