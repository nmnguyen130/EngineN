#include "gameplay/KinematicSolver2D.h"

#include "world/World.h"

#include <algorithm>
#include <cmath>

namespace en::gameplay {

namespace {

constexpr void ApplyGroundContact(KinematicMotor2D& motor, const SlabImpact2D& hit) noexcept {
    motor.vy        = 0.0f;
    motor.contacts |= ContactFlags::Ground;
    motor.carrierId = hit.carrierId;
    motor.SetSlopeNormal(hit.normalX, hit.normalY);
    if (std::abs(hit.normalX) > 0.01f) motor.contacts |= ContactFlags::OnSlope;
    if (hit.tileShape == TileShape::OneWayTop) motor.contacts |= ContactFlags::OnOneWay;
}

} // namespace

void KinematicSolver2D::StepCarriersAndRiders(
    world::World& world,
    const SpatialIndex2D& spatial,
    time::SimStepView step,
    const KinematicStepConfig& cfg) noexcept {

    const u8 frozenMask = step.FrozenMask();
    if (frozenMask == 0xFFu || world.GetPool<KinematicCarrier2D>().Size() == 0) {
        return;
    }

    auto& motorPool      = world.GetPool<KinematicMotor2D>();
    const auto riderIds  = motorPool.Entities();
    auto motors          = motorPool.Components();
    const u16 motorCount = motorPool.Size();

    for (u16 i = 0; i < motorCount; ++i) {
        KinematicMotor2D& motor = motors[i];
        if (motor.carrierId == world::kNullEntity ||
            (frozenMask & (1u << static_cast<u8>(motor.timeChannel))) != 0) {
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

        const world::EntityId riderId    = riderIds[i];
        const world::EntityId savedCarry = motor.carrierId;
        const ContactFlags savedGround   = motor.contacts &
            (ContactFlags::Ground | ContactFlags::OnSlope | ContactFlags::OnOneWay);

        world::LocalTransform2D& loc = world.ModifyLocalTransformByIndex(riderId.index);
        ResolveSingleMotorSweep(riderId, loc, motor, spatial, carrier->deltaX, carrier->deltaY, cfg);

        if ((carrier->deltaY > 0.0f && HasContact(motor.contacts, ContactFlags::Ceiling)) ||
            (carrier->deltaY < 0.0f && HasContact(motor.contacts, ContactFlags::Ground) && motor.carrierId != savedCarry) ||
            (carrier->deltaX > 0.0f && HasContact(motor.contacts, ContactFlags::WallRight)) ||
            (carrier->deltaX < 0.0f && HasContact(motor.contacts, ContactFlags::WallLeft))) {
            motor.flags |= MotorFlags::Crushed;
        }

        motor.contacts |= savedGround;
        motor.carrierId = savedCarry;
    }
}

void KinematicSolver2D::StepMotorsAndMotionStacks(
    world::World& world,
    const SpatialIndex2D& spatial,
    time::SimStepView step,
    const KinematicStepConfig& cfg) noexcept {

    const u8 frozenMask = step.FrozenMask();
    if (frozenMask == 0xFFu) {
        return;
    }

    constexpr world::ComponentMask kMotionBit = world::MakeComponentMask<MotionStack2D>();
    constexpr world::ComponentMask kCrumbBit  = world::MakeComponentMask<SafeGroundBreadcrumb2D>();

    auto& motorPool  = world.GetPool<KinematicMotor2D>();
    auto& motionPool = world.GetPool<MotionStack2D>();
    auto& crumbPool  = world.GetPool<SafeGroundBreadcrumb2D>();

    const auto ids       = motorPool.Entities();
    auto motors          = motorPool.Components();
    const u16 motorCount = motorPool.Size();

    for (u16 i = 0; i < motorCount; ++i) {
        KinematicMotor2D& motor = motors[i];
        if ((frozenMask & (1u << static_cast<u8>(motor.timeChannel))) != 0) continue;

        const f32 dt = step.Dt(motor.timeChannel);
        if (dt <= 0.0f) continue;

        motor.oneWayIgnoreTicks -= (motor.oneWayIgnoreTicks > 0);
        motor.flags &= ~MotorFlags::Crushed;

        const world::EntityId id       = ids[i];
        const world::ComponentMask msk = world.GetComponentMaskByIndex(id.index);
        world::LocalTransform2D& loc   = world.ModifyLocalTransformByIndex(id.index);

        const f32 cx = loc.x + motor.bounds.OffsetX();
        const f32 cy = loc.y + motor.bounds.OffsetY();
        const f32 hw = motor.bounds.HalfW();
        const f32 hh = motor.bounds.HalfH();

        const ZoneSample2D zone = spatial.SampleEnvironmentalZones(cx, cy);
        if (zone.submerged) motor.flags |= MotorFlags::Submerged;
        else                motor.flags &= ~MotorFlags::Submerged;

        f32 rootDx = 0.0f, rootDy = 0.0f;
        MotionStack2D* const motion = ((msk & kMotionBit) != 0)
            ? &motionPool.DirectAtEntityIndex(id.index)
            : nullptr;

        if (motion != nullptr) {
            if (motor.carrierId != world::kNullEntity && motion->IsCarrierLaunchActive()) {
                if (const auto* carrier = world.TryGet<KinematicCarrier2D>(motor.carrierId)) {
                    motion->impulseVx += carrier->deltaX / dt;
                    motion->impulseVy += std::max(0.0f, carrier->deltaY / dt);
                }
                motion->SetCarrierLaunchActive(false);
            }

            const f32 gravDelta = cfg.baseGravity * (static_cast<f32>(motion->gravityScale_q8) * (1.0f / 256.0f)) * dt;
            if (motion->overrideTicks > 0) {
                motor.vx = motion->overrideVx;
                motor.vy = motion->overrideVy + gravDelta;
            } else {
                motor.vx = motion->targetWalkVx;
                motor.vy = std::max(cfg.terminalFallVy, motor.vy + gravDelta);
            }

            motor.vx = (motor.vx + motion->impulseVx + zone.forceVx) * zone.dragScale;
            motor.vy = (motor.vy + motion->impulseVy + zone.forceVy) * zone.dragScale;
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

        if (HasContact(motor.contacts, ContactFlags::OnSlope) &&
            HasMotorFlag(motor.flags, MotorFlags::ProjectSlopes) &&
            motor.vy <= 0.0f) {
            const f32 sny = motor.SlopeNormalY();
            if (sny > 0.2f) deltaY -= (motor.SlopeNormalX() / sny) * deltaX;
        }

        ResolveSingleMotorSweep(id, loc, motor, spatial, deltaX, deltaY, cfg);

        if (motion != nullptr) {
            if (HasContact(motor.contacts, ContactFlags::Ground)) {
                motion->groundCoyoteTicks = cfg.maxCoyoteTicks;
                motion->SetWallSign(0);
                motion->SetCarrierLaunchActive(motor.carrierId != world::kNullEntity);
            } else if (HasContact(motor.contacts, ContactFlags::WallLeft | ContactFlags::WallRight)) {
                motion->wallCoyoteTicks = cfg.maxWallCoyoteTicks;
                motion->SetWallSign(HasContact(motor.contacts, ContactFlags::WallLeft) ? i8{-1} : i8{1});
                motor.vy = std::max(motor.vy, cfg.wallSlideMaxVy);
            }
        }

        if (HasContact(motor.contacts, ContactFlags::Ground) &&
            motor.carrierId == world::kNullEntity &&
            HasMotorFlag(motor.flags, MotorFlags::TrackSafeAnchor) &&
            (msk & kCrumbBit) != 0) {
            SafeGroundBreadcrumb2D& crumb = crumbPool.DirectAtEntityIndex(id.index);
            const f32 footY = loc.y + motor.bounds.OffsetY() - hh - 0.05f;
            const f32 footX = loc.x + motor.bounds.OffsetX();
            if (spatial.CheckStaticPointSolid(footX - hw * 0.7f, footY, true) &&
                spatial.CheckStaticPointSolid(footX, footY, true) &&
                spatial.CheckStaticPointSolid(footX + hw * 0.7f, footY, true) &&
                (std::abs(loc.x - crumb.safeX) > 1.5f || std::abs(loc.y - crumb.safeY) > 1.5f)) {
                crumb.prevSafeX = crumb.safeX;
                crumb.prevSafeY = crumb.safeY;
                crumb.safeX     = loc.x;
                crumb.safeY     = loc.y;
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

    const bool wasGrounded  = HasContact(inoutMotor.contacts, ContactFlags::Ground);
    const bool notJumpingUp = (inoutMotor.vy <= 0.0f);
    inoutMotor.contacts     = ContactFlags::None;
    inoutMotor.carrierId    = world::kNullEntity;

    const f32 offX = inoutMotor.bounds.OffsetX();
    const f32 offY = inoutMotor.bounds.OffsetY();
    const f32 hw   = inoutMotor.bounds.HalfW();
    const f32 hh   = inoutMotor.bounds.HalfH();
    const bool ignoreOneWay = (inoutMotor.oneWayIgnoreTicks > 0);

    bool steppedUp = false;
    f32 stepHeightUsed = 0.0f;

    if (std::abs(deltaX) > 1e-6f) {
        const f32 cx = inoutTransform.x + offX, cy = inoutTransform.y + offY;
        const SlabImpact2D hitX = spatial.SweepAabb(id, cx, cy, hw, hh, deltaX, 0.0f, inoutMotor.solidMask, true);
        if (hitX.hit) {
            if (HasMotorFlag(inoutMotor.flags, MotorFlags::CornerCorrection) && (wasGrounded || notJumpingUp)) {
                const f32 stepHeight = std::min(0.16f, cfg.cornerCorrectionMax);
                if (!spatial.SweepAabb(id, cx, cy + stepHeight, hw, hh, deltaX, 0.0f, inoutMotor.solidMask, true).hit) {
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

    const f32 effectiveDeltaY = steppedUp ? std::min(deltaY, -(stepHeightUsed + cfg.skinWidth * 2.0f)) : deltaY;

    if (std::abs(effectiveDeltaY) > 1e-6f || (wasGrounded && notJumpingUp)) {
        const f32 cx = inoutTransform.x + offX, cy = inoutTransform.y + offY;
        const SlabImpact2D hitY = (std::abs(effectiveDeltaY) > 1e-6f)
            ? spatial.SweepAabb(id, cx, cy, hw, hh, 0.0f, effectiveDeltaY, inoutMotor.solidMask, ignoreOneWay)
            : SlabImpact2D{};

        if (hitY.hit) {
            bool cornerCorrected = false;
            if (effectiveDeltaY > 0.0f && hitY.normalY < 0.0f &&
                HasMotorFlag(inoutMotor.flags, MotorFlags::CornerCorrection)) {
                for (f32 nudge : {-cfg.cornerCorrectionMax, cfg.cornerCorrectionMax}) {
                    if (!spatial.SweepAabb(id, cx + nudge, cy, hw, hh, 0.0f, effectiveDeltaY, inoutMotor.solidMask, true).hit) {
                        inoutTransform.x += nudge;
                        inoutTransform.y += effectiveDeltaY;
                        cornerCorrected = true;
                        break;
                    }
                }
            }

            if (!cornerCorrected) {
                inoutTransform.y += effectiveDeltaY * hitY.toi + hitY.normalY * cfg.skinWidth;
                if (hitY.normalY > 0.0f) {
                    ApplyGroundContact(inoutMotor, hitY);
                } else {
                    inoutMotor.vy = 0.0f;
                    inoutMotor.contacts |= ContactFlags::Ceiling;
                }
            }
        } else {
            inoutTransform.y += effectiveDeltaY;

            if ((wasGrounded || steppedUp) && notJumpingUp && HasMotorFlag(inoutMotor.flags, MotorFlags::SnapToGround)) {
                const SlabImpact2D snapHit = spatial.SweepAabb(
                    id, inoutTransform.x + offX, inoutTransform.y + offY,
                    hw, hh, 0.0f, -cfg.groundSnapMaxDist, inoutMotor.solidMask, ignoreOneWay);
                if (snapHit.hit && snapHit.normalY > 0.5f) {
                    inoutTransform.y += (-cfg.groundSnapMaxDist) * snapHit.toi + cfg.skinWidth;
                    ApplyGroundContact(inoutMotor, snapHit);
                }
            }
        }
    }
}

} // namespace en::gameplay
