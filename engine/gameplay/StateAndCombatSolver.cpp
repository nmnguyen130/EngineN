#include "gameplay/StateAndCombatSolver.h"

#include "assets/AssetBank.h"
#include "gameplay/GameplayPipeline.h"
#include "input/ActionBuffer.h"
#include "world/World.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace en::gameplay {

void StateAndCombatSolver::DispatchAnimationFrameDirectives(
    world::World& world,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    GameplayPipeline& pipeline) noexcept {

    const u8 activeChannelMask = static_cast<u8>(~step.FrozenMask());
    if (activeChannelMask == 0) return;

    alignas(16) std::array<u16, 8> chSubTicks{};
    for (u32 m = activeChannelMask; m != 0; m &= (m - 1u)) {
        const u8 c = static_cast<u8>(std::countr_zero(m));
        chSubTicks[c] = static_cast<u16>(
            step.EffectiveScale(static_cast<time::TimeChannel>(c)) *
            static_cast<f32>(world::SpriteAnimator2D::kSubTicksPerTick) + 0.5f);
    }

    constexpr world::ComponentMask kSpriteBit = world::MakeComponentMask<world::Sprite2D>();
    constexpr world::ComponentMask kFsmBit    = world::MakeComponentMask<StateMachine2D>();
    constexpr world::ComponentMask kMotionBit = world::MakeComponentMask<MotionStack2D>();
    constexpr world::ComponentMask kRigBit    = world::MakeComponentMask<HitboxRig2D>();

    auto& spritePool = world.GetPool<world::Sprite2D>();
    auto& animPool   = world.GetPool<world::SpriteAnimator2D>();
    auto& fsmPool    = world.GetPool<StateMachine2D>();
    auto& motionPool = world.GetPool<MotionStack2D>();
    auto& rigPool    = world.GetPool<HitboxRig2D>();

    const auto entities = animPool.Entities();
    auto anims          = animPool.Components();
    const auto* __restrict channels        = world.TimeChannels().data();
    const auto* __restrict worldTransforms = world.WorldTransforms().data();

    for (u16 i = 0, animCount = animPool.Size(); i < animCount; ++i) {
        const world::EntityId entity = entities[i];
        const u16 idx                = entity.index;
        const u8 chIdx               = static_cast<u8>(channels[idx]);
        if ((activeChannelMask & (1u << chIdx)) == 0) continue;

        world::SpriteAnimator2D& anim  = anims[i];
        const world::ComponentMask msk = world.GetComponentMaskByIndex(idx);
        const u16 deltaSub             = chSubTicks[chIdx];

        if (anim.playing && anim.frameCount > 1 && deltaSub > 0) {
            const u32 targetSub = static_cast<u32>(std::max<u16>(1u, anim.ticksPerFrame)) *
                                  world::SpriteAnimator2D::kSubTicksPerTick;
            u32 accum = static_cast<u32>(anim.subTickAccum) + deltaSub;
            if (accum >= targetSub) {
                const u32 nextFrame = static_cast<u32>(anim.currentFrame) + (accum / targetSub);
                accum %= targetSub;
                if (nextFrame >= anim.frameCount) {
                    if (anim.loop) {
                        anim.currentFrame = static_cast<u16>(nextFrame % anim.frameCount);
                    } else {
                        anim.currentFrame = static_cast<u16>(anim.frameCount - 1u);
                        anim.playing      = 0;
                        accum             = 0;
                    }
                } else {
                    anim.currentFrame = static_cast<u16>(nextFrame);
                }
                if ((msk & kSpriteBit) != 0) {
                    spritePool.DirectAtEntityIndex(idx).sprite = assetBank.GetSpriteHandle(
                        anim.atlas, static_cast<u16>(anim.firstFrame + anim.currentFrame));
                }
            }
            anim.subTickAccum = static_cast<u16>(accum);
        }

        if ((msk & kFsmBit) == 0) continue;

        StateMachine2D& fsm = fsmPool.DirectAtEntityIndex(idx);
        const u16 currFrame = anim.currentFrame;
        const u16 prevFrame = fsm.lastAnimFrame;
        if (currFrame == prevFrame) continue;
        fsm.lastAnimFrame = currFrame;

        const assets::BlobRecord& blob = assetBank.GetBlob(fsm.rigBlob);
        if (blob.data == nullptr || blob.size < sizeof(CookedAnimRigHeader)) continue;
        const auto* rigHdr = reinterpret_cast<const CookedAnimRigHeader*>(blob.data);
        if (rigHdr->magic != CookedAnimRigHeader::kMagic) continue;

        MotionStack2D* const optMotion = ((msk & kMotionBit) != 0) ? &motionPool.DirectAtEntityIndex(idx) : nullptr;
        HitboxRig2D* const optRig      = ((msk & kRigBit) != 0)    ? &rigPool.DirectAtEntityIndex(idx)    : nullptr;
        const auto& wtf                = worldTransforms[idx];
        const i8 facingSign            = optMotion != nullptr ? optMotion->FacingSign() : i8{1};

        const auto* directives   = reinterpret_cast<const CookedFrameDirective*>(blob.data + rigHdr->directiveOffset);
        const auto* hurtProfiles = reinterpret_cast<const CompactAabb2D*>(blob.data + rigHdr->hurtboxProfileOffset);
        const auto* slices       = reinterpret_cast<const CookedHitboxSlice*>(blob.data + rigHdr->hitboxSliceOffset);

        auto runDirectiveRange = [&](u16 fromFrame, u16 toFrame) noexcept {
            for (u16 d = 0; d < rigHdr->directiveCount; ++d) {
                const CookedFrameDirective& dir = directives[d];
                if (dir.clipId != fsm.animClipId || dir.frameIndex < fromFrame || dir.frameIndex > toFrame) continue;
                switch (dir.kind) {
                    case FrameDirectiveKind::SetCancelMask:
                        fsm.cancelMask = dir.paramU16;
                        break;
                    case FrameDirectiveKind::SetOnHitCancelMask:
                        fsm.onHitCancelMask = dir.paramU16;
                        break;
                    case FrameDirectiveKind::SetHurtboxProfile:
                        if (optRig != nullptr && dir.paramU8 < rigHdr->hurtboxProfileCount) {
                            fsm.hurtboxProfileId = dir.paramU8;
                            optRig->hurtbox      = hurtProfiles[dir.paramU8];
                        }
                        break;
                    case FrameDirectiveKind::ActivateHitboxSlice:
                        if (optRig != nullptr && dir.paramU8 < rigHdr->hitboxSliceCount) {
                            optRig->ActivateSlice(dir.paramU8, slices[dir.paramU8].boundingUnion, dir.paramU16 != 0);
                        }
                        break;
                    case FrameDirectiveKind::DeactivateHitbox:
                        if (optRig != nullptr) optRig->DeactivateHitbox();
                        break;
                    case FrameDirectiveKind::ApplyRootMotionDelta:
                        if (optMotion != nullptr) {
                            optMotion->rootMotionDx_q8 = static_cast<i16>(
                                optMotion->rootMotionDx_q8 + static_cast<i32>(dir.paramI16) * facingSign);
                            optMotion->rootMotionDy_q8 = static_cast<i16>(
                                optMotion->rootMotionDy_q8 + static_cast<i16>(dir.paramU16));
                        }
                        break;
                    case FrameDirectiveKind::ApplyVelocityImpulse:
                        if (optMotion != nullptr) {
                            optMotion->impulseVx += static_cast<f32>(dir.paramI16) * (1.0f / 256.0f) * static_cast<f32>(facingSign);
                            optMotion->impulseVy += static_cast<f32>(static_cast<i16>(dir.paramU16)) * (1.0f / 256.0f);
                        }
                        break;
                    case FrameDirectiveKind::EmitSimCue:
                        pipeline.EmitCue({assets::AssetId{static_cast<u64>(dir.paramU16)}, wtf.x, wtf.y, entity,
                                          65535u, 0, static_cast<SimCueKind>(dir.paramU8), 0, static_cast<u16>(dir.paramI16)});
                        break;
                }
            }
        };

        if (prevFrame != 0xFFFFu && currFrame < prevFrame && anim.frameCount > 0) {
            const u16 lastValid = static_cast<u16>(anim.frameCount - 1u);
            if (prevFrame < lastValid) runDirectiveRange(static_cast<u16>(prevFrame + 1u), lastValid);
            runDirectiveRange(0u, currFrame);
        } else {
            runDirectiveRange(prevFrame == 0xFFFFu ? 0u : static_cast<u16>(prevFrame + 1u), currFrame);
        }
    }
}

