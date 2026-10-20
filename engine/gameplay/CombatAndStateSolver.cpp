#include "gameplay/CombatAndStateSolver.h"
#include "core/Serialization.h"

namespace en {

void CombatAndStateSolver::DispatchAnimationFrameDirectives(
    World& world, const AssetBank& assetBank, const SimStepView& step,
    GameplayPipeline& /*pipeline*/) noexcept {
    world.Each<StateMachine2D, HitboxRig2D>(
        [&](EntityId id, StateMachine2D& fsm, HitboxRig2D& rig) noexcept {
            const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
            if (step.IsFrozen(ch)) return;
            const BlobRecord& blob = assetBank.GetBlob(fsm.rigBlob);
            if (!blob.IsValid() || blob.size < sizeof(CookedRigHeader)) return;

            BinaryReader r(blob.Bytes());
            CookedRigHeader hdr{};
            if (!r.Read(hdr) || hdr.magic != kCookedRigMagic) return;

            for (u16 i = 0; i < hdr.directiveCount; ++i) {
                CookedRigFrameDirective dir{};
                if (!r.Read(dir)) break;
                if (dir.stateId == fsm.stateId && dir.triggerTick == fsm.stateTicks) {
                    if (dir.enableHitbox != 0) {
                        rig.BeginNewSwing(dir.damage, dir.hitbox);
                    } else {
                        rig.EndSwing();
                    }
                    if (dir.rootVelX_q4 != 0 || dir.rootVelY_q4 != 0) {
                        if (MotionStack2D* ms = world.TryGet<MotionStack2D>(id)) {
                            const f32 facing = static_cast<f32>(ms->facingSign >= 0 ? 1 : -1);
                            ms->AddImpulse(
                                static_cast<f32>(dir.rootVelX_q4) * (1.0f / 16.0f) * facing,
                                static_cast<f32>(dir.rootVelY_q4) * (1.0f / 16.0f));
                        }
                    }
                }
            }
        });
}

void CombatAndStateSolver::StepStateMachinesAndPolicies(
    World& world, const AssetBank& assetBank, const SimStepView& step,
    const TickInput& tickInput, ActionBuffer& actionBuffer,
    const GameplayPolicyTable& policies, GameplayPipeline& pipeline) noexcept {
    world.Each<StateMachine2D>([&](EntityId id, StateMachine2D& fsm) noexcept {
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (!step.IsFrozen(ch) && fsm.stateTicks < 0xFFFFU) {
            ++fsm.stateTicks;
        }
    });

    world.Each<DamageReceiver2D>([&](EntityId id, DamageReceiver2D& dmg) noexcept {
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) return;
        if (dmg.iframeTicks > 0) --dmg.iframeTicks;
        if (dmg.stunTicks > 0) --dmg.stunTicks;
        if (dmg.statusTicks > 0) {
            --dmg.statusTicks;
            if ((dmg.statusMask & 1U) != 0 && (dmg.statusTicks % 30U) == 0U && dmg.hp > 1) {
                --dmg.hp;
            }
            if (dmg.statusTicks == 0) dmg.statusMask = 0;
        }
    });

    if (policies.onStepStateMachines) {
        policies.onStepStateMachines(world, assetBank, step, tickInput, actionBuffer, pipeline);
    }
}

u32 CombatAndStateSolver::GatherAndResolveCombatHits(
    World& world, const SimStepView& step, std::span<ResolvedHitEvent> hitScratch,
    const GameplayPolicyTable& policies, GameplayPipeline& pipeline) noexcept {
    u32 hitCount = 0;

    world.Each<HitboxRig2D>([&](EntityId atkId, HitboxRig2D& atkRig) noexcept {
        if (!HasFlag(atkRig.rigFlags, HitboxRigFlags::HitboxActive)) return;
        const WorldTransform2D& atkWt = world.GetWorldByIndex(atkId.index);
        f32 facing = 1.0f;
        if (const MotionStack2D* ms = world.TryGet<MotionStack2D>(atkId)) {
            facing = ms->facingSign >= 0 ? 1.0f : -1.0f;
        }
        const Vec2 hbOffset = atkRig.primaryHitbox.Offset();
        const Vec2 hbCenter{atkWt.x + hbOffset.x * facing, atkWt.y + hbOffset.y};
        const Rect atkRect = Rect::FromCenterHalf(hbCenter, atkRig.primaryHitbox.HalfExtents());

        const DamageReceiver2D* atkDmg = world.TryGet<DamageReceiver2D>(atkId);
        const u8 atkTeam = atkDmg ? atkDmg->teamId : 0xFFU;

        world.Each<HitboxRig2D, DamageReceiver2D>(
            [&](EntityId defId, const HitboxRig2D& defRig, const DamageReceiver2D& defDmg) noexcept {
                if (atkId == defId || hitCount >= hitScratch.size()) return;
                if (!HasFlag(defRig.rigFlags, HitboxRigFlags::HurtboxActive)) return;
                if (HasFlag(defRig.rigFlags, HitboxRigFlags::Invulnerable)) return;
                if ((atkRig.hitLayerMask & defRig.hurtLayerMask) == 0) return;
                if (atkTeam != 0xFFU && atkTeam == defDmg.teamId) return;
                if (defDmg.iframeTicks > 0 || defDmg.IsDead()) return;
                if (atkRig.HasAlreadyHit(defId)) return;

                const WorldTransform2D& defWt = world.GetWorldByIndex(defId.index);
                const Rect defRect = defRig.hurtbox.WorldRect({defWt.x, defWt.y});
                if (!atkRect.Overlaps(defRect)) return;

                atkRig.MarkHit(defId);
                ResolvedHitEvent ev{};
                ev.attacker = atkId;
                ev.defender = defId;
                ev.contactPoint = (atkRect.Center() + defRect.Center()) * 0.5f;
                ev.knockbackDir = {(defWt.x >= atkWt.x) ? 1.0f : -1.0f, 0.25f};
                ev.damage = static_cast<i16>(atkRig.activeDamage);
                ev.hitstopTicks = 4;
                ev.parried = HasFlag(defRig.rigFlags, HitboxRigFlags::ParryWindow);
                ev.reflected = HasFlag(defRig.rigFlags, HitboxRigFlags::ReflectProjectiles);
                hitScratch[hitCount++] = ev;
            });
    });

    if (hitCount > 0) {
        if (policies.onResolveCombatHits) {
            policies.onResolveCombatHits(world, step, hitScratch.subspan(0, hitCount), pipeline);
        } else {
            for (u32 i = 0; i < hitCount; ++i) {
                const ResolvedHitEvent& ev = hitScratch[i];
                if (!ev.parried) {
                    if (DamageReceiver2D* dmg = world.TryGet<DamageReceiver2D>(ev.defender)) {
                        dmg->hp = static_cast<i16>(std::max<i32>(0, dmg->hp - ev.damage));
                    }
                }
            }
        }
    }
    return hitCount;
}

} // namespace en
