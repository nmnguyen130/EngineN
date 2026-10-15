#include "gameplay/StateAndCombatSolver.h"
#include "gameplay/GameplayPipeline.h"

#include "assets/AssetBank.h"
#include "world/World.h"

#include <algorithm>
#include <cmath>

namespace en::gameplay {

void StateAndCombatSolver::GateFrozenAnimators(
    world::World& world,
    time::SimStepView step,
    bool restoreAfterStep) noexcept {
    for (auto [id, anim, motor] : world.View<world::SpriteAnimator2D, KinematicMotor2D>()) {
        if (!step.IsFrozen(motor.timeChannel)) {
            continue;
        }
        if (auto* rig = world.TryGet<HitboxRig2D>(id)) {
            if (!restoreAfterStep) {
                if (anim.playing) {
                    rig->rigFlags |= static_cast<u8>(HitboxRigFlags::LatchedAnimPlay);
                } else {
                    rig->rigFlags &= ~static_cast<u8>(HitboxRigFlags::LatchedAnimPlay);
                }
                anim.playing = false;
            } else {
                anim.playing = (rig->rigFlags & static_cast<u8>(HitboxRigFlags::LatchedAnimPlay)) != 0;
            }
        } else {
            anim.playing = restoreAfterStep;
        }
    }
}

void StateAndCombatSolver::DispatchAnimationFrameDirectives(
    world::World& world,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    GameplayPipeline& pipeline) noexcept {
    for (auto [id, fsm] : world.View<StateMachine2D>()) {
        const auto* motor = world.TryGet<KinematicMotor2D>(id);
        if (motor != nullptr && step.IsFrozen(motor->timeChannel)) {
            continue;
        }

        u16 currentFrame = fsm.stateTicks;
        if (const auto* anim = world.TryGet<world::SpriteAnimator2D>(id)) {
            currentFrame = anim->currentFrame;
        }

        if (currentFrame == fsm.lastAnimFrame) {
            continue;
        }

        const u16 prevFrame = fsm.lastAnimFrame;
        fsm.lastAnimFrame = currentFrame;

        const assets::BlobRecord& blob = assetBank.GetBlob(fsm.rigBlob);
        if (blob.data == nullptr || blob.size < sizeof(CookedAnimRigHeader)) {
            continue;
        }

        const auto* hdr = reinterpret_cast<const CookedAnimRigHeader*>(blob.data);
        if (hdr->magic != CookedAnimRigHeader::kMagic ||
            hdr->version != CookedAnimRigHeader::kVersion) {
            continue;
        }

        const auto* directives = reinterpret_cast<const CookedFrameDirective*>(
            blob.data + hdr->directiveOffset);
        const auto* hurtProfiles = reinterpret_cast<const CompactAabb2D*>(
            blob.data + hdr->hurtboxProfileOffset);
        const auto* slices = reinterpret_cast<const CookedHitboxSlice*>(
            blob.data + hdr->hitboxSliceOffset);

        auto* motion = world.TryGet<MotionStack2D>(id);
        auto* rig    = world.TryGet<HitboxRig2D>(id);

        for (u16 i = 0; i < hdr->directiveCount; ++i) {
            const CookedFrameDirective& dir = directives[i];
            if (dir.clipId != fsm.animClipId) {
                continue;
            }

            const bool frameEntered = (prevFrame == 0xFFFFu)
                ? (dir.frameIndex <= currentFrame)
                : ((currentFrame > prevFrame)
                    ? (dir.frameIndex > prevFrame && dir.frameIndex <= currentFrame)
                    : (dir.frameIndex > prevFrame || dir.frameIndex <= currentFrame));

            if (!frameEntered) {
                continue;
            }

            switch (dir.kind) {
            case FrameDirectiveKind::SetCancelMask:
                fsm.cancelMask = dir.paramU16;
                break;

            case FrameDirectiveKind::SetOnHitCancelMask:
                fsm.onHitCancelMask = dir.paramU16;
                break;

            case FrameDirectiveKind::SetHurtboxProfile:
                fsm.hurtboxProfileId = dir.paramU8;
                if (rig != nullptr && dir.paramU8 < hdr->hurtboxProfileCount) {
                    rig->hurtbox = hurtProfiles[dir.paramU8];
                    if (dir.paramU16 == 0) {
                        rig->rigFlags |= static_cast<u8>(HitboxRigFlags::HurtboxActive);
                    } else {
                        rig->rigFlags &= ~static_cast<u8>(HitboxRigFlags::HurtboxActive);
                    }
                }
                break;

            case FrameDirectiveKind::ActivateHitboxSlice:
                if (rig != nullptr && dir.paramU8 < hdr->hitboxSliceCount) {
                    const CookedHitboxSlice& slice = slices[dir.paramU8];
                    rig->ActivateSlice(dir.paramU8, slice.boundingUnion, dir.paramU16 != 0);
                }
                break;

            case FrameDirectiveKind::DeactivateHitbox:
                if (rig != nullptr) {
                    rig->DeactivateHitbox();
                }
                break;

            case FrameDirectiveKind::ApplyRootMotionDelta:
                if (motion != nullptr) {
                    motion->rootMotionDx_q8 = static_cast<i16>(motion->rootMotionDx_q8 + dir.paramI16);
                    motion->rootMotionDy_q8 = static_cast<i16>(motion->rootMotionDy_q8 + static_cast<i16>(dir.paramU16));
                }
                break;

            case FrameDirectiveKind::ApplyVelocityImpulse:
                if (motion != nullptr) {
                    const f32 sign = static_cast<f32>(motion->FacingSign());
                    motion->impulseVx += (static_cast<f32>(dir.paramI16) * (1.0f / 256.0f)) * sign;
                    motion->impulseVy += static_cast<f32>(static_cast<i16>(dir.paramU16)) * (1.0f / 256.0f);
                }
                break;

            case FrameDirectiveKind::EmitSimCue: {
                const auto& wt = world.GetWorldTransform(id);
                SimCue cue{};
                cue.assetId = assets::AssetId{static_cast<u64>(dir.paramU16)};
                cue.worldX = wt.x;
                cue.worldY = wt.y;
                cue.kind = SimCueKind::AudioOneShot;
                cue.channel = dir.paramU8;
                cue.userTag = static_cast<u16>(dir.paramI16);
                pipeline.EmitCue(cue);
                break;
            }
            }
        }
    }
}

void StateAndCombatSolver::StepStateMachinesAndPolicies(
    world::World& world,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    const platform::input::TickInput& tickInput,
    platform::input::ActionBuffer& actionBuffer,
    const GameplayPolicyTable& policies,
    GameplayPipeline& pipeline) noexcept {
    for (auto [id, dmg] : world.View<DamageReceiver2D>()) {
        time::TimeChannel ch = time::TimeChannel::World;
        if (const auto* motor = world.TryGet<KinematicMotor2D>(id)) {
            ch = motor->timeChannel;
        }
        if (step.IsFrozen(ch)) {
            continue;
        }
        if (dmg.iframeTicks > 0) --dmg.iframeTicks;
        if (dmg.stunTicks > 0) --dmg.stunTicks;
    }

    for (auto [id, motor, motion, fsm] :
         world.View<KinematicMotor2D, MotionStack2D, StateMachine2D>()) {
        if (step.IsFrozen(motor.timeChannel)) {
            continue;
        }

        if (fsm.stateTicks < 0xFFFEu) {
            ++fsm.stateTicks;
        }

        if (policies.evaluateActor != nullptr) {
            auto* optRig = world.TryGet<HitboxRig2D>(id);
            const ActorPolicyContext ctx{
                world, assetBank, step, tickInput, actionBuffer, pipeline, id};
            policies.evaluateActor(ctx, motor, motion, fsm, optRig);
        }
    }
}

void StateAndCombatSolver::ResolveCombatHits(
    world::World& world,
    time::SimStepView step,
    std::span<const ResolvedHitEvent> hits,
    const GameplayPolicyTable& policies,
    GameplayPipeline& pipeline) noexcept {
    if (hits.empty()) {
        return;
    }

    for (const ResolvedHitEvent& hit : hits) {
        auto* atkRig = world.TryGet<HitboxRig2D>(hit.attacker);
        if (atkRig == nullptr || atkRig->HasAlreadyHit(hit.defender)) {
            continue;
        }
        atkRig->MarkHit(hit.defender);

        if (auto* atkFsm = world.TryGet<StateMachine2D>(hit.attacker)) {
            atkFsm->PromoteCancelWindowOnHit();
        }

        if (hit.isClashParry != 0) {
            if (auto* defRig = world.TryGet<HitboxRig2D>(hit.defender)) {
                defRig->MarkHit(hit.attacker);
            }
            if (auto* atkDmg = world.TryGet<DamageReceiver2D>(hit.attacker)) {
                atkDmg->iframeTicks = std::max<u16>(atkDmg->iframeTicks, 14u);
            }
            step.RequestMultiHitstop(
                std::max<u32>(hit.hitstopTicks, 10u), 0.0f,
                time::TimeChannel::Player,
                time::TimeChannel::Enemy);
            continue;
        }

        if (auto* defDmg = world.TryGet<DamageReceiver2D>(hit.defender)) {
            if (defDmg->iframeTicks > 0 || defDmg->hp <= 0) {
                continue;
            }
            const i32 damage = std::max(1, (10 * static_cast<i32>(hit.damageMultiplier_q8)) >> 8);
            defDmg->hp = std::max(0, defDmg->hp - damage);
            defDmg->iframeTicks = 18;
            defDmg->stunTicks = 10;
        }

        if (auto* defMotion = world.TryGet<MotionStack2D>(hit.defender)) {
            const f32 rad = static_cast<f32>(hit.knockbackAngleDeg) * (3.14159265f / 180.0f);
            const f32 speed = static_cast<f32>(hit.knockbackSpeed_q2) * 0.25f;
            defMotion->PushOverride(6, std::cos(rad) * speed, std::sin(rad) * speed, 10, 192);
        }

        // Pogo bounce check (socketFlags bit 1 = pogoTrigger, bit 3 on target = PogoSurface).
        if ((hit.socketFlags & 0x02u) != 0 ||
            (hit.socketFlags & static_cast<u8>(HitboxRigFlags::PogoSurface)) != 0) {
            if (auto* atkMotion = world.TryGet<MotionStack2D>(hit.attacker)) {
                atkMotion->PushOverride(3, atkMotion->targetWalkVx, 13.5f, 6, 256);
            }
        }

        if (hit.hitstopTicks > 0) {
            step.RequestMultiHitstop(
                hit.hitstopTicks, 0.0f,
                time::TimeChannel::Player,
                time::TimeChannel::Enemy);
        }

        for (auto [camId, cam] : world.View<CameraController2D>()) {
            const f32 rad = static_cast<f32>(hit.knockbackAngleDeg) * (3.14159265f / 180.0f);
            cam.AddDirectionalKick(std::cos(rad) * 2.5f, std::sin(rad) * 1.5f);
            cam.AddTrauma(0.22f);
        }
    }

    if (policies.resolveCombatHits != nullptr) {
        policies.resolveCombatHits(world, step, hits, pipeline);
    }
}

} // namespace en::gameplay
