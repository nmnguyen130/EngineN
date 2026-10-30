#include "gameplay/StateAndCombatSolver2D.h"

#include "assets/AssetBank.h"
#include "gameplay/GameplayPipeline.h"

namespace en {

void StateAndCombatSolver2D::Step(
    World& world,
    const AssetBank& assetBank,
    const SimStepView& step,
    const TickInput& input,
    ActionBuffer& actions,
    const GameplayPolicyTable& policies,
    GameplayPipeline& pipeline) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();

    // Always decay iframe/stun regardless of policy ownership of damage.
    if ((regMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL) {
        auto& recvPool = world.GetPool<DamageReceiver2D>();
        const auto entities = recvPool.Entities();
        auto recvs = recvPool.Components();
        for (usize i = 0U; i < entities.size(); ++i) {
            const EntityId ent = entities[i];
            if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(ent.index)))) {
                continue;
            }
            DamageReceiver2D& r = recvs[i];
            if (r.iframeTicks > 0U) --r.iframeTicks;
            if (r.stunTicks > 0U && --r.stunTicks == 0U) r.poise = r.maxPoise;
        }
    }

    if ((regMask & (1ULL << StateMachine2D::kTypeId)) == 0ULL) return;

    auto& fsmPool = world.GetPool<StateMachine2D>();
    const auto entities = fsmPool.Entities();
    auto fsms = fsmPool.Components();

    const bool hasAnim   = (regMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL;
    const bool hasHitbox = (regMask & (1ULL << HitboxRig2D::kTypeId)) != 0ULL;
    const bool hasMotion = (regMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL;
    const bool hasRecv   = (regMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL;

    for (usize i = 0U; i < entities.size(); ++i) {
        const EntityId id = entities[i];
        if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index)))) {
            continue;
        }

        StateMachine2D& fsm = fsms[i];
        const u64 entMask = world.GetComponentMaskByIndex(id.index);

        if (fsm.bufferedActionTicks > 0U && --fsm.bufferedActionTicks == 0U) {
            fsm.bufferedActionId = 0xFFU;
        }

        if (fsm.hitstopTicks > 0U) {
            --fsm.hitstopTicks;
            if (hasAnim && (entMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
                world.GetPool<SpriteAnimator2D>().DirectAtEntityIndex(id.index).playing =
                    (fsm.hitstopTicks == 0U);
            }
            continue;
        }

        const CookedStateEntry* activeState = nullptr;
        std::span<const CookedFrameDirective> directives{};

        if (fsm.rigBlob.IsValid()) {
            const std::span<const u8> bytes = assetBank.GetBlob(fsm.rigBlob).Bytes();
            if (bytes.size() >= sizeof(CookedGameplayRigHeader)) {
                const auto* hdr = reinterpret_cast<const CookedGameplayRigHeader*>(bytes.data());
                const usize statesBytes = static_cast<usize>(hdr->stateCount) * sizeof(CookedStateEntry);
                const usize dirsBytes   = static_cast<usize>(hdr->directiveCount) * sizeof(CookedFrameDirective);
                if (hdr->magic == CookedGameplayRigHeader::kMagic &&
                    hdr->version == CookedGameplayRigHeader::kVersion &&
                    bytes.size() >= sizeof(CookedGameplayRigHeader) + statesBytes + dirsBytes &&
                    fsm.stateId < hdr->stateCount) {
                    const auto* states = reinterpret_cast<const CookedStateEntry*>(
                        bytes.data() + sizeof(CookedGameplayRigHeader));
                    const auto* allDirs = reinterpret_cast<const CookedFrameDirective*>(
                        bytes.data() + sizeof(CookedGameplayRigHeader) + statesBytes);
                    activeState = &states[fsm.stateId];
                    const u16 startDir = activeState->firstDirective;
                    if (startDir < hdr->directiveCount) {
                        const u16 count = static_cast<u16>(std::min<u32>(
                            hdr->directiveCount - startDir, activeState->directiveCount));
                        directives = {allDirs + startDir, count};
                    }
                }
            }
        }

        if ((fsm.flags & 1U) != 0U) {
            if (activeState) {
                fsm.cancelMask = activeState->defaultCancels;
                fsm.onHitCancelMask = activeState->onHitCancels;
                if (activeState->animFrameCount > 0U && hasAnim &&
                    (entMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
                    SpriteAnimator2D& anim = world.GetPool<SpriteAnimator2D>().DirectAtEntityIndex(id.index);
                    anim.firstFrame = activeState->animFirstFrame;
                    anim.frameCount = activeState->animFrameCount;
                    anim.ticksPerFrame = std::max<u16>(1U, activeState->animTicksPerFrame);
                    anim.currentFrame = 0U;
                    anim.tickAccum = 0U;
                    anim.loop = (activeState->animLoop != 0U);
                    anim.playing = true;
                }
            }
            fsm.flags &= ~1U;
        }

        const u16 currentFrame = (hasAnim && (entMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL)
            ? world.GetPool<SpriteAnimator2D>().DirectAtEntityIndex(id.index).currentFrame
            : fsm.stateTicks;

        if (!directives.empty() && currentFrame != fsm.lastAnimFrame) {
            fsm.lastAnimFrame = currentFrame;
            const WorldTransform2D& xf = world.GetWorldByIndex(id.index);

            for (const CookedFrameDirective& dir : directives) {
                if (dir.triggerFrame != currentFrame) continue;

                switch (dir.op) {
                    case FrameDirectiveOp::None:
                        break;
                    case FrameDirectiveOp::EmitAudioCue:
                    case FrameDirectiveOp::EmitVfxCue:
                        pipeline.EmitCue({
                            AssetId{static_cast<u64>(dir.assetHashOrParam)},
                            xf.x + dir.boxOrMotion.OffsetX(),
                            xf.y + dir.boxOrMotion.OffsetY(),
                            id, 65535U, 0U,
                            (dir.op == FrameDirectiveOp::EmitAudioCue)
                                ? SimCueKind::AudioOneShot
                                : SimCueKind::SpawnPrefabEffect,
                            dir.auxByte, 0U});
                        break;
                    case FrameDirectiveOp::ActivateHitbox:
                        if (hasHitbox && (entMask & (1ULL << HitboxRig2D::kTypeId)) != 0ULL) {
                            HitboxRig2D& rig = world.GetPool<HitboxRig2D>().DirectAtEntityIndex(id.index);
                            rig.BeginNewSwing(
                                dir.boxOrMotion,
                                static_cast<i16>(dir.assetHashOrParam & 0xFFFFU),
                                static_cast<u8>((dir.assetHashOrParam >> 16U) & 0xFFU),
                                rig.KnockbackX(), rig.KnockbackY(),
                                static_cast<u8>((dir.assetHashOrParam >> 24U) & 0x03U),
                                dir.auxByte);
                        }
                        break;
                    case FrameDirectiveOp::DeactivateHitbox:
                        if (hasHitbox && (entMask & (1ULL << HitboxRig2D::kTypeId)) != 0ULL) {
                            world.GetPool<HitboxRig2D>().DirectAtEntityIndex(id.index).EndSwing();
                        }
                        break;
                    case FrameDirectiveOp::SetCancelMask:
                        fsm.cancelMask = static_cast<CancelCategory>(dir.auxByte);
                        break;
                    case FrameDirectiveOp::AddRootMotion:
                        if (hasMotion && (entMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
                            MotionStack2D& ms = world.GetPool<MotionStack2D>().DirectAtEntityIndex(id.index);
                            ms.AddRootMotionMeters(
                                dir.boxOrMotion.OffsetX() * static_cast<f32>(ms.FacingSign()),
                                dir.boxOrMotion.OffsetY());
                        }
                        break;
                    case FrameDirectiveOp::SetInvulnerable:
                        if (hasRecv && (entMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL) {
                            DamageReceiver2D& r = world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(id.index);
                            r.flags = (dir.auxByte != 0U)
                                ? (r.flags | ReceiverFlags::Invulnerable)
                                : (r.flags & ~ReceiverFlags::Invulnerable);
                        }
                        break;
                }
            }
        }

        if (policies.onStepEntityState) {
            policies.onStepEntityState(id, fsm, world, input, actions, pipeline);
        }

        ++fsm.stateTicks;
        if (activeState && activeState->durationTicks > 0U &&
            fsm.stateTicks >= activeState->durationTicks) {
            const u8 oldState = fsm.stateId;
            const u8 nextState = activeState->nextStateId;
            fsm.TransitionTo(nextState);
            if (policies.onStateTransition) {
                policies.onStateTransition(id, oldState, nextState, world, pipeline);
            }
        }
    }
}

u16 StateAndCombatSolver2D::ResolveHits(
    World& world,
    const SimStepView& step,
    const GameplayPolicyTable& policies,
    GameplayPipeline& pipeline,
    std::span<CombatHitEvent> outEvents) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << HitboxRig2D::kTypeId)) == 0ULL) return 0U;

    auto& rigPool = world.GetPool<HitboxRig2D>();
    if (rigPool.Size() < 2U) return 0U;

    const bool hasMotion = (regMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL;
    const bool hasFsm    = (regMask & (1ULL << StateMachine2D::kTypeId)) != 0ULL;
    const bool hasRecv   = (regMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL;
    const bool hasBody   = (regMask & (1ULL << RigidBody2D::kTypeId)) != 0ULL;

    const auto entities = rigPool.Entities();
    auto rigs = rigPool.Components();
    u16 hitCount = 0U;

    const auto worldBox = [&](EntityId id, const CompactAabb2D& local, bool flip) noexcept -> Rect {
        const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        f32 sign = 1.0f;
        if (flip && hasMotion &&
            (world.GetComponentMaskByIndex(id.index) & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
            sign = static_cast<f32>(world.GetPool<MotionStack2D>().DirectAtEntityIndex(id.index).FacingSign());
        }
        const f32 hw = local.HalfW() * std::abs(xf.scaleX);
        const f32 hh = local.HalfH() * std::abs(xf.scaleY);
        const f32 cx = xf.x + local.OffsetX() * xf.scaleX * sign;
        const f32 cy = xf.y + local.OffsetY() * xf.scaleY;
        return {cx - hw, cy - hh, 2.0f * hw, 2.0f * hh};
    };

    const auto overlap = [](const Rect& a, const Rect& b) noexcept -> bool {
        return a.x <= b.Right() && a.Right() >= b.x && a.y <= b.Bottom() && a.Bottom() >= b.y;
    };

    const auto applyHitstop = [&](EntityId idA, EntityId idB, f32 ptX, f32 ptY, u8 ticks) noexcept {
        if (ticks == 0U) return;
        bool froze = false;
        if (hasFsm) {
            if ((world.GetComponentMaskByIndex(idA.index) & (1ULL << StateMachine2D::kTypeId)) != 0ULL) {
                world.GetPool<StateMachine2D>().DirectAtEntityIndex(idA.index).hitstopTicks = ticks;
                froze = true;
            }
            if ((world.GetComponentMaskByIndex(idB.index) & (1ULL << StateMachine2D::kTypeId)) != 0ULL) {
                world.GetPool<StateMachine2D>().DirectAtEntityIndex(idB.index).hitstopTicks = ticks;
                froze = true;
            }
        }
        pipeline.EmitCue({
            AssetId{}, ptX, ptY, idA, 65535U, ticks,
            SimCueKind::HitstopFreeze,
            world.GetTimeChannelByIndex(idB.index),
            static_cast<u16>(froze ? 1U : 0U)});
    };

    for (usize i = 0U; i < entities.size() && hitCount < outEvents.size(); ++i) {
        HitboxRig2D& attRig = rigs[i];
        if (!HasFlag(attRig.flags, HitboxFlags::HitboxActive)) continue;

        const EntityId attId = entities[i];
        if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(attId.index)))) continue;

        const u64 attMask = world.GetComponentMaskByIndex(attId.index);
        const Rect hitBoxA = worldBox(attId, attRig.hitbox, HasFlag(attRig.flags, HitboxFlags::FlipWithFacing));

        // Clash pass
        if (HasFlag(attRig.flags, HitboxFlags::CanClash)) {
            for (usize j = i + 1U; j < entities.size() && hitCount < outEvents.size(); ++j) {
                HitboxRig2D& rigB = rigs[j];
                if ((rigB.flags & (HitboxFlags::HitboxActive | HitboxFlags::CanClash)) !=
                        (HitboxFlags::HitboxActive | HitboxFlags::CanClash) ||
                    attRig.Priority() != rigB.Priority()) {
                    continue;
                }
                const EntityId idB = entities[j];
                if (attRig.HasAlreadyHit(idB) || rigB.HasAlreadyHit(attId)) continue;
                if ((attRig.hitLayerMask & (1U << rigB.HurtLayer())) == 0U &&
                    (rigB.hitLayerMask & (1U << attRig.HurtLayer())) == 0U) {
                    continue;
                }
                const Rect hitBoxB = worldBox(idB, rigB.hitbox, HasFlag(rigB.flags, HitboxFlags::FlipWithFacing));
                if (!overlap(hitBoxA, hitBoxB)) continue;

                attRig.RecordDefenderHit(idB);
                rigB.RecordDefenderHit(attId);

                CombatHitEvent ev{};
                ev.attacker = attId;
                ev.defender = idB;
                ev.hitPointX = 0.5f * (hitBoxA.Center().x + hitBoxB.Center().x);
                ev.hitPointY = 0.5f * (hitBoxA.Center().y + hitBoxB.Center().y);
                ev.outcome = HitOutcome::Clashed;
                ev.hitstopTicks = std::max(attRig.HitstopTicks(), rigB.HitstopTicks());
                outEvents[hitCount++] = ev;
                applyHitstop(attId, idB, ev.hitPointX, ev.hitPointY, ev.hitstopTicks);
                if (policies.onCombatHit) policies.onCombatHit(ev, world, pipeline);
            }
        }

        // Hurtbox pass
        for (usize j = 0U; j < entities.size() && hitCount < outEvents.size(); ++j) {
            if (i == j) continue;
            HitboxRig2D& defRig = rigs[j];
            if (!HasFlag(defRig.flags, HitboxFlags::HurtboxActive)) continue;

            const EntityId defId = entities[j];
            if (attRig.HasAlreadyHit(defId)) continue;
            if ((attRig.hitLayerMask & (1U << defRig.HurtLayer())) == 0U) continue;
            if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(defId.index)))) continue;

            const u64 defMask = world.GetComponentMaskByIndex(defId.index);
            const Rect hurtBox = worldBox(defId, defRig.hurtbox, HasFlag(defRig.flags, HitboxFlags::FlipWithFacing));
            if (!overlap(hitBoxA, hurtBox)) continue;

            attRig.RecordDefenderHit(defId);

            DamageReceiver2D* defRecv = nullptr;
            if (hasRecv && (defMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL) {
                defRecv = &world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(defId.index);
                if (HasFlag(defRecv->flags, ReceiverFlags::Dead) ||
                    HasFlag(defRecv->flags, ReceiverFlags::Invulnerable) ||
                    defRecv->iframeTicks > 0U) {
                    continue;
                }
                // Same team skip
                if (hasRecv && (attMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL) {
                    const auto& attRecv = world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(attId.index);
                    if (attRecv.teamId == defRecv->teamId && attRecv.teamId != 0U) continue;
                }
            }

            const f32 face = (hasMotion && (attMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL)
                ? static_cast<f32>(world.GetPool<MotionStack2D>().DirectAtEntityIndex(attId.index).FacingSign())
                : 1.0f;

            CombatHitEvent ev{};
            ev.attacker = attId;
            ev.defender = defId;
            ev.hitPointX = 0.5f * (hitBoxA.Center().x + hurtBox.Center().x);
            ev.hitPointY = 0.5f * (hitBoxA.Center().y + hurtBox.Center().y);
            ev.knockbackVx = attRig.KnockbackX() * face;
            ev.knockbackVy = attRig.KnockbackY();
            ev.hitstopTicks = attRig.HitstopTicks();
            ev.attackTag = static_cast<u16>((attRig.SwingId() << 8U) | attId.index);
            const u16 attackTag = ev.attackTag;

            const bool policyDamage =
                HasFlag(attRig.flags, HitboxFlags::PolicyResolvesDamage) && policies.onCombatHit != nullptr;

            if (!policyDamage) {
                if (defRecv && HasFlag(defRecv->flags, ReceiverFlags::Parrying)) {
                    ev.outcome = HitOutcome::Parried;
                    if (hasMotion && (attMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
                        world.GetPool<MotionStack2D>().DirectAtEntityIndex(attId.index)
                            .AddKnockbackImpulse(-ev.knockbackVx * 0.6f, 0.0f);
                    }
                } else if (defRecv && HasFlag(defRecv->flags, ReceiverFlags::SuperArmor) &&
                           defRecv->poise > attRig.poiseDamage) {
                    ev.outcome = HitOutcome::ArmorAbsorbed;
                    ev.damageApplied = attRig.baseDamage;
                    defRecv->lastAttackTag = attackTag;
                    defRecv->poise = static_cast<u8>(defRecv->poise - attRig.poiseDamage);
                    ev.defenderDied = defRecv->ApplyDamage(attRig.baseDamage) ? 1U : 0U;
                } else {
                    ev.outcome = HitOutcome::Damaged;
                    ev.damageApplied = attRig.baseDamage;
                    if (defRecv) {
                        defRecv->lastAttackTag = attackTag;
                        defRecv->iframeTicks = std::max<u16>(defRecv->iframeTicks, defRecv->autoIframeTicks);
                        if (defRecv->poise <= attRig.poiseDamage) {
                            defRecv->poise = defRecv->maxPoise;
                            defRecv->stunTicks = 15U;
                            ev.poiseBroken = 1U;
                        } else {
                            defRecv->poise = static_cast<u8>(defRecv->poise - attRig.poiseDamage);
                        }
                        ev.defenderDied = defRecv->ApplyDamage(attRig.baseDamage) ? 1U : 0U;
                    }
                    if (hasMotion && (defMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
                        world.GetPool<MotionStack2D>().DirectAtEntityIndex(defId.index)
                            .AddKnockbackImpulse(ev.knockbackVx, ev.knockbackVy);
                    } else if (hasBody && (defMask & (1ULL << RigidBody2D::kTypeId)) != 0ULL) {
                        world.GetPool<RigidBody2D>().DirectAtEntityIndex(defId.index)
                            .ApplyLinearImpulse(ev.knockbackVx, ev.knockbackVy);
                    }
                }
            }

            outEvents[hitCount++] = ev;
            applyHitstop(attId, defId, ev.hitPointX, ev.hitPointY, ev.hitstopTicks);
            if (policies.onCombatHit) policies.onCombatHit(ev, world, pipeline);
        }
    }

    return hitCount;
}

} // namespace en