void StateAndCombatSolver::StepStateMachinesAndPolicies(
    world::World& world,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    const input::TickInput& tickInput,
    input::ActionBuffer& actionBuffer,
    const GameplayPolicyTable& policies,
    GameplayPipeline& pipeline) noexcept {

    const u8 frozenMask = step.FrozenMask();
    if (frozenMask == 0xFFu) return;

    const auto* __restrict channels = world.TimeChannels().data();
    auto& dmgPool    = world.GetPool<DamageReceiver2D>();
    const auto dmgIds = dmgPool.Entities();
    auto dmgs        = dmgPool.Components();

    for (u16 i = 0, dmgCount = dmgPool.Size(); i < dmgCount; ++i) {
        if ((frozenMask & (1u << static_cast<u8>(channels[dmgIds[i].index]))) == 0) {
            DamageReceiver2D& dmg = dmgs[i];
            dmg.iframeTicks -= (dmg.iframeTicks > 0);
            dmg.stunTicks   -= (dmg.stunTicks > 0);
        }
    }

    constexpr world::ComponentMask kReqActorMask = world::MakeComponentMask<KinematicMotor2D, MotionStack2D>();
    constexpr world::ComponentMask kRigBit       = world::MakeComponentMask<HitboxRig2D>();
    constexpr world::ComponentMask kSpriteBit    = world::MakeComponentMask<world::Sprite2D>();

    auto& fsmPool    = world.GetPool<StateMachine2D>();
    auto& motorPool  = world.GetPool<KinematicMotor2D>();
    auto& motionPool = world.GetPool<MotionStack2D>();
    auto& rigPool    = world.GetPool<HitboxRig2D>();
    auto& spritePool = world.GetPool<world::Sprite2D>();

    const auto fsmIds = fsmPool.Entities();
    auto fsms         = fsmPool.Components();

    for (u16 i = 0, fsmCount = fsmPool.Size(); i < fsmCount; ++i) {
        const world::EntityId entity   = fsmIds[i];
        const u16 idx                  = entity.index;
        const world::ComponentMask msk = world.GetComponentMaskByIndex(idx);
        if ((msk & kReqActorMask) != kReqActorMask) continue;

        KinematicMotor2D& motor = motorPool.DirectAtEntityIndex(idx);
        if ((frozenMask & (1u << static_cast<u8>(motor.timeChannel))) != 0) continue;

        MotionStack2D& motion = motionPool.DirectAtEntityIndex(idx);
        StateMachine2D& fsm   = fsms[i];

        fsm.stateTicks += (fsm.stateTicks < 0xFFFFu);

        if (motion.overrideTicks > 0 && --motion.overrideTicks == 0) {
            motion.activePriority  = 0;
            motion.gravityScale_q8 = 256;
        }
        if (!HasContact(motor.contacts, ContactFlags::Ground) && motion.groundCoyoteTicks > 0) {
            --motion.groundCoyoteTicks;
        }
        if (!HasContact(motor.contacts, ContactFlags::WallLeft | ContactFlags::WallRight) &&
            motion.wallCoyoteTicks > 0) {
            --motion.wallCoyoteTicks;
        }

        if (policies.evaluateActor != nullptr) {
            HitboxRig2D* const optRig = ((msk & kRigBit) != 0) ? &rigPool.DirectAtEntityIndex(idx) : nullptr;
            const ActorPolicyContext ctx{world, assetBank, step, tickInput, actionBuffer, pipeline, entity};
            policies.evaluateActor(ctx, motor, motion, fsm, optRig);
        }

        if (!motion.IsFacingLocked() && fsm.CanCancelInto(CancelCategory::MovementTurn)) {
            if (motion.targetWalkVx > 0.15f)       motion.SetFacingSign(1);
            else if (motion.targetWalkVx < -0.15f) motion.SetFacingSign(-1);
        }

        if ((msk & kSpriteBit) != 0) {
            world::Sprite2D& sp = spritePool.DirectAtEntityIndex(idx);
            if (motion.FacingSign() < 0) sp.blendAndFlags |= static_cast<u8>(world::SpriteFlags::FlipX);
            else                         sp.blendAndFlags &= ~static_cast<u8>(world::SpriteFlags::FlipX);
        }
    }
}

