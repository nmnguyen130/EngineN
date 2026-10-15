#include "gameplay/KinematicSolver2D.h"

#include "world/World.h"

#include <algorithm>
#include <cmath>

namespace en::gameplay {

void KinematicSolver2D::StepCarriersAndRiders(
    world::World& world,
    const SpatialIndex2D& spatial,
    time::SimStepView step) noexcept {
    if (step.IsFrozen(time::TimeChannel::World)) {
        return;
    }

    const KinematicStepConfig defaultCfg{};

    for (auto [riderId, motor] : world.View<KinematicMotor2D>()) {
        if (motor.carrierId == world::kNullEntity || !world.IsAlive(motor.carrierId)) {
            motor.carrierId = world::kNullEntity;
            continue;
        }

        const auto* carrier = world.TryGet<KinematicCarrier2D>(motor.carrierId);
        if (carrier == nullptr) {
            motor.carrierId = world::kNullEntity;
            continue;
        }

        if (std::abs(carrier->deltaX) < 1e-6f && std::abs(carrier->deltaY) < 1e-6f) {
            continue;
        }

        world::LocalTransform2D local = world.GetLocalTransform(riderId);
        const ContactFlags savedContacts = motor.contacts;
        ResolveSingleMotorSweep(
            riderId, local, motor, spatial, carrier->deltaX, carrier->deltaY, defaultCfg);
        motor.contacts |= savedContacts;
        world.SetLocalTransform(riderId, local);
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
    const bool startedGrounded = HasContact(inoutMotor.contacts, ContactFlags::Ground);
    inoutMotor.contacts = ContactFlags::None;

    const f32 offX  = inoutMotor.bounds.OffsetX();
    const f32 offY  = inoutMotor.bounds.OffsetY();
    const f32 halfW = inoutMotor.bounds.HalfW();
    const f32 halfH = inoutMotor.bounds.HalfH();
    const bool ignoreOneWay = (inoutMotor.oneWayIgnoreTicks > 0) ||
                              HasMotorFlag(inoutMotor.flags, MotorFlags::Crushed);

    f32 cx = inoutTransform.x + offX;
    f32 cy = inoutTransform.y + offY;
    f32 remDx = deltaX;
    f32 remDy = deltaY;

    f32 prevNormalX = 0.0f;
    f32 prevNormalY = 0.0f;
    bool hasPrevNormal = false;

    for (u32 iter = 0; iter < 3u; ++iter) {
        if (std::abs(remDx) < 1e-6f && std::abs(remDy) < 1e-6f) {
            break;
        }

        const SlabImpact2D hit = spatial.SweepAabb(
            id, cx, cy, halfW, halfH, remDx, remDy, inoutMotor.solidMask, ignoreOneWay);

        if (!hit.hit) {
            cx += remDx;
            cy += remDy;
            break;
        }

        if (hit.normalY < -0.7f && remDy > 0.0f &&
            HasMotorFlag(inoutMotor.flags, MotorFlags::CornerCorrection)) {
            const f32 advanceX = remDx * hit.toi;
            const f32 advanceY = remDy * hit.toi;
            const f32 testX = cx + advanceX;
            const f32 testY = cy + advanceY;
            const f32 remUpY = remDy * (1.0f - hit.toi);

            bool corrected = false;
            for (f32 sign : {-1.0f, 1.0f}) {
                const f32 nudgeX = sign * cfg.cornerCorrectionMax;
                const SlabImpact2D sideCheck = spatial.SweepAabb(
                    id, testX, testY, halfW, halfH, nudgeX, 0.0f, inoutMotor.solidMask, true);
                if (!sideCheck.hit) {
                    const SlabImpact2D upCheck = spatial.SweepAabb(
                        id, testX + nudgeX, testY, halfW, halfH, 0.0f, remUpY, inoutMotor.solidMask, true);
                    if (!upCheck.hit) {
                        cx = testX + nudgeX;
                        cy = testY + remUpY;
                        remDx = 0.0f;
                        remDy = 0.0f;
                        corrected = true;
                        break;
                    }
                }
            }
            if (corrected) {
                break;
            }
        }

        cx += remDx * hit.toi + hit.normalX * cfg.skinWidth;
        cy += remDy * hit.toi + hit.normalY * cfg.skinWidth;

        const f32 remFrac = 1.0f - hit.toi;
        remDx *= remFrac;
        remDy *= remFrac;

        if (hit.normalY > 0.5f) {
            inoutMotor.contacts |= ContactFlags::Ground;
            inoutMotor.SetSlopeNormal(hit.normalX, hit.normalY);
            inoutMotor.carrierId = hit.carrierId;
            if (hit.tileShape == TileShape::OneWayTop) {
                inoutMotor.contacts |= ContactFlags::OnOneWay;
            }
            if (std::abs(hit.normalX) > 0.1f) {
                inoutMotor.contacts |= ContactFlags::OnSlope;
            }
            if (inoutMotor.vy < 0.0f) {
                inoutMotor.vy = 0.0f;
            }
        } else if (hit.normalY < -0.5f) {
            inoutMotor.contacts |= ContactFlags::Ceiling;
            if (inoutMotor.vy > 0.0f) {
                inoutMotor.vy = 0.0f;
            }
        } else if (hit.normalX > 0.5f) {
            inoutMotor.contacts |= ContactFlags::WallLeft;
            if (inoutMotor.vx < 0.0f) {
                inoutMotor.vx = 0.0f;
            }
        } else if (hit.normalX < -0.5f) {
            inoutMotor.contacts |= ContactFlags::WallRight;
            if (inoutMotor.vx > 0.0f) {
                inoutMotor.vx = 0.0f;
            }
        }

        // Prevent infinite oscillation inside acute slope-to-wall creases.
        if (hasPrevNormal && (prevNormalX * hit.normalX + prevNormalY * hit.normalY) < -0.1f) {
            remDx = 0.0f;
            remDy = 0.0f;
            break;
        }
        prevNormalX = hit.normalX;
        prevNormalY = hit.normalY;
        hasPrevNormal = true;

        if (HasContact(inoutMotor.contacts, ContactFlags::OnSlope) &&
            HasMotorFlag(inoutMotor.flags, MotorFlags::ProjectSlopes)) {
            const f32 tangentX = hit.normalY;
            const f32 tangentY = -hit.normalX;
            const f32 sign = (remDx >= 0.0f) ? 1.0f : -1.0f;
            const f32 mag = std::abs(remDx);
            remDx = std::abs(tangentX) * sign * mag;
            remDy = (sign * tangentX >= 0.0f ? tangentY : -tangentY) * mag;
        } else {
            const f32 dot = remDx * hit.normalX + remDy * hit.normalY;
            if (dot < 0.0f) {
                remDx -= dot * hit.normalX;
                remDy -= dot * hit.normalY;
            }
        }
    }

    if (startedGrounded &&
        !HasContact(inoutMotor.contacts, ContactFlags::Ground) &&
        inoutMotor.vy <= 0.0f &&
        HasMotorFlag(inoutMotor.flags, MotorFlags::SnapToGround)) {
        const f32 snapDist = std::min(cfg.groundSnapMaxDist, std::abs(deltaX) * 1.2f + 0.04f);
        const SlabImpact2D snapHit = spatial.SweepAabb(
            id, cx, cy, halfW, halfH, 0.0f, -snapDist, inoutMotor.solidMask, ignoreOneWay);
        if (snapHit.hit && snapHit.normalY > 0.5f) {
            cy -= snapDist * snapHit.toi - cfg.skinWidth;
            inoutMotor.contacts |= ContactFlags::Ground;
            inoutMotor.SetSlopeNormal(snapHit.normalX, snapHit.normalY);
            inoutMotor.carrierId = snapHit.carrierId;
            inoutMotor.vy = 0.0f;
        } else {
            inoutMotor.carrierId = world::kNullEntity;
        }
    } else if (!HasContact(inoutMotor.contacts, ContactFlags::Ground)) {
        inoutMotor.carrierId = world::kNullEntity;
        inoutMotor.SetSlopeNormal(0.0f, 1.0f);
    }

    if (!HasContact(inoutMotor.contacts, ContactFlags::Ground) &&
        HasMotorFlag(inoutMotor.flags, MotorFlags::DetectLedges)) {
        if (HasContact(inoutMotor.contacts, ContactFlags::WallRight)) {
            if (spatial.DetectLedgeCorner(cx + halfW + 0.08f, cy + halfH, cx)) {
                inoutMotor.contacts |= ContactFlags::LedgeGrabRight;
            }
        }
        if (HasContact(inoutMotor.contacts, ContactFlags::WallLeft)) {
            if (spatial.DetectLedgeCorner(cx - halfW - 0.08f, cy + halfH, cx)) {
                inoutMotor.contacts |= ContactFlags::LedgeGrabLeft;
            }
        }
    }

    inoutTransform.x = cx - offX;
    inoutTransform.y = cy - offY;
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

        world::LocalTransform2D local = world.GetLocalTransform(id);
        const f32 cx = local.x + motor.bounds.OffsetX();
        const f32 cy = local.y + motor.bounds.OffsetY();
        const f32 hw = motor.bounds.HalfW();
        const f32 hh = motor.bounds.HalfH();

        const ZoneSample2D zone = spatial.SampleEnvironmentalZones(cx, cy);
        if (zone.submerged) {
            motor.flags |= MotorFlags::Submerged;
        } else {
            motor.flags &= ~MotorFlags::Submerged;
        }

        f32 sepVx = 0.0f;
        if (HasMotorFlag(motor.flags, MotorFlags::SoftPushbox) && motor.pushboxGroup != 0) {
            spatial.ComputeSoftPushboxSeparation(id, motor.pushboxGroup, cx, cy, hw, hh, sepVx);
        }

        f32 rootDx = 0.0f;
        f32 rootDy = 0.0f;

        if (auto* motion = world.TryGet<MotionStack2D>(id)) {
            if (motion->overrideTicks > 0) {
                --motion->overrideTicks;
                motor.vx = motion->overrideVx;
                motor.vy = motion->overrideVy;
                if (motion->overrideTicks == 0) {
                    motion->activePriority = 0;
                    motion->gravityScale_q8 = 256;
                }
            } else {
                motor.vx = motion->targetWalkVx;
                const f32 gravScale = static_cast<f32>(motion->gravityScale_q8) * (1.0f / 256.0f);
                // Suppress slope gravity drift while standing still on an angled floor.
                const bool idleOnSlope = HasContact(motor.contacts, ContactFlags::Ground) &&
                                         HasContact(motor.contacts, ContactFlags::OnSlope) &&
                                         std::abs(motion->targetWalkVx) < 1e-4f;
                if (!idleOnSlope) {
                    motor.vy = std::max(cfg.terminalFallVy, motor.vy + cfg.baseGravity * gravScale * dt);
                } else {
                    motor.vy = 0.0f;
                }
            }

            if ((HasContact(motor.contacts, ContactFlags::WallLeft) ||
                 HasContact(motor.contacts, ContactFlags::WallRight)) &&
                !HasContact(motor.contacts, ContactFlags::Ground) &&
                motor.vy < cfg.wallSlideMaxVy) {
                motor.vy = cfg.wallSlideMaxVy;
            }

            motor.vx = (motor.vx + motion->impulseVx + zone.forceVx + sepVx) * zone.dragScale;
            motor.vy = (motor.vy + motion->impulseVy + zone.forceVy) * zone.dragScale;

            const f32 decay = motion->IsCarrierLaunchActive() ? 0.992f : 0.88f;
            motion->impulseVx *= decay;
            motion->impulseVy *= 0.88f;
            if (std::abs(motion->impulseVx) < 0.01f) motion->impulseVx = 0.0f;
            if (std::abs(motion->impulseVy) < 0.01f) motion->impulseVy = 0.0f;

            rootDx = static_cast<f32>(motion->rootMotionDx_q8) * (1.0f / 256.0f) *
                     static_cast<f32>(motion->FacingSign());
            rootDy = static_cast<f32>(motion->rootMotionDy_q8) * (1.0f / 256.0f);
            motion->rootMotionDx_q8 = 0;
            motion->rootMotionDy_q8 = 0;
        } else {
            motor.vy = std::max(cfg.terminalFallVy, motor.vy + cfg.baseGravity * dt);
            motor.vx = (motor.vx + zone.forceVx + sepVx) * zone.dragScale;
            motor.vy = (motor.vy + zone.forceVy) * zone.dragScale;
        }

        const world::EntityId prevCarrier = motor.carrierId;
        const f32 stepDx = motor.vx * dt + rootDx;
        const f32 stepDy = motor.vy * dt + rootDy;

        ResolveSingleMotorSweep(id, local, motor, spatial, stepDx, stepDy, cfg);
        world.SetLocalTransform(id, local);

        if (auto* motion = world.TryGet<MotionStack2D>(id)) {
            if (HasContact(motor.contacts, ContactFlags::Ground)) {
                motion->groundCoyoteTicks = cfg.maxCoyoteTicks;
                motion->SetCarrierLaunchActive(false);
            } else if (motion->groundCoyoteTicks > 0) {
                --motion->groundCoyoteTicks;
            }

            if (HasContact(motor.contacts, ContactFlags::WallLeft)) {
                motion->wallCoyoteTicks = cfg.maxWallCoyoteTicks;
                motion->SetWallSign(-1);
            } else if (HasContact(motor.contacts, ContactFlags::WallRight)) {
                motion->wallCoyoteTicks = cfg.maxWallCoyoteTicks;
                motion->SetWallSign(1);
            } else if (motion->wallCoyoteTicks > 0) {
                --motion->wallCoyoteTicks;
            } else {
                motion->SetWallSign(0);
            }

            // Inherit carrier momentum when jumping or detaching off a moving platform.
            if (prevCarrier != world::kNullEntity && motor.carrierId == world::kNullEntity) {
                if (const auto* carrier = world.TryGet<KinematicCarrier2D>(prevCarrier)) {
                    const f32 carrierVx = carrier->deltaX / dt;
                    const f32 carrierVy = carrier->deltaY / dt;
                    motion->impulseVx += carrierVx;
                    if (motor.vy > 0.0f && carrierVy > 0.0f) {
                        motor.vy += carrierVy * 0.85f;
                    }
                    motion->SetCarrierLaunchActive(true);
                }
            }
        }

        if (HasContact(motor.contacts, ContactFlags::Ground) &&
            !HasContact(motor.contacts, ContactFlags::OnOneWay) &&
            motor.carrierId == world::kNullEntity &&
            (step.TickIndex() & 15ULL) == 0ULL) {
            if (auto* crumb = world.TryGet<SafeGroundBreadcrumb2D>(id)) {
                const f32 footY = (local.y + motor.bounds.OffsetY()) - hh - 0.05f;
                const f32 newCx = local.x + motor.bounds.OffsetX();
                if (spatial.CheckStaticPointSolid(newCx - hw * 0.8f, footY, true) &&
                    spatial.CheckStaticPointSolid(newCx, footY, true) &&
                    spatial.CheckStaticPointSolid(newCx + hw * 0.8f, footY, true)) {
                    crumb->prevSafeX = crumb->safeX;
                    crumb->prevSafeY = crumb->safeY;
                    crumb->safeX = local.x;
                    crumb->safeY = local.y;
                }
            }
        }
    }
}

} // namespace en::gameplay
