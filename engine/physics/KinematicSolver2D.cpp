#include "physics/KinematicSolver2D.h"
#include <algorithm>
#include <cmath>

namespace en {

void KinematicSolver2D::StepCarriersAndRiders(
    World& world, const SpatialIndex2D& spatial, const SimStepView& step,
    const KinematicStepConfig& /*cfg*/) noexcept {
    if (world.GetPool<KinematicCarrier2D>().Size() == 0) return;

    world.Each<KinematicCarrier2D>([&](EntityId carrierId, const KinematicCarrier2D& carrier) noexcept {
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(carrierId.index));
        if (step.IsFrozen(ch)) return;
        const Vec2 delta{carrier.deltaX, carrier.deltaY};
        if (delta.LengthSq() < 1e-8f) return;

        LocalTransform2D& cLoc = world.ModifyLocalTransformByIndex(carrierId.index);
        cLoc.x += delta.x;
        cLoc.y += delta.y;

        world.Each<KinematicMotor2D>([&](EntityId riderId, KinematicMotor2D& motor) noexcept {
            if (motor.carrierId != carrierId) return;
            LocalTransform2D& rLoc = world.ModifyLocalTransformByIndex(riderId.index);
            const Vec2 half = motor.bounds.HalfExtents();
            const Vec2 center = Vec2{rLoc.x, rLoc.y} + motor.bounds.Offset();
            const SlabImpact2D imp = spatial.SweepAabb(
                riderId, center, half, delta, motor.solidMask, motor.oneWayIgnoreTicks > 0);
            if (imp.hit && imp.toi < 0.99f) {
                rLoc.x += delta.x * imp.toi;
                rLoc.y += delta.y * imp.toi;
                motor.flags |= MotorFlags::Crushed;
            } else {
                rLoc.x += delta.x;
                rLoc.y += delta.y;
                motor.contacts |= ContactFlags::Ground;
            }
        });
    });
    world.PropagateTransforms();
}