void StateAndCombatSolver::ResolveCombatHits(
    world::World& world,
    time::SimStepView step,
    std::span<const ResolvedHitEvent> hits,
    const GameplayPolicyTable& policies,
    GameplayPipeline& pipeline) noexcept {

    constexpr world::ComponentMask kRigBit    = world::MakeComponentMask<HitboxRig2D>();
    constexpr world::ComponentMask kFsmBit    = world::MakeComponentMask<StateMachine2D>();
    constexpr world::ComponentMask kMotionBit = world::MakeComponentMask<MotionStack2D>();

    auto& rigPool    = world.GetPool<HitboxRig2D>();
    auto& fsmPool    = world.GetPool<StateMachine2D>();
    auto& motionPool = world.GetPool<MotionStack2D>();

    f32 accumCameraTrauma = 0.0f, accumKickVx = 0.0f, accumKickVy = 0.0f;

    for (const ResolvedHitEvent& ev : hits) {
        const world::ComponentMask attMask = world.GetComponentMaskByIndex(ev.attacker.index);
        if ((attMask & kRigBit) != 0) rigPool.DirectAtEntityIndex(ev.attacker.index).MarkHit(ev.defender);
        if ((attMask & kFsmBit) != 0) fsmPool.DirectAtEntityIndex(ev.attacker.index).PromoteCancelWindowOnHit();

        MotionStack2D* const attMotion = ((attMask & kMotionBit) != 0)
            ? &motionPool.DirectAtEntityIndex(ev.attacker.index)
            : nullptr;

        const u16 traumaMagQ16 = static_cast<u16>(std::min<u32>(65535u, static_cast<u32>(ev.hitstopTicks) * 5400u));
        accumCameraTrauma += static_cast<f32>(traumaMagQ16) * (1.0f / 65535.0f);

        auto emitHitCues = [&](u64 audioAssetId, u16 audioMag, u16 tag) noexcept {
            pipeline.EmitCue({assets::AssetId{audioAssetId}, ev.contactX, ev.contactY, ev.attacker,
                              audioMag, ev.hitstopTicks, SimCueKind::AudioOneShot, 0, tag});
            pipeline.EmitCue({assets::AssetId{0ULL}, ev.contactX, ev.contactY, ev.attacker,
                              traumaMagQ16, ev.hitstopTicks, SimCueKind::CameraTraumaPulse, 0, tag});
        };

        if (ev.isClashParry != 0) {
            const world::ComponentMask defMask = world.GetComponentMaskByIndex(ev.defender.index);
            if ((defMask & kRigBit) != 0) rigPool.DirectAtEntityIndex(ev.defender.index).MarkHit(ev.attacker);
            if ((defMask & kFsmBit) != 0) fsmPool.DirectAtEntityIndex(ev.defender.index).PromoteCancelWindowOnHit();

            const f32 clashSpeed = std::max(6.0f, static_cast<f32>(ev.knockbackSpeed_q2) * 0.25f);
            if (attMotion != nullptr) {
                attMotion->PushOverride(60u, -static_cast<f32>(attMotion->FacingSign()) * clashSpeed, 0.0f, 10u, 256u);
            }
            emitHitCues(1ULL, 65535u, 1u);
            continue;
        }

        const u16 audioMagQ16 = static_cast<u16>(std::min<u32>(65535u, static_cast<u32>(ev.damageMultiplier_q8) * 192u));
        emitHitCues(2ULL, audioMagQ16, ev.socketFlags);

        const f32 rad = static_cast<f32>(ev.knockbackAngleDeg) * (3.1415926535f / 180.0f);
        f32 cosA = 1.0f, sinA = 0.0f;
        FastSinCos(rad, cosA, sinA);

        const f32 facingSign = attMotion != nullptr ? static_cast<f32>(attMotion->FacingSign()) : 1.0f;
        accumKickVx += cosA * facingSign * 1.8f;
        accumKickVy += sinA * 1.8f;

        if ((ev.socketFlags & 0x01u) != 0 && ev.knockbackSpeed_q2 > 0 && attMotion != nullptr) {
            const f32 speed = static_cast<f32>(ev.knockbackSpeed_q2) * 0.25f;
            const f32 recoilVx = (std::abs(cosA) > 1e-4f) ? (-cosA * speed * facingSign) : attMotion->targetWalkVx;
            attMotion->PushOverride(50u, recoilVx, sinA * speed, 8u, 256u);
            attMotion->groundCoyoteTicks = 0;
        }
    }

    if (accumCameraTrauma > 0.0f || accumKickVx != 0.0f || accumKickVy != 0.0f) {
        for (CameraController2D& cam : world.GetPool<CameraController2D>().Components()) {
            cam.AddTrauma(accumCameraTrauma);
            cam.AddDirectionalKick(accumKickVx, accumKickVy);
        }
    }

    if (policies.resolveCombatHits != nullptr) {
        policies.resolveCombatHits(world, step, hits, pipeline);
    }
}

} // namespace en::gameplay
