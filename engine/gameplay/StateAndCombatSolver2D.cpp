#include "gameplay/StateAndCombatSolver2D.h"
#include "assets/AssetBank.h"
#include "gameplay/GameplayPipeline.h"

#include <cstring>

namespace en {

void StateAndCombatSolver2D::StepStateMachinesAndDirectives(
    World& world,
    const AssetBank& assetBank,
    const SimStepView& step,
    const TickInput& input,
    ActionBuffer& actions,
    const GameplayPolicyTable& policies,
    GameplayPipeline& pipeline) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();

    // 1. Step DamageReceiver2D invulnerability and stun timers
    if ((regMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL) {
        auto& recvPool = world.GetPool<DamageReceiver2D>();
        const auto entities = recvPool.Entities();
        auto recvs = recvPool.Components();
        for (usize i = 0U; i < entities.size(); ++i) {
            const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(entities[i].index));
            if (step.IsFrozen(ch)) continue;
            DamageReceiver2D& r = recvs[i];
            if (r.iframeTicks > 0U) --r.iframeTicks;
            if (r.stunTicks > 0U) {
                if (--r.stunTicks == 0U) {
                    r.poise = r.maxPoise;
                }
            }
        }
    }

    if ((regMask & (1ULL << StateMachine2D::kTypeId)) == 0ULL) return;
    auto& fsmPool = world.GetPool<StateMachine2D>();
    const auto entities = fsmPool.Entities();
    auto fsms = fsmPool.Components();

