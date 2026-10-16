#include "gameplay/KinematicSolver2D.h"

#include "world/World.h"

#include <algorithm>
#include <cmath>

namespace en::gameplay {

void KinematicSolver2D::StepCarriersAndRiders(
    world::World& world,
    const SpatialIndex2D& spatial,
    time::SimStepView step) noexcept {
    for (auto [riderId, motor] : world.View<KinematicMotor2D>()) {
        if (motor.carrierId == world::kNullEntity || step.IsFrozen(motor.timeChannel)) {
            continue;
        }
        const auto* carrier = world.TryGet<KinematicCarrier2D>(motor.carrierId);
        if (carrier == nullptr) {
            motor.carrierId = world::kNullEntity;
            continue;
        }
        if (carrier->deltaX == 0.0f && carrier->deltaY == 0.0f) {
            continue;
        }
        world::LocalTransform2D& loc = world.ModifyLocalTransformByIndex(riderId.index);
        ResolveSingleMotorSweep(
            riderId, loc, motor, spatial, carrier->deltaX, carrier->deltaY, KinematicStepConfig{});
    }
}

void KinematicSolver2D::StepMotorsAndMotionStacks(
    world::World& world,
    const SpatialIndex2D& spatial,
    time::SimStepView step,
    const KinematicStepConfig& cfg) noexcept {

    for (auto [id, motor] : world.View<KinematicMotor2D>()) {
        if (step.IsFrozen(motor.timeChannel)) {
            continue;
        }
        const f32 dt = step.Dt(motor.timeChannel);
        if (dt <= 0.0f) {
            continue;
        }

        if (motor.oneWayIgnoreTicks > 0) {
            --motor.oneWayIgnoreTicks;
        }

        world::LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
        const f32 cx = loc.x + motor.bounds.OffsetX();
        const f32 cy = loc.y + motor.bounds.OffsetY();
        const f32 hw = motor.bounds.HalfW();
        const f32 hh = motor.bounds.HalfH();

        const ZoneSample2D zone = spatial.SampleEnvironmentalZones(cx, cy);
        if (zone.submerged) {
            motor.flags |= MotorFlags::Submerged;
        } else {
            motor.flags &= ~MotorFlags::Submerged;
        }

        f32 rootDx = 0.0f;
        f32 rootDy = 0.0f;
        const world::EntityId prevCarrierId = motor.carrierId;

        if (auto* motion = world.TryGet<MotionStack2D>(id)) {
            if (prevCarrierId != world::kNullEntity && motion->IsCarrierLaunchActive()) {
                if (const auto* carrier = world.TryGet<KinematicCarrier2D>(prevCarrierId)) {
                    motion->impulseVx += carrier->deltaX / dt;
                    motion->impulseVy += std::max(0.0f, carrier->deltaY / dt);
                }
                motion->SetCarrierLaunchActive(false);
            }

            const f32 gravScale = static_cast<f32>(motion->gravityScale_q8) * (1.0f / 256.0f);
            if (motion->overrideTicks > 0) {
                motor.vx = motion->overrideVx;
                motor.vy = motion->overrideVy + cfg.baseGravity * gravScale * dt;
            } else {
                motor.vx = motion->targetWalkVx;
                motor.vy = std::max(cfg.terminalFallVy, motor.vy + cfg.baseGravity * gravScale * dt);
            }

            motor.vx += motion->impulseVx + zone.forceVx;
            motor.vy += motion->impulseVy + zone.forceVy;
            motor.vx *= zone.dragScale;
            motor.vy *= zone.dragScale;

            motion->impulseVx = 0.0f;
            motion->impulseVy = 0.0f;

            rootDx = static_cast<f32>(motion->rootMotionDx_q8) * (1.0f / 256.0f);
            rootDy = static_cast<f32>(motion->rootMotionDy_q8) * (1.0f / 256.0f);
            motion->rootMotionDx_q8 = 0;
            motion->rootMotionDy_q8 = 0;
        } else {
            motor.vy = std::max(cfg.terminalFallVy, motor.vy + cfg.baseGravity * dt);
        }

        if (HasMotorFlag(motor.flags, MotorFlags::SoftPushbox)) {
            spatial.ComputeSoftPushboxSeparation(id, motor.pushboxGroup, cx, cy, hw, hh, motor.vx);
        }

        f32 deltaX = motor.vx * dt + rootDx;
        f32 deltaY = motor.vy * dt + rootDy;

        // Project horizontal movement along slope tangent so walking uphill/downhill maintains constant speed
        if (HasContact(motor.contacts, ContactFlags::OnSlope) &&
            HasMotorFlag(motor.flags, MotorFlags::ProjectSlopes) &&
            motor.vy <= 0.0f) {
            const f32 snx = motor.SlopeNormalX();
            const f32 sny = motor.SlopeNormalY();
            if (sny > 0.2f) {
                deltaY -= (snx / sny) * deltaX;
            }
        }

        ResolveSingleMotorSweep(id, loc, motor, spatial, deltaX, deltaY, cfg);

        if (auto* motion = world.TryGet<MotionStack2D>(id)) {
            if (HasContact(motor.contacts, ContactFlags::Ground)) {
                motion->groundCoyoteTicks = cfg.maxCoyoteTicks;
                motion->SetWallSign(0);
                motion->SetCarrierLaunchActive(motor.carrierId != world::kNullEntity);
            } else if (HasContact(motor.contacts, ContactFlags::WallLeft)) {
                motion->wallCoyoteTicks = cfg.maxWallCoyoteTicks;
                motion->SetWallSign(-1);
                if (motor.vy < cfg.wallSlideMaxVy) {
                    motor.vy = cfg.wallSlideMaxVy;
                }
            } else if (HasContact(motor.contacts, ContactFlags::WallRight)) {
                motion->wallCoyoteTicks = cfg.maxWallCoyoteTicks;
                motion->SetWallSign(1);
                if (motor.vy < cfg.wallSlideMaxVy) {
                    motor.vy = cfg.wallSlideMaxVy;
                }
            }
        }

        // Update SafeGroundBreadcrumb2D when grounded on verified non-hazard static tiles
        if (HasContact(motor.contacts, ContactFlags::Ground) &&
            motor.carrierId == world::kNullEntity &&
            HasMotorFlag(motor.flags, MotorFlags::TrackSafeAnchor)) {
            if (auto* crumb = world.TryGet<SafeGroundBreadcrumb2D>(id)) {
                const f32 footY = loc.y + motor.bounds.OffsetY() - hh - 0.05f;
                const f32 footX = loc.x + motor.bounds.OffsetX();
                if (spatial.CheckStaticPointSolid(footX - hw * 0.7f, footY, true) &&
                    spatial.CheckStaticPointSolid(footX, footY, true) &&
                    spatial.CheckStaticPointSolid(footX + hw * 0.7f, footY, true)) {
                    if (std::abs(loc.x - crumb->safeX) > 1.5f || std::abs(loc.y - crumb->safeY) > 1.5f) {
                        crumb->prevSafeX = crumb->safeX;
                        crumb->prevSafeY = crumb->safeY;
                        crumb->safeX     = loc.x;
                        crumb->safeY     = loc.y;
                    }
                }
            }
        }
    }
}

void KinematicSolver2D::ResolveSingleMotorSweep(
    world::EntityId id,
    world::LocalTransform2D& inoutTransform,
    KinematicMotor2D& inoutMotor,
    const SpatialIndex2D& spatial,
    f32 deltaX,
    f32 deltaY,
    const KinematicStepConfig& cfg) noexcept {

    const bool wasGrounded    = HasContact(inoutMotor.contacts, ContactFlags::Ground);
    const bool notJumpingUp   = (inoutMotor.vy <= 0.0f);
    inoutMotor.contacts       = ContactFlags::None;
    inoutMotor.carrierId      = world::kNullEntity;

    const f32 offX = inoutMotor.bounds.OffsetX();
    const f32 offY = inoutMotor.bounds.OffsetY();
    const f32 hw   = inoutMotor.bounds.HalfW();
    const f32 hh   = inoutMotor.bounds.HalfH();
    const bool ignoreOneWay = (inoutMotor.oneWayIgnoreTicks > 0);

    bool steppedUp = false;
    f32 stepHeightUsed = 0.0f;

    // 1. Horizontal Sweep (with micro-lip / toe-stub step-up correction)
    if (std::abs(deltaX) > 1e-6f) {
        const f32 cx = inoutTransform.x + offX;
        const f32 cy = inoutTransform.y + offY;
        const SlabImpact2D hitX = spatial.SweepAabb(
            id, cx, cy, hw, hh, deltaX, 0.0f, inoutMotor.solidMask, true);
        if (hitX.hit) {
            if (HasMotorFlag(inoutMotor.flags, MotorFlags::CornerCorrection) &&
                (wasGrounded || notJumpingUp)) {
                const f32 stepHeight = std::min(0.16f, cfg.cornerCorrectionMax);
                const SlabImpact2D retryHigh = spatial.SweepAabb(
                    id, cx, cy + stepHeight, hw, hh, deltaX, 0.0f, inoutMotor.solidMask, true);
                if (!retryHigh.hit) {
                    inoutTransform.x += deltaX;
                    inoutTransform.y += stepHeight;
                    steppedUp      = true;
                    stepHeightUsed = stepHeight;
                }
            }

            if (!steppedUp) {
                inoutTransform.x += deltaX * hitX.toi + hitX.normalX * cfg.skinWidth;
                inoutMotor.vx = 0.0f;
                inoutMotor.contacts |= (hitX.normalX > 0.0f) ? ContactFlags::WallLeft : ContactFlags::WallRight;

                if (HasMotorFlag(inoutMotor.flags, MotorFlags::DetectLedges) && notJumpingUp) {
                    const f32 wallProbeX = (inoutTransform.x + offX) + (hitX.normalX > 0.0f ? -(hw + 0.08f) : (hw + 0.08f));
                    const f32 headTopY   = (inoutTransform.y + offY) + hh;
                    if (spatial.DetectLedgeCorner(wallProbeX, headTopY, inoutTransform.x + offX)) {
                        inoutMotor.contacts |= (hitX.normalX > 0.0f)
                            ? ContactFlags::LedgeGrabLeft
                            : ContactFlags::LedgeGrabRight;
                    }
                }
            }
        } else {
            inoutTransform.x += deltaX;
        }
    }

    // If horizontal step-up raised the actor, ensure vertical sweep settles back onto the step surface
    // even when deltaY == 0 so ContactFlags::Ground is never dropped for 1 frame.
    const f32 effectiveDeltaY = steppedUp ? std::min(deltaY, -(stepHeightUsed + cfg.skinWidth * 2.0f)) : deltaY;

    // 2. Vertical Sweep & Uphill/Downhill Ground Snap
    if (std::abs(effectiveDeltaY) > 1e-6f || (wasGrounded && notJumpingUp)) {
        const f32 cx = inoutTransform.x + offX;
        const f32 cy = inoutTransform.y + offY;
        const SlabImpact2D hitY = (std::abs(effectiveDeltaY) > 1e-6f)
            ? spatial.SweepAabb(id, cx, cy, hw, hh, 0.0f, effectiveDeltaY, inoutMotor.solidMask, ignoreOneWay)
            : SlabImpact2D{};

        if (hitY.hit) {
            bool cornerCorrected = false;
            if (effectiveDeltaY > 0.0f && hitY.normalY < 0.0f &&
                HasMotorFlag(inoutMotor.flags, MotorFlags::CornerCorrection)) {
                for (f32 nudge : {-cfg.cornerCorrectionMax, cfg.cornerCorrectionMax}) {
                    const SlabImpact2D retry = spatial.SweepAabb(
                        id, cx + nudge, cy, hw, hh, 0.0f, effectiveDeltaY, inoutMotor.solidMask, true);
                    if (!retry.hit) {
                        inoutTransform.x += nudge;
                        inoutTransform.y += effectiveDeltaY;
                        cornerCorrected = true;
                        break;
                    }
                }
            }

            if (!cornerCorrected) {
                inoutTransform.y += effectiveDeltaY * hitY.toi + hitY.normalY * cfg.skinWidth;
                inoutMotor.vy = 0.0f;
                if (hitY.normalY > 0.0f) {
                    inoutMotor.contacts |= ContactFlags::Ground;
                    inoutMotor.carrierId = hitY.carrierId;
                    inoutMotor.SetSlopeNormal(hitY.normalX, hitY.normalY);
                    if (std::abs(hitY.normalX) > 0.01f) {
                        inoutMotor.contacts |= ContactFlags::OnSlope;
                    }
                    if (hitY.tileShape == TileShape::OneWayTop) {
                        inoutMotor.contacts |= ContactFlags::OnOneWay;
                    }
                } else {
                    inoutMotor.contacts |= ContactFlags::Ceiling;
                }
            }
        } else {
            inoutTransform.y += effectiveDeltaY;

            // Ground snap when walking uphill/downhill or stepping onto ledges (gated by notJumpingUp, NOT deltaY <= 0)
            if ((wasGrounded || steppedUp) && notJumpingUp && HasMotorFlag(inoutMotor.flags, MotorFlags::SnapToGround)) {
                const SlabImpact2D snapHit = spatial.SweepAabb(
                    id, inoutTransform.x + offX, inoutTransform.y + offY,
                    hw, hh, 0.0f, -cfg.groundSnapMaxDist, inoutMotor.solidMask, ignoreOneWay);
                if (snapHit.hit && snapHit.normalY > 0.5f) {
                    inoutTransform.y += (-cfg.groundSnapMaxDist) * snapHit.toi + cfg.skinWidth;
                    inoutMotor.vy = 0.0f;
                    inoutMotor.contacts |= ContactFlags::Ground;
                    inoutMotor.carrierId = snapHit.carrierId;
                    inoutMotor.SetSlopeNormal(snapHit.normalX, snapHit.normalY);
                    if (std::abs(snapHit.normalX) > 0.01f) {
                        inoutMotor.contacts |= ContactFlags::OnSlope;
                    }
                }
            }
        }
    }
}

} // namespace en::gameplay