void KinematicSolver2D::StepMotorsAndMotionStacks(
    World& world, const SpatialIndex2D& spatial, const SimStepView& step,
    const KinematicStepConfig& cfg) noexcept {
    world.Each<KinematicMotor2D>([&](EntityId id, KinematicMotor2D& motor) noexcept {
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) return;
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) return;

        if (motor.oneWayIgnoreTicks > 0) --motor.oneWayIgnoreTicks;
        if (motor.stepCooldownTicks > 0) --motor.stepCooldownTicks;

        MotionStack2D* ms = world.TryGet<MotionStack2D>(id);
        if (ms) {
            if (motor.Grounded()) {
                ms->groundCoyoteTicks = cfg.maxCoyoteTicks;
            } else if (ms->groundCoyoteTicks > 0) {
                --ms->groundCoyoteTicks;
            }

            if (HasFlag(motor.contacts, ContactFlags::WallLeft)) {
                ms->wallCoyoteTicks = cfg.maxCoyoteTicks;
                ms->wallSign = -1;
            } else if (HasFlag(motor.contacts, ContactFlags::WallRight)) {
                ms->wallCoyoteTicks = cfg.maxCoyoteTicks;
                ms->wallSign = 1;
            } else if (ms->wallCoyoteTicks > 0) {
                --ms->wallCoyoteTicks;
            }

            if (ms->overrideTicks > 0) {
                --ms->overrideTicks;
                motor.vx = ms->overrideVx;
                motor.vy = ms->overrideVy;
                if (ms->overrideTicks == 0) ms->activePriority = 0;
            } else {
                const f32 accel = motor.Grounded() ? cfg.groundAccel : cfg.airAccel;
                const f32 diff = ms->targetWalkVx - motor.vx;
                const f32 maxDelta = accel * dt;
                motor.vx += std::clamp(diff, -maxDelta, maxDelta);

                if (HasFlag(motor.flags, MotorFlags::UseGravity)) {
                    const f32 gScale = static_cast<f32>(ms->gravityScale_q8) * (1.0f / 256.0f);
                    motor.vy -= cfg.gravity * gScale * dt;
                }
            }

            motor.vx += ms->impulseVx;
            motor.vy += ms->impulseVy;
            ms->impulseVx = 0.0f;
            ms->impulseVy = 0.0f;

            if (!ms->lockFacing && std::abs(ms->targetWalkVx) > 0.05f) {
                ms->facingSign = (ms->targetWalkVx >= 0.0f) ? 1 : -1;
            }
        } else if (HasFlag(motor.flags, MotorFlags::UseGravity)) {
            motor.vy -= cfg.gravity * dt;
        }

        if (HasFlag(motor.contacts, ContactFlags::InWater)) {
            motor.vy += cfg.waterBuoyancy * dt;
            motor.vx *= std::max(0.0f, 1.0f - cfg.waterDrag * dt);
            motor.vy *= std::max(0.0f, 1.0f - cfg.waterDrag * dt);
        }

        motor.vy = std::max(motor.vy, -cfg.maxFallSpeed);
        if (HasFlag(motor.flags, MotorFlags::SlideWalls) &&
            HasFlag(motor.contacts, ContactFlags::WallLeft | ContactFlags::WallRight) &&
            motor.vy < -cfg.wallSlideSpeed) {
            motor.vy = -cfg.wallSlideSpeed;
        }

        LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
        if (HasFlag(motor.flags, MotorFlags::Noclip)) {
            loc.x += motor.vx * dt;
            loc.y += motor.vy * dt;
            motor.contacts = ContactFlags::None;
            return;
        }

        const Vec2 half = motor.bounds.HalfExtents();
        const Vec2 offset = motor.bounds.Offset();
        ContactFlags newContacts = ContactFlags::None;
        EntityId newCarrier = kNullEntity;

        const f32 dx = motor.vx * dt;
        if (std::abs(dx) > 1e-6f) {
            const Vec2 center{loc.x + offset.x, loc.y + offset.y};
            const SlabImpact2D impX = spatial.SweepAabb(
                id, center, half, {dx, 0.0f}, motor.solidMask, true);
            if (impX.hit) {
                bool steppedUp = false;
                if (motor.Grounded() && cfg.stepUpHeight > 0.0f) {
                    const Vec2 raisedCenter{center.x, center.y + cfg.stepUpHeight};
                    const SlabImpact2D impStep = spatial.SweepAabb(
                        id, raisedCenter, half, {dx, 0.0f}, motor.solidMask, true);
                    if (!impStep.hit) {
                        loc.x += dx;
                        loc.y += cfg.stepUpHeight;
                        steppedUp = true;
                    }
                }
                if (!steppedUp) {
                    loc.x += dx * std::max(0.0f, impX.toi - 0.001f);
                    motor.vx = 0.0f;
                    if (impX.normal.x > 0.5f) newContacts |= ContactFlags::WallLeft;
                    else if (impX.normal.x < -0.5f) newContacts |= ContactFlags::WallRight;
                }
            } else {
                loc.x += dx;
            }
        }

        const f32 dy = motor.vy * dt;
        if (std::abs(dy) > 1e-6f) {
            const Vec2 center{loc.x + offset.x, loc.y + offset.y};
            const SlabImpact2D impY = spatial.SweepAabb(
                id, center, half, {0.0f, dy}, motor.solidMask, motor.oneWayIgnoreTicks > 0);
            if (impY.hit) {
                loc.y += dy * std::max(0.0f, impY.toi - 0.001f);
                if (impY.normal.y > 0.5f) {
                    newContacts |= ContactFlags::Ground;
                    newCarrier = impY.hitEntity;
                    if (motor.vy < 0.0f) motor.vy = 0.0f;
                } else if (impY.normal.y < -0.5f) {
                    newContacts |= ContactFlags::Ceiling;
                    if (motor.vy > 0.0f) motor.vy = 0.0f;
                }
            } else {
                loc.y += dy;
            }
        } else {
            const Vec2 center{loc.x + offset.x, loc.y + offset.y};
            const SlabImpact2D probe = spatial.SweepAabb(
                id, center, half, {0.0f, -0.05f}, motor.solidMask, motor.oneWayIgnoreTicks > 0);
            if (probe.hit && probe.normal.y > 0.5f) {
                newContacts |= ContactFlags::Ground;
                newCarrier = probe.hitEntity;
            }
        }

        if (HasFlag(motor.flags, MotorFlags::SnapToSlopes)) {
            const Vec2 footPt{loc.x + offset.x, loc.y + offset.y - half.y - 0.05f};
            const i32 tx = static_cast<i32>(std::floor((footPt.x - spatial.Origin().x) / spatial.TileSize()));
            const i32 ty = static_cast<i32>(std::floor((footPt.y - spatial.Origin().y) / spatial.TileSize()));
            const TileType tt = spatial.GetTile(tx, ty);
            if (tt == TileType::SlopeUpRight || tt == TileType::SlopeUpLeft) {
                Vec2 slopeNorm{};
                const f32 surfY = spatial.SampleSlopeSurfaceY(tx, ty, footPt.x, slopeNorm);
                if (std::abs((loc.y + offset.y - half.y) - surfY) <= cfg.slopeSnapDist && motor.vy <= 0.1f) {
                    loc.y = surfY + half.y - offset.y;
                    motor.vy = 0.0f;
                    newContacts |= (ContactFlags::Ground | ContactFlags::OnSlope);
                    motor.slopeNormalX_q14 = static_cast<i16>(slopeNorm.x * 16384.0f);
                    motor.slopeNormalY_q14 = static_cast<i16>(slopeNorm.y * 16384.0f);
                }
            } else if (tt == TileType::ConveyorRight && HasFlag(newContacts, ContactFlags::Ground)) {
                loc.x += cfg.conveyorSpeed * dt;
            } else if (tt == TileType::ConveyorLeft && HasFlag(newContacts, ContactFlags::Ground)) {
                loc.x -= cfg.conveyorSpeed * dt;
            }
        }

        const Rect finalRect = motor.bounds.WorldRect({loc.x, loc.y});
        newContacts |= spatial.SampleEnvironmentFlags(finalRect);
        motor.contacts = newContacts;
        motor.carrierId = newCarrier;
    });
    world.PropagateTransforms();
}

