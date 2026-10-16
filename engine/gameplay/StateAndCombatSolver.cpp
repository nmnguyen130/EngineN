#include "gameplay/StateAndCombatSolver.h"

#include "assets/AssetBank.h"
#include "gameplay/GameplayPipeline.h"
#include "input/ActionBuffer.h"
#include "world/World.h"

#include <algorithm>
#include <cmath>

namespace en::gameplay {

namespace {

void ExecuteFrameDirectivesForRange(
    world::EntityId entity,
    const CookedAnimRigHeader* rigHdr,
    const u8* blobBase,
    u8 clipId,
    u16 fromFrameInclusive,
    u16 toFrameInclusive,
    StateMachine2D& fsm,
    MotionStack2D* optMotion,
    HitboxRig2D* optRig,
    const world::WorldTransform2D& worldTf,
    GameplayPipeline& pipeline) noexcept {

    if (fromFrameInclusive > toFrameInclusive) {
        return;
    }

    const auto* directives   = reinterpret_cast<const CookedFrameDirective*>(blobBase + rigHdr->directiveOffset);
    const auto* hurtProfiles = reinterpret_cast<const CompactAabb2D*>(blobBase + rigHdr->hurtboxProfileOffset);
    const auto* slices       = reinterpret_cast<const CookedHitboxSlice*>(blobBase + rigHdr->hitboxSliceOffset);

    const i8 facingSign = optMotion != nullptr ? optMotion->FacingSign() : i8{1};

    for (u16 d = 0; d < rigHdr->directiveCount; ++d) {
        const CookedFrameDirective& dir = directives[d];
        if (dir.clipId != clipId) {
            continue;
        }
        const u16 f = dir.frameIndex;
        if (f < fromFrameInclusive || f > toFrameInclusive) {
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
                if (optRig != nullptr && dir.paramU8 < rigHdr->hurtboxProfileCount) {
                    fsm.hurtboxProfileId = dir.paramU8;
                    optRig->hurtbox = hurtProfiles[dir.paramU8];
                }
                break;
            case FrameDirectiveKind::ActivateHitboxSlice:
                if (optRig != nullptr && dir.paramU8 < rigHdr->hitboxSliceCount) {
                    const CookedHitboxSlice& sl = slices[dir.paramU8];
                    const bool newSwing = (dir.paramU16 != 0);
                    optRig->ActivateSlice(dir.paramU8, sl.boundingUnion, newSwing);
                }
                break;
            case FrameDirectiveKind::DeactivateHitbox:
                if (optRig != nullptr) {
                    optRig->DeactivateHitbox();
                }
                break;
            case FrameDirectiveKind::ApplyRootMotionDelta:
                if (optMotion != nullptr) {
                    const i16 dx = static_cast<i16>(static_cast<i32>(dir.paramI16) * facingSign);
                    const i16 dy = static_cast<i16>(dir.paramU16);
                    optMotion->rootMotionDx_q8 = static_cast<i16>(optMotion->rootMotionDx_q8 + dx);
                    optMotion->rootMotionDy_q8 = static_cast<i16>(optMotion->rootMotionDy_q8 + dy);
                }
                break;
            case FrameDirectiveKind::ApplyVelocityImpulse:
                if (optMotion != nullptr) {
                    const f32 vx = static_cast<f32>(dir.paramI16) * (1.0f / 256.0f) * static_cast<f32>(facingSign);
                    const f32 vy = static_cast<f32>(static_cast<i16>(dir.paramU16)) * (1.0f / 256.0f);
                    optMotion->impulseVx += vx;
                    optMotion->impulseVy += vy;
                }
                break;
            case FrameDirectiveKind::EmitSimCue:
                pipeline.EmitCue(SimCue{
                    .assetId       = assets::AssetId{static_cast<u64>(dir.paramU16)},
                    .worldX        = worldTf.x,
                    .worldY        = worldTf.y,
                    .emitter       = entity,
                    .magnitude_q16 = 65535u,
                    .durationTicks = 0,
                    .kind          = static_cast<SimCueKind>(dir.paramU8),
                    .channel       = 0,
                    .userTag       = static_cast<u16>(dir.paramI16),
                });
                break;
        }
    }
}

} // namespace

void AnimationSubsystem::StepAnimationsAndDirectives(
    world::World& world,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    GameplayPipeline& pipeline) noexcept {

    alignas(16) std::array<u16, 8> chSubTicks{};
    u8 activeChannelMask = 0;
    for (u8 c = 0; c < 8; ++c) {
        const auto ch = static_cast<time::TimeChannel>(c);
        if (!step.IsFrozen(ch)) {
            activeChannelMask |= static_cast<u8>(1u << c);
            chSubTicks[c] = static_cast<u16>(
                std::lround(step.EffectiveScale(ch) * static_cast<f32>(world::SpriteAnimator2D::kSubTicksPerTick))
            );
        }
    }
    if (activeChannelMask == 0) {
        return;
    }

    auto& spritePool         = world.GetPool<world::Sprite2D>();
    auto& animPool           = world.GetPool<world::SpriteAnimator2D>();
    const u16 groupCount     = spritePool.GroupSize();
    const auto groupEntities = animPool.Entities();
    auto sprites             = spritePool.Components();
    auto anims               = animPool.Components();
    const auto* __restrict channels = world.TimeChannels().data();

    for (u16 i = 0; i < groupCount; ++i) {
        world::SpriteAnimator2D& anim = anims[i];
        if (!anim.playing || anim.frameCount <= 1) {
            continue;
        }
        const u16 entityIdx = groupEntities[i].index;
        const u16 deltaSub  = chSubTicks[static_cast<u8>(channels[entityIdx])];
        if (deltaSub == 0) {
            continue;
        }

        const u32 targetSubTicks = static_cast<u32>(std::max<u16>(1u, anim.ticksPerFrame)) *
                                   world::SpriteAnimator2D::kSubTicksPerTick;
        u32 accum = static_cast<u32>(anim.subTickAccum) + deltaSub;

        if (accum >= targetSubTicks) {
            const u32 framesAdvanced = accum / targetSubTicks;
            accum %= targetSubTicks;
            const u32 nextFrame = static_cast<u32>(anim.currentFrame) + framesAdvanced;
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
            sprites[i].sprite = assetBank.GetSpriteHandle(
                anim.atlas,
                static_cast<u16>(anim.firstFrame + anim.currentFrame)
            );
        }
        anim.subTickAccum = static_cast<u16>(accum);
    }

    const auto* __restrict worldTransforms = world.WorldTransforms().data();
    for (auto [entity, fsm, anim] : world.View<StateMachine2D, world::SpriteAnimator2D>()) {
        if ((activeChannelMask & (1u << static_cast<u8>(channels[entity.index]))) == 0) {
            continue;
        }
        const u16 currFrame = anim.currentFrame;
        const u16 prevFrame = fsm.lastAnimFrame;
        if (currFrame == prevFrame) {
            continue;
        }

        const assets::BlobRecord& blob = assetBank.GetBlob(fsm.rigBlob);
        if (blob.data != nullptr && blob.size >= sizeof(CookedAnimRigHeader)) {
            const auto* rigHdr = reinterpret_cast<const CookedAnimRigHeader*>(blob.data);
            if (rigHdr->magic == CookedAnimRigHeader::kMagic) {
                auto* optMotion = world.TryGet<MotionStack2D>(entity);
                auto* optRig    = world.TryGet<HitboxRig2D>(entity);
                const auto& wtf = worldTransforms[entity.index];

                if (prevFrame == 0xFFFFu) {
                    ExecuteFrameDirectivesForRange(
                        entity, rigHdr, blob.data, fsm.animClipId,
                        0u, currFrame, fsm, optMotion, optRig, wtf, pipeline);
                } else if (currFrame > prevFrame) {
                    ExecuteFrameDirectivesForRange(
                        entity, rigHdr, blob.data, fsm.animClipId,
                        static_cast<u16>(prevFrame + 1u), currFrame,
                        fsm, optMotion, optRig, wtf, pipeline);
                } else if (anim.frameCount > 0) {
                    const u16 lastValidFrame = static_cast<u16>(anim.frameCount - 1u);
                    if (prevFrame < lastValidFrame) {
                        ExecuteFrameDirectivesForRange(
                            entity, rigHdr, blob.data, fsm.animClipId,
                            static_cast<u16>(prevFrame + 1u), lastValidFrame,
                            fsm, optMotion, optRig, wtf, pipeline);
                    }
                    ExecuteFrameDirectivesForRange(
                        entity, rigHdr, blob.data, fsm.animClipId,
                        0u, currFrame, fsm, optMotion, optRig, wtf, pipeline);
                }
            }
        }
        fsm.lastAnimFrame = currFrame;
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

    const auto* __restrict channels = world.TimeChannels().data();

    for (auto [entity, dmg] : world.View<DamageReceiver2D>()) {
        if (step.IsFrozen(channels[entity.index])) {
            continue;
        }
        if (dmg.iframeTicks > 0) --dmg.iframeTicks;
        if (dmg.stunTicks > 0)   --dmg.stunTicks;
    }

    for (auto [entity, motor, motion, fsm] : world.View<KinematicMotor2D, MotionStack2D, StateMachine2D>()) {
        if (step.IsFrozen(motor.timeChannel)) {
            continue;
        }

        if (fsm.stateTicks < 0xFFFFu) {
            ++fsm.stateTicks;
        }
        if (motion.overrideTicks > 0) {
            if (--motion.overrideTicks == 0) {
                motion.activePriority = 0;
                motion.gravityScale_q8 = 256;
            }
        }
        if (!HasContact(motor.contacts, ContactFlags::Ground) && motion.groundCoyoteTicks > 0) {
            --motion.groundCoyoteTicks;
        }
        if (!HasContact(motor.contacts, ContactFlags::WallLeft | ContactFlags::WallRight) &&
            motion.wallCoyoteTicks > 0) {
            --motion.wallCoyoteTicks;
        }

        if (policies.evaluateActor != nullptr) {
            auto* optRig = world.TryGet<HitboxRig2D>(entity);
            const ActorPolicyContext ctx{
                .world        = world,
                .assetBank    = assetBank,
                .step         = step,
                .tickInput    = tickInput,
                .actionBuffer = actionBuffer,
                .pipeline     = pipeline,
                .entity       = entity,
            };
            policies.evaluateActor(ctx, motor, motion, fsm, optRig);
        }

        if (!motion.IsFacingLocked() && fsm.CanCancelInto(CancelCategory::MovementTurn)) {
            if (motion.targetWalkVx > 0.15f) {
                motion.SetFacingSign(1);
            } else if (motion.targetWalkVx < -0.15f) {
                motion.SetFacingSign(-1);
            }
        }

        if (auto* sp = world.TryGet<world::Sprite2D>(entity)) {
            if (motion.FacingSign() < 0) {
                sp->blendAndFlags |= static_cast<u8>(world::SpriteFlags::FlipX);
            } else {
                sp->blendAndFlags &= ~static_cast<u8>(world::SpriteFlags::FlipX);
            }
        }
    }
}

void StateAndCombatSolver::ResolveCombatHits(
    world::World& world,
    time::SimStepView step,
    std::span<const ResolvedHitEvent> hits,
    const GameplayPolicyTable& policies,
    GameplayPipeline& pipeline) noexcept {

    for (const ResolvedHitEvent& ev : hits) {
        if (auto* attRig = world.TryGet<HitboxRig2D>(ev.attacker)) {
            attRig->MarkHit(ev.defender);
        }
        if (auto* attFsm = world.TryGet<StateMachine2D>(ev.attacker)) {
            attFsm->PromoteCancelWindowOnHit();
        }

        const u16 traumaMagQ16 = static_cast<u16>(std::min<u32>(65535u, static_cast<u32>(ev.hitstopTicks) * 5400u));

        // 1. Weapon-vs-Weapon Clash / Parry arbitration & cue emission
        if (ev.isClashParry != 0) {
            if (auto* defRig = world.TryGet<HitboxRig2D>(ev.defender)) {
                defRig->MarkHit(ev.attacker);
            }
            if (auto* defFsm = world.TryGet<StateMachine2D>(ev.defender)) {
                defFsm->PromoteCancelWindowOnHit();
            }
            const f32 clashSpeed = std::max(6.0f, static_cast<f32>(ev.knockbackSpeed_q2) * 0.25f);
            if (auto* attMotion = world.TryGet<MotionStack2D>(ev.attacker)) {
                attMotion->PushOverride(60u, -static_cast<f32>(attMotion->FacingSign()) * clashSpeed, 0.0f, 10u, 256u);
            }

            pipeline.EmitCue(SimCue{
                .assetId       = assets::AssetId{1ULL},
                .worldX        = ev.contactX,
                .worldY        = ev.contactY,
                .emitter       = ev.attacker,
                .magnitude_q16 = 65535u,
                .durationTicks = ev.hitstopTicks,
                .kind          = SimCueKind::AudioOneShot,
                .channel       = 0,
                .userTag       = 1u,
            });
            pipeline.EmitCue(SimCue{
                .assetId       = assets::AssetId{0ULL},
                .worldX        = ev.contactX,
                .worldY        = ev.contactY,
                .emitter       = ev.attacker,
                .magnitude_q16 = traumaMagQ16,
                .durationTicks = ev.hitstopTicks,
                .kind          = SimCueKind::CameraTraumaPulse,
                .channel       = 0,
                .userTag       = 1u,
            });
            continue;
        }

        // 2. Normal Combat Hit: Emit AudioOneShot & CameraTraumaPulse SimCues + apply CameraController2D kick/trauma
        pipeline.EmitCue(SimCue{
            .assetId       = assets::AssetId{2ULL},
            .worldX        = ev.contactX,
            .worldY        = ev.contactY,
            .emitter       = ev.attacker,
            .magnitude_q16 = static_cast<u16>(std::min<u32>(65535u, static_cast<u32>(ev.damageMultiplier_q8) * 192u)),
            .durationTicks = ev.hitstopTicks,
            .kind          = SimCueKind::AudioOneShot,
            .channel       = 0,
            .userTag       = ev.socketFlags,
        });
        pipeline.EmitCue(SimCue{
            .assetId       = assets::AssetId{0ULL},
            .worldX        = ev.contactX,
            .worldY        = ev.contactY,
            .emitter       = ev.attacker,
            .magnitude_q16 = traumaMagQ16,
            .durationTicks = ev.hitstopTicks,
            .kind          = SimCueKind::CameraTraumaPulse,
            .channel       = 0,
            .userTag       = ev.socketFlags,
        });

        // Apply directional hit-kick and trauma directly onto active CameraController2D instances
        const f32 rad = static_cast<f32>(ev.knockbackAngleDeg) * (3.1415926535f / 180.0f);
        f32 cosA = 1.0f;
        f32 sinA = 0.0f;
        world::FastSinCos(rad, cosA, sinA);

        auto* attMotion = world.TryGet<MotionStack2D>(ev.attacker);
        const f32 facingSign = attMotion != nullptr ? static_cast<f32>(attMotion->FacingSign()) : 1.0f;

        for (auto [camId, cam] : world.View<CameraController2D>()) {
            cam.AddTrauma(static_cast<f32>(traumaMagQ16) * (1.0f / 65535.0f));
            cam.AddDirectionalKick(cosA * facingSign * 1.8f, sinA * 1.8f);
        }

        // 3. Data-driven Attacker Recoil / Pogo Impulse
        if ((ev.socketFlags & 0x01u) != 0 && ev.knockbackSpeed_q2 > 0 && attMotion != nullptr) {
            const f32 speed = static_cast<f32>(ev.knockbackSpeed_q2) * 0.25f;
            const f32 recoilVx = (std::abs(cosA) > 1e-4f)
                ? (-cosA * speed * facingSign)
                : attMotion->targetWalkVx;
            const f32 recoilVy = sinA * speed;

            attMotion->PushOverride(50u, recoilVx, recoilVy, 8u, 256u);
            attMotion->groundCoyoteTicks = 0;
        }
    }

    if (policies.resolveCombatHits != nullptr) {
        policies.resolveCombatHits(world, step, hits, pipeline);
    }
}

} // namespace en::gameplay