    const bool hasAnim = (regMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL;
    const bool hasHitbox = (regMask & (1ULL << HitboxRig2D::kTypeId)) != 0ULL;
    const bool hasMotion = (regMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL;
    const bool hasRecv = (regMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL;

    for (usize i = 0U; i < entities.size(); ++i) {
        const EntityId id = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) continue;

        StateMachine2D& fsm = fsms[i];
        const u64 entMask = world.GetComponentMaskByIndex(id.index);

        // Decrement buffered action window even during hitstop so players can buffer reversals during freeze frames
        if (fsm.bufferedActionTicks > 0U) {
            if (--fsm.bufferedActionTicks == 0U) {
                fsm.bufferedActionId = 0xFFU;
            }
        }

        // Enforce per-entity combat hitstop freeze (pauses FSM and SpriteAnimator2D)
        if (fsm.hitstopTicks > 0U) {
            --fsm.hitstopTicks;
            if (hasAnim && (entMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
                world.GetPool<SpriteAnimator2D>().DirectAtEntityIndex(id.index).playing = (fsm.hitstopTicks == 0U);
            }
            continue;
        }

        CookedStateEntry activeStateStorage{};
        const CookedStateEntry* activeStateEntry = nullptr;
        const u8* directiveBytes = nullptr;
        u16 totalDirectives = 0U;

        if (fsm.rigBlob.IsValid()) {
            const BlobRecord& blob = assetBank.GetBlob(fsm.rigBlob);
            const std::span<const u8> bytes = blob.Bytes();
            if (bytes.size() >= sizeof(CookedGameplayRigHeader)) {
                CookedGameplayRigHeader hdr{};
                std::memcpy(&hdr, bytes.data(), sizeof(CookedGameplayRigHeader));
                const usize statesBytes = static_cast<usize>(hdr.stateCount) * sizeof(CookedStateEntry);
                const usize dirsBytes = static_cast<usize>(hdr.directiveCount) * sizeof(CookedFrameDirective);
                if (hdr.magic == CookedGameplayRigHeader::kMagic &&
                    hdr.version == CookedGameplayRigHeader::kVersion &&
                    bytes.size() >= sizeof(CookedGameplayRigHeader) + statesBytes + dirsBytes &&
                    fsm.stateId < hdr.stateCount) {
                    std::memcpy(
                        &activeStateStorage,
                        bytes.data() + sizeof(CookedGameplayRigHeader) + static_cast<usize>(fsm.stateId) * sizeof(CookedStateEntry),
                        sizeof(CookedStateEntry));
                    activeStateEntry = &activeStateStorage;
                    directiveBytes = bytes.data() + sizeof(CookedGameplayRigHeader) + statesBytes;
                    totalDirectives = hdr.directiveCount;
                }
            }
        }

        // On state entry: apply initial cancel masks and automatically sync SpriteAnimator2D clip parameters
        if ((fsm.flags & 1U) != 0U) {
            if (activeStateEntry) {
                fsm.cancelMask = activeStateEntry->defaultCancels;
                fsm.onHitCancelMask = activeStateEntry->onHitCancels;
                if (activeStateEntry->animFrameCount > 0U && hasAnim &&
                    (entMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
                    SpriteAnimator2D& anim = world.GetPool<SpriteAnimator2D>().DirectAtEntityIndex(id.index);
                    anim.firstFrame = activeStateEntry->animFirstFrame;
                    anim.frameCount = activeStateEntry->animFrameCount;
                    anim.ticksPerFrame = std::max<u16>(1U, activeStateEntry->animTicksPerFrame);
                    anim.currentFrame = 0U;
                    anim.tickAccum = 0U;
                    anim.loop = (activeStateEntry->animLoop != 0U);
                    anim.playing = true;
                }
            }
            fsm.flags &= ~1U;
        }

        u16 currentFrame = fsm.stateTicks;
        if (hasAnim && (entMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
            currentFrame = world.GetPool<SpriteAnimator2D>().DirectAtEntityIndex(id.index).currentFrame;
        }

        // Fire frame-accurate timeline directives once per frame advance
        if (activeStateEntry && directiveBytes && currentFrame != fsm.lastAnimFrame) {
            fsm.lastAnimFrame = currentFrame;
            const u16 startDir = activeStateEntry->firstDirective;
            const u16 endDir = static_cast<u16>(
                std::min<u32>(totalDirectives, static_cast<u32>(startDir) + activeStateEntry->directiveCount));

            for (u16 d = startDir; d < endDir; ++d) {
                CookedFrameDirective dir{};
                std::memcpy(
                    &dir,
                    directiveBytes + static_cast<usize>(d) * sizeof(CookedFrameDirective),
                    sizeof(CookedFrameDirective));
                if (dir.triggerFrame != currentFrame) continue;

                const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
                switch (dir.op) {
                    case FrameDirectiveOp::None:
                        break;
                    case FrameDirectiveOp::EmitAudioCue:
                    case FrameDirectiveOp::EmitVfxCue: {
                        SimCue cue{};
                        cue.assetId = AssetId{static_cast<u64>(dir.assetHashOrParam)};
                        cue.worldX = xf.x + dir.boxOrMotion.OffsetX();
                        cue.worldY = xf.y + dir.boxOrMotion.OffsetY();
                        cue.emitter = id;
                        cue.kind = (dir.op == FrameDirectiveOp::EmitAudioCue)
                                       ? SimCueKind::AudioOneShot
                                       : SimCueKind::SpawnPrefabEffect;
                        cue.channel = dir.auxByte;
                        pipeline.EmitCue(cue);
                        break;
                    }
                    case FrameDirectiveOp::ActivateHitbox:
                        if (hasHitbox && (entMask & (1ULL << HitboxRig2D::kTypeId)) != 0ULL) {
                            HitboxRig2D& rig = world.GetPool<HitboxRig2D>().DirectAtEntityIndex(id.index);
                            const i16 dmg = static_cast<i16>(dir.assetHashOrParam & 0xFFFFU);
                            const u8 poiseDmg = static_cast<u8>((dir.assetHashOrParam >> 16U) & 0xFFU);
                            const u8 prio = static_cast<u8>((dir.assetHashOrParam >> 24U) & 0x03U);
                            rig.BeginNewSwing(
                                dir.boxOrMotion, dmg, poiseDmg, rig.KnockbackX(), rig.KnockbackY(), prio, dir.auxByte);
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
                            const f32 sign = static_cast<f32>(ms.FacingSign());
                            ms.AddRootMotionMeters(dir.boxOrMotion.OffsetX() * sign, dir.boxOrMotion.OffsetY());
                        }
                        break;
                    case FrameDirectiveOp::SetInvulnerable:
                        if (hasRecv && (entMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL) {
                            DamageReceiver2D& r = world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(id.index);
                            if (dir.auxByte != 0U) r.AddFlags(ReceiverFlags::Invulnerable);
                            else                   r.RemoveFlags(ReceiverFlags::Invulnerable);
                        }
                        break;
                }
            }
        }

        if (policies.onStepEntityState) {
            policies.onStepEntityState(id, fsm, world, input, actions, pipeline);
        }

        ++fsm.stateTicks;
        if (activeStateEntry && activeStateEntry->durationTicks > 0U &&
            fsm.stateTicks >= activeStateEntry->durationTicks) {
            const u8 oldState = fsm.stateId;
            const u8 nextState = activeStateEntry->nextStateId;
            fsm.TransitionTo(nextState);
            if (policies.onStateTransition) {
                policies.onStateTransition(id, oldState, nextState, world, pipeline);
            }
        }
    }
}

u16 StateAndCombatSolver2D::ResolveCombatOverlaps(
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
    const bool hasFsm = (regMask & (1ULL << StateMachine2D::kTypeId)) != 0ULL;
    const bool hasRecv = (regMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL;
    const bool hasBody = (regMask & (1ULL << RigidBody2D::kTypeId)) != 0ULL;

    const auto entities = rigPool.Entities();
    auto rigs = rigPool.Components();
    u16 hitCount = 0U;

    const auto computeWorldBox = [&](EntityId id, const CompactAabb2D& localBox, bool flipFacing) noexcept -> Rect {
        const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        f32 sign = 1.0f;
        if (flipFacing && hasMotion &&
            (world.GetComponentMaskByIndex(id.index) & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
            sign = static_cast<f32>(world.GetPool<MotionStack2D>().DirectAtEntityIndex(id.index).FacingSign());
        }
        const f32 hw = localBox.HalfW() * std::abs(xf.scaleX);
        const f32 hh = localBox.HalfH() * std::abs(xf.scaleY);
        const f32 cx = xf.x + localBox.OffsetX() * xf.scaleX * sign;
        const f32 cy = xf.y + localBox.OffsetY() * xf.scaleY;
        return {cx - hw, cy - hh, 2.0f * hw, 2.0f * hh};
    };

    const auto boxesOverlap = [](const Rect& a, const Rect& b) noexcept -> bool {
        return a.x <= b.Right() && a.Right() >= b.x &&
               a.y <= b.Bottom() && a.Bottom() >= b.y;
    };

    const auto applyEntityHitstopAndCue = [&](EntityId idA, EntityId idB, f32 ptX, f32 ptY, u8 ticks) noexcept {
        if (ticks == 0U) return;
        if (hasFsm) {
            if ((world.GetComponentMaskByIndex(idA.index) & (1ULL << StateMachine2D::kTypeId)) != 0ULL) {
                world.GetPool<StateMachine2D>().DirectAtEntityIndex(idA.index).hitstopTicks = ticks;
            }
            if ((world.GetComponentMaskByIndex(idB.index) & (1ULL << StateMachine2D::kTypeId)) != 0ULL) {
                world.GetPool<StateMachine2D>().DirectAtEntityIndex(idB.index).hitstopTicks = ticks;
            }
        }
        SimCue freezeCue{};
        freezeCue.worldX = ptX;
        freezeCue.worldY = ptY;
        freezeCue.emitter = idA;
        freezeCue.durationTicks = ticks;
        freezeCue.kind = SimCueKind::HitstopFreeze;
        pipeline.EmitCue(freezeCue);
    };

    // Unified single-pass combat evaluator: outer loop visits ONLY active attackers (HitboxActive)
    for (usize i = 0U; i < entities.size(); ++i) {
        HitboxRig2D& attRig = rigs[i];
        if (!HasPhysicsFlag(attRig.flags, HitboxFlags::HitboxActive)) continue;

        const EntityId attId = entities[i];
        const auto attCh = static_cast<TimeChannel>(world.GetTimeChannelByIndex(attId.index));
        if (step.IsFrozen(attCh)) continue;

        const u64 attMask = world.GetComponentMaskByIndex(attId.index);
        const Rect hitBoxA = computeWorldBox(attId, attRig.hitbox, HasPhysicsFlag(attRig.flags, HitboxFlags::FlipWithFacing));

        // 1. Check weapon-vs-weapon Clash against other active CanClash swings first
        if (HasPhysicsFlag(attRig.flags, HitboxFlags::CanClash)) {
            bool clashed = false;
            for (usize j = i + 1U; j < entities.size(); ++j) {
                if (hitCount >= outEvents.size()) break;
                HitboxRig2D& rigB = rigs[j];
                if ((rigB.flags & (HitboxFlags::HitboxActive | HitboxFlags::CanClash)) !=
                    (HitboxFlags::HitboxActive | HitboxFlags::CanClash)) {
                    continue;
                }
                if (attRig.Priority() != rigB.Priority()) continue;
                const EntityId idB = entities[j];
                if (attRig.HasAlreadyHit(idB) || rigB.HasAlreadyHit(attId)) continue;
                if ((attRig.hitLayerMask & (1U << rigB.HurtLayer())) == 0U &&
                    (rigB.hitLayerMask & (1U << attRig.HurtLayer())) == 0U) {
                    continue;
                }

                const Rect hitBoxB = computeWorldBox(idB, rigB.hitbox, HasPhysicsFlag(rigB.flags, HitboxFlags::FlipWithFacing));
                if (!boxesOverlap(hitBoxA, hitBoxB)) continue;

                attRig.RecordDefenderHit(idB);
                rigB.RecordDefenderHit(attId);
                attRig.EndSwing();
                rigB.EndSwing();

                CombatHitEvent ev{};
                ev.attacker = attId;
                ev.defender = idB;
                ev.hitPointX = 0.5f * (hitBoxA.Center().x + hitBoxB.Center().x);
                ev.hitPointY = 0.5f * (hitBoxA.Center().y + hitBoxB.Center().y);
                ev.hitstopTicks = std::max(attRig.HitstopTicks(), rigB.HitstopTicks());
                ev.outcome = HitOutcome::Clashed;
                outEvents[hitCount++] = ev;

                applyEntityHitstopAndCue(attId, idB, ev.hitPointX, ev.hitPointY, ev.hitstopTicks);
                if (policies.onCombatHit) {
                    policies.onCombatHit(ev, world, pipeline);
                }
                clashed = true;
                break;
            }
            if (clashed) continue;
        }

        // 2. Check Hitbox-vs-Hurtbox against valid defenders
        const u8 attTeam = (hasRecv && (attMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL)
                               ? world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(attId.index).TeamId()
                               : 0U;
        f32 attFacing = 1.0f;
        if (hasMotion && (attMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
            attFacing = static_cast<f32>(world.GetPool<MotionStack2D>().DirectAtEntityIndex(attId.index).FacingSign());
        }

        for (usize j = 0U; j < entities.size(); ++j) {
            if (i == j || hitCount >= outEvents.size()) continue;
            const HitboxRig2D& defRig = rigs[j];
            if (!HasPhysicsFlag(defRig.flags, HitboxFlags::HurtboxActive)) continue;
            if ((attRig.hitLayerMask & (1U << defRig.HurtLayer())) == 0U) continue;

            const EntityId defId = entities[j];
            if (attRig.HasAlreadyHit(defId)) continue;

            const u64 defMask = world.GetComponentMaskByIndex(defId.index);
            DamageReceiver2D* defRecv = (hasRecv && (defMask & (1ULL << DamageReceiver2D::kTypeId)) != 0ULL)
                                            ? &world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(defId.index)
                                            : nullptr;
            if (defRecv) {
                if (attTeam != 0U && defRecv->TeamId() == attTeam) continue;
                if (defRecv->HasFlag(ReceiverFlags::Dead | ReceiverFlags::Invulnerable) ||
                    defRecv->iframeTicks > 0U) {
                    continue;
                }
            }

            const Rect hurtBoxB = computeWorldBox(defId, defRig.hurtbox, false);
            if (!boxesOverlap(hitBoxA, hurtBoxB)) continue;

            attRig.RecordDefenderHit(defId);

            if (hasFsm && (attMask & (1ULL << StateMachine2D::kTypeId)) != 0ULL) {
                world.GetPool<StateMachine2D>().DirectAtEntityIndex(attId.index).PromoteOnHitCancels();
            }

            CombatHitEvent ev{};
            ev.attacker = attId;
            ev.defender = defId;
            ev.hitPointX = 0.5f * (std::max(hitBoxA.x, hurtBoxB.x) + std::min(hitBoxA.Right(), hurtBoxB.Right()));
            ev.hitPointY = 0.5f * (std::max(hitBoxA.y, hurtBoxB.y) + std::min(hitBoxA.Bottom(), hurtBoxB.Bottom()));
            ev.knockbackVx = attRig.KnockbackX() * attFacing;
            ev.knockbackVy = attRig.KnockbackY();
            ev.hitstopTicks = attRig.HitstopTicks();

            // Check directional parry (defender must have Parrying flag and face toward the attacker if MotionStack2D is attached)
            bool parryValid = (defRecv != nullptr) && defRecv->HasFlag(ReceiverFlags::Parrying) &&
                              !HasPhysicsFlag(attRig.flags, HitboxFlags::Unblockable);
            if (parryValid && hasMotion && (defMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
                const f32 defFacing = static_cast<f32>(world.GetPool<MotionStack2D>().DirectAtEntityIndex(defId.index).FacingSign());
                const f32 dxToAttacker = world.GetWorldByIndex(attId.index).x - world.GetWorldByIndex(defId.index).x;
                if (dxToAttacker * defFacing < -0.05f) {
                    parryValid = false; // Hit from behind bypasses directional parry!
                }
            }

            if (parryValid) {
                ev.outcome = HitOutcome::Parried;
                ev.damageApplied = 0;
                if (hasMotion && (attMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
                    world.GetPool<MotionStack2D>().DirectAtEntityIndex(attId.index)
                        .AddKnockbackImpulse(-ev.knockbackVx * 0.6f, 0.0f);
                }
            } else if (defRecv && defRecv->HasFlag(ReceiverFlags::SuperArmor) &&
                       defRecv->poise > attRig.poiseDamage) {
                ev.outcome = HitOutcome::ArmorAbsorbed;
                ev.damageApplied = attRig.baseDamage;
                defRecv->poise = static_cast<u8>(defRecv->poise - attRig.poiseDamage);
                defRecv->hp -= attRig.baseDamage;
                if (defRecv->hp <= 0) {
                    defRecv->hp = 0;
                    defRecv->AddFlags(ReceiverFlags::Dead);
                    ev.defenderDied = 1U;
                }
            } else {
                ev.outcome = HitOutcome::Damaged;
                ev.damageApplied = attRig.baseDamage;
                if (defRecv) {
                    defRecv->hp -= attRig.baseDamage;
                    defRecv->iframeTicks = std::max<u16>(defRecv->iframeTicks, defRecv->autoIframeTicks);
                    if (defRecv->poise <= attRig.poiseDamage) {
                        defRecv->poise = defRecv->maxPoise;
                        defRecv->stunTicks = 15U;
                        ev.poiseBroken = 1U;
                    } else {
                        defRecv->poise = static_cast<u8>(defRecv->poise - attRig.poiseDamage);
                    }
                    if (defRecv->hp <= 0) {
                        defRecv->hp = 0;
                        defRecv->AddFlags(ReceiverFlags::Dead);
                        ev.defenderDied = 1U;
                    }
                }

                if (hasMotion && (defMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
                    world.GetPool<MotionStack2D>().DirectAtEntityIndex(defId.index)
                        .AddKnockbackImpulse(ev.knockbackVx, ev.knockbackVy);
                } else if (hasBody && (defMask & (1ULL << RigidBody2D::kTypeId)) != 0ULL) {
                    world.GetPool<RigidBody2D>().DirectAtEntityIndex(defId.index)
                        .ApplyLinearImpulse(ev.knockbackVx, ev.knockbackVy);
                }
            }

            outEvents[hitCount++] = ev;
            applyEntityHitstopAndCue(attId, defId, ev.hitPointX, ev.hitPointY, ev.hitstopTicks);

            if (policies.onCombatHit) {
                policies.onCombatHit(ev, world, pipeline);
            }
        }
    }

    return hitCount;
}

} // namespace en