void KinematicSolver2D::StepConstraintsAndVerlet(
    World& world, const SpatialIndex2D& spatial, const SimStepView& step,
    const KinematicStepConfig& cfg) noexcept {
    world.Each<DistanceJoint2D>([&](EntityId id, const DistanceJoint2D& joint) noexcept {
        if (!world.IsAlive(joint.targetEntity)) return;
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) return;
        LocalTransform2D& locA = world.ModifyLocalTransformByIndex(id.index);
        const WorldTransform2D& wtB = world.GetWorldByIndex(joint.targetEntity.index);
        const Vec2 delta = Vec2{wtB.x - locA.x, wtB.y - locA.y};
        const f32 dist = delta.Length();
        if (dist > 1e-4f) {
            const f32 err = (dist - joint.restLength) * joint.stiffness;
            const Vec2 corr = (delta / dist) * err;
            locA.x += corr.x;
            locA.y += corr.y;
        }
    });

    world.Each<VerletRope2D>([&](EntityId id, VerletRope2D& rope) noexcept {
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) return;
        const f32 dt = step.Dt(ch);
        const u8 count = std::min<u8>(rope.nodeCount, static_cast<u8>(VerletRope2D::kMaxNodes));
        if (count < 2) return;
        const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
        if (rope.pinStartToEntity) {
            rope.currPos[0] = {wt.x, wt.y};
            rope.prevPos[0] = {wt.x, wt.y};
        }
        for (u8 i = (rope.pinStartToEntity ? 1U : 0U); i < count; ++i) {
            const Vec2 cur = rope.currPos[i];
            const Vec2 vel = (cur - rope.prevPos[i]) * rope.damping;
            rope.prevPos[i] = cur;
            rope.currPos[i] = cur + vel + Vec2{0.0f, -cfg.gravity * dt * dt};
        }
        const u16 iters = std::clamp<u16>(rope.solverIterations, 1, 8);
        for (u16 iter = 0; iter < iters; ++iter) {
            if (rope.pinStartToEntity) rope.currPos[0] = {wt.x, wt.y};
            for (u8 i = 0; i + 1U < count; ++i) {
                const Vec2 d = rope.currPos[i + 1] - rope.currPos[i];
                const f32 len = d.Length();
                if (len > 1e-5f) {
                    const Vec2 offset = d * ((len - rope.segmentLength) / len);
                    if (i == 0 && rope.pinStartToEntity) {
                        rope.currPos[i + 1] -= offset;
                    } else {
                        rope.currPos[i] += offset * 0.5f;
                        rope.currPos[i + 1] -= offset * 0.5f;
                    }
                }
                const i32 tx = static_cast<i32>(std::floor((rope.currPos[i + 1].x - spatial.Origin().x) / spatial.TileSize()));
                const i32 ty = static_cast<i32>(std::floor((rope.currPos[i + 1].y - spatial.Origin().y) / spatial.TileSize()));
                if (spatial.GetTile(tx, ty) == TileType::Solid) {
                    rope.currPos[i + 1].y = spatial.Origin().y + static_cast<f32>(ty + 1) * spatial.TileSize() + 0.02f;
                }
            }
        }
    });
}

SlabImpact2D KinematicSolver2D::SweepProjectileCCD(
    const SpatialIndex2D& spatial,
    EntityId ignoreEntity,
    Vec2 startPos,
    Vec2 velocity,
    f32 dt,
    Vec2 halfExtents,
    u16 hitMask) noexcept {
    const Vec2 totalDelta = velocity * dt;
    const f32 totalDist = totalDelta.Length();
    if (totalDist <= 1e-6f) {
        return SlabImpact2D{1.0f, {0.0f, 0.0f}, kNullEntity, false};
    }

    const f32 minExtent = std::max(0.05f, std::min(halfExtents.x, halfExtents.y));
    const u32 steps = std::clamp<u32>(static_cast<u32>(std::ceil(totalDist / minExtent)), 1U, 16U);
    const Vec2 stepDelta = totalDelta * (1.0f / static_cast<f32>(steps));
    Vec2 curPos = startPos;

    for (u32 s = 0; s < steps; ++s) {
        const SlabImpact2D hit = spatial.SweepAabb(ignoreEntity, curPos, halfExtents, stepDelta, hitMask, false);
        if (hit.hit) {
            SlabImpact2D result = hit;
            result.toi = (static_cast<f32>(s) + hit.toi) / static_cast<f32>(steps);
            return result;
        }
        curPos += stepDelta;
    }
    return SlabImpact2D{1.0f, {0.0f, 0.0f}, kNullEntity, false};
}

} // namespace en
