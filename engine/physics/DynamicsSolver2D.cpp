#include "physics/DynamicsSolver2D.h"

#include "physics/Narrowphase2D.h"

#include <algorithm>

namespace en {

namespace {

constexpr void AppendContactEvent(
    std::span<ContactEvent2D> outEvents,
    u16& ioCount,
    const ContactEvent2D& ev) noexcept {
    if (ioCount < outEvents.size()) {
        outEvents[ioCount++] = ev;
    }
}

[[nodiscard]] constexpr Vec2 CrossScalarVec(f32 w, Vec2 r) noexcept {
    return {-w * r.y, w * r.x};
}

constexpr void ApplyBodyImpulse(RigidBody2D* b, Vec2 P, Vec2 r, f32 sign) noexcept {
    if (!b || !b->IsDynamic()) {
        return;
    }
    b->vx += sign * P.x * b->invMass;
    b->vy += sign * P.y * b->invMass;
    b->angularVel += sign * r.Cross(P) * b->invInertia;
}

[[nodiscard]] constexpr bool IsTileSentinelEntity(EntityId id) noexcept {
    return (id.generation & 0x8000U) != 0U;
}

[[nodiscard]] constexpr bool IsActiveMover(const RigidBody2D* b) noexcept {
    if (!b || !HasFlag(b->flags, BodyFlags2D::Enabled)) {
        return false;
    }
    if (b->IsDynamic()) {
        return !b->IsSleeping();
    }
    if (b->IsKinematic()) {
        return std::abs(b->vx) > 1e-5f || std::abs(b->vy) > 1e-5f || std::abs(b->angularVel) > 1e-5f;
    }
    return false;
}

[[nodiscard]] constexpr Vec2 BodyContactVelocity(const RigidBody2D* b, Vec2 r) noexcept {
    return b ? (Vec2{b->vx, b->vy} + CrossScalarVec(b->angularVel, r)) : Vec2{};
}

} // namespace

bool DynamicsSolver2D::StepKinematicBodiesAndRiders(
    World& world,
    const SpatialIndex2D& spatial,
    const SimStepView& step) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) == 0ULL) {
        return false;
    }
    auto& bodyPool = world.GetPool<RigidBody2D>();
    if (bodyPool.Size() == 0U) {
        return false;
    }

    const auto bodyEntities = bodyPool.Entities();
    const auto bodies = bodyPool.Components();
    const bool hasCols = (world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) != 0ULL;
    const bool hasMotors = (world.RegisteredPoolsMask() & (1ULL << CharacterMotor2D::kTypeId)) != 0ULL;
    bool anyMoved = false;

    for (usize bIdx = 0U; bIdx < bodyEntities.size(); ++bIdx) {
        const RigidBody2D& body = bodies[bIdx];
        if (!body.IsKinematic() || !HasFlag(body.flags, BodyFlags2D::Enabled)) {
            continue;
        }

        const EntityId carrierId = bodyEntities[bIdx];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(carrierId.index));
        if (step.IsFrozen(ch)) {
            continue;
        }
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) {
            continue;
        }

        f32 dx = body.vx * dt;
        f32 dy = body.vy * dt;
        const f32 dRot = body.angularVel * dt;
        if (std::abs(dx) <= 1e-7f && std::abs(dy) <= 1e-7f && std::abs(dRot) <= 1e-7f) {
            continue;
        }

        const WorldTransform2D& cXfBefore = world.GetWorldByIndex(carrierId.index);
        const bool hasCol = hasCols && ((world.GetComponentMaskByIndex(carrierId.index) & (1ULL << Collider2D::kTypeId)) != 0ULL);

        f32 cCenterX = cXfBefore.x;
        f32 cCenterY = cXfBefore.y;
        f32 cHalfW = 0.0f;
        f32 cHalfH = 0.0f;
        if (hasCol) {
            const Collider2D& col = world.GetPool<Collider2D>().DirectAtEntityIndex(carrierId.index);
            const Vec2 c = cXfBefore.TransformPoint({col.localBounds.OffsetX(), col.localBounds.OffsetY()});
            cCenterX = c.x;
            cCenterY = c.y;
            cHalfW = col.localBounds.HalfW() * std::abs(cXfBefore.scaleX);
            cHalfH = col.localBounds.HalfH() * std::abs(cXfBefore.scaleY);

            if (HasFlag(body.flags, BodyFlags2D::BulletCCD) && !col.IsSensor()) {
                const SweepHit2D kHit = spatial.SweepAabb(
                    carrierId, cCenterX, cCenterY, cHalfW, cHalfH, dx, dy, col.filter.maskBits, true);
                if (kHit.Hit() && kHit.toi < 1.0f) {
                    const f32 safeToi = std::max(0.0f, kHit.toi - 1e-4f);
                    dx *= safeToi;
                    dy *= safeToi;
                }
            }
        }
        const f32 cTopY = cCenterY + cHalfH;

        LocalTransform2D& cLoc = world.ModifyLocalTransformByIndex(carrierId.index);
        cLoc.x += dx;
        cLoc.y += dy;
        if (!HasFlag(body.flags, BodyFlags2D::FixedRotation)) {
            cLoc.rotationRad += dRot;
        }
        anyMoved = true;

        if (!hasMotors || !hasCol || (std::abs(dx) <= 1e-7f && std::abs(dy) <= 1e-7f)) {
            continue;
        }
        auto& motorPool = world.GetPool<CharacterMotor2D>();
        const auto motorEntities = motorPool.Entities();
        auto motors = motorPool.Components();

        for (usize mIdx = 0U; mIdx < motorEntities.size(); ++mIdx) {
            CharacterMotor2D& motor = motors[mIdx];
            if (!HasFlag(motor.flags, BodyFlags2D::Enabled)) {
                continue;
            }
            const EntityId riderId = motorEntities[mIdx];
            const WorldTransform2D& rXf = world.GetWorldByIndex(riderId.index);
            const f32 rHalfW = motor.bounds.HalfW() * std::abs(rXf.scaleX);
            const f32 rHalfH = motor.bounds.HalfH() * std::abs(rXf.scaleY);
            const f32 rCenterX = rXf.x + motor.bounds.OffsetX() * rXf.scaleX;
            const f32 rCenterY = rXf.y + motor.bounds.OffsetY() * rXf.scaleY;
            const f32 rBottomY = rCenterY - rHalfH;

            const f32 vGapTol = (motor.carrierId == carrierId) ? 0.12f : 0.06f;
            const bool onCarrierTop =
                (rCenterX + rHalfW > cCenterX - cHalfW) &&
                (rCenterX - rHalfW < cCenterX + cHalfW) &&
                (std::abs(rBottomY - cTopY) <= vGapTol) &&
                (motor.vy <= 0.05f);

            if (!onCarrierTop) {
                if (motor.carrierId == carrierId) {
                    motor.carrierId = kNullEntity;
                }
                continue;
            }

            const SweepHit2D hit = spatial.SweepAabb(
                riderId, rCenterX, rCenterY, rHalfW, rHalfH, dx, dy, motor.solidMask, true);

            LocalTransform2D& rLoc = world.ModifyLocalTransformByIndex(riderId.index);
            if (hit.Hit() && hit.hitEntity != carrierId && hit.toi < 1.0f) {
                rLoc.x += dx * hit.toi;
                rLoc.y += dy * hit.toi;
                if (hit.toi < 0.05f && (dx * hit.normalX + dy * hit.normalY) < -1e-3f) {
                    motor.contacts |= ContactFlags::Crushed;
                }
            } else {
                rLoc.x += dx;
                rLoc.y += dy;
            }
            motor.carrierId = carrierId;
            motor.contacts |= ContactFlags::Ground;
        }
    }
    if (anyMoved) {
        world.PropagateTransforms();
    }
    return anyMoved;
}

void DynamicsSolver2D::StepCharacterMotors(
    World& world,
    const SpatialIndex2D& spatial,
    const SimStepView& step,
    const PhysicsConfig& cfg) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) {
        return;
    }
    auto& motorPool = world.GetPool<CharacterMotor2D>();
    const auto entities = motorPool.Entities();
    auto motors = motorPool.Components();

    for (usize i = 0U; i < entities.size(); ++i) {
        CharacterMotor2D& motor = motors[i];
        if (!HasFlag(motor.flags, BodyFlags2D::Enabled)) {
            continue;
        }

        const EntityId id = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) {
            continue;
        }
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) {
            continue;
        }

        const bool ignoreOneWay = HasFlag(motor.flags, BodyFlags2D::IgnoreOneWay);
        const bool wasGrounded = HasFlag(motor.contacts, ContactFlags::Ground);
        motor.contacts &= ContactFlags::Crushed;

        if (HasFlag(motor.flags, BodyFlags2D::UseGravity)) {
            motor.vy += cfg.gravityY * dt;
        }

        const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        const f32 halfW = motor.bounds.HalfW() * std::abs(xf.scaleX);
        const f32 halfH = motor.bounds.HalfH() * std::abs(xf.scaleY);
        const f32 offX = motor.bounds.OffsetX() * xf.scaleX;
        const f32 offY = motor.bounds.OffsetY() * xf.scaleY;

        f32 curCenterX = xf.x + offX;
        f32 curCenterY = xf.y + offY;
        f32 remDX = motor.vx * dt;
        f32 remDY = motor.vy * dt;

        const auto setGrounded = [&](const SweepHit2D& h) noexcept {
            motor.contacts |= ContactFlags::Ground;
            if (h.normalY < 0.999f) motor.contacts |= ContactFlags::OnSlope;
            if (h.isOneWay != 0U)   motor.contacts |= ContactFlags::OnOneWay;
            motor.groundNormalX = h.normalX;
            motor.groundNormalY = h.normalY;
            motor.carrierId = h.hitEntity;
            if (motor.vy < 0.0f) motor.vy = 0.0f;
        };

        for (u32 iter = 0U; iter < 3U; ++iter) {
            if (std::abs(remDX) <= 1e-6f && std::abs(remDY) <= 1e-6f) {
                break;
            }

            const SweepHit2D hit = spatial.SweepAabb(
                id, curCenterX, curCenterY, halfW, halfH, remDX, remDY, motor.solidMask, ignoreOneWay);

            if (!hit.Hit()) {
                curCenterX += remDX;
                curCenterY += remDY;
                break;
            }

            if (hit.startPenetrating != 0U) {
                curCenterX += hit.normalX * (hit.penetration + 1e-3f);
                curCenterY += hit.normalY * (hit.penetration + 1e-3f);
            } else {
                const f32 safeToi = std::max(0.0f, hit.toi - 1e-4f);
                curCenterX += remDX * safeToi;
                curCenterY += remDY * safeToi;
                if (hit.toi <= 1e-3f && std::abs(hit.normalX) > 0.7f) {
                    curCenterX += hit.normalX * 3e-4f;
                }
            }

            const f32 fracRemaining = 1.0f - hit.toi;
            remDX *= fracRemaining;
            remDY *= fracRemaining;

            if (std::abs(hit.normalX) > 0.9f && cfg.maxStepHeight > 1e-3f && (wasGrounded || motor.vy <= 0.0f)) {
                if (!spatial.SweepAabb(id, curCenterX, curCenterY, halfW, halfH, 0.0f, cfg.maxStepHeight, motor.solidMask, true).Hit()) {
                    const f32 raisedY = curCenterY + cfg.maxStepHeight;
                    const SweepHit2D fwdHit = spatial.SweepAabb(
                        id, curCenterX, raisedY, halfW, halfH, remDX, 0.0f, motor.solidMask, true);
                    if (!fwdHit.Hit() || fwdHit.toi > 0.25f) {
                        const f32 stepAdvance = fwdHit.Hit() ? (remDX * std::max(0.0f, fwdHit.toi - 1e-4f)) : remDX;
                        const f32 steppedX = curCenterX + stepAdvance;
                        const SweepHit2D downHit = spatial.SweepAabb(
                            id, steppedX, raisedY, halfW, halfH, 0.0f, -cfg.maxStepHeight, motor.solidMask, ignoreOneWay);
                        if (downHit.Hit() && downHit.normalY >= cfg.minGroundNormalY) {
                            curCenterX = steppedX;
                            curCenterY = raisedY - cfg.maxStepHeight * std::max(0.0f, downHit.toi - 1e-4f);
                            setGrounded(downHit);
                            remDX -= stepAdvance;
                            remDY = 0.0f;
                            continue;
                        }
                    }
                }
            }

            if (hit.normalY >= cfg.minGroundNormalY) {
                setGrounded(hit);
                remDY = (std::abs(hit.normalY) > 1e-4f) ? ((-hit.normalX / hit.normalY) * remDX) : 0.0f;
            } else {
                if (hit.normalY < -0.7f) {
                    motor.contacts |= ContactFlags::Ceiling;
                    if (motor.vy > 0.0f) motor.vy = 0.0f;
                }
                if (hit.normalX > 0.7f) {
                    motor.contacts |= ContactFlags::WallLeft;
                    if (motor.vx < 0.0f) motor.vx = 0.0f;
                } else if (hit.normalX < -0.7f) {
                    motor.contacts |= ContactFlags::WallRight;
                    if (motor.vx > 0.0f) motor.vx = 0.0f;
                }

                const f32 dotRem = remDX * hit.normalX + remDY * hit.normalY;
                if (dotRem < 0.0f) {
                    remDX -= hit.normalX * dotRem;
                    remDY -= hit.normalY * dotRem;
                }
            }
        }

        if (!HasFlag(motor.contacts, ContactFlags::Ground) &&
            wasGrounded && motor.vy <= 0.0f && cfg.groundSnapDist > 0.0f) {
            const SweepHit2D snapHit = spatial.SweepAabb(
                id, curCenterX, curCenterY, halfW, halfH, 0.0f, -cfg.groundSnapDist, motor.solidMask, ignoreOneWay);
            if (snapHit.Hit() && snapHit.normalY >= cfg.minGroundNormalY && snapHit.toi > 0.0f && snapHit.toi < 1.0f) {
                curCenterY -= cfg.groundSnapDist * std::max(0.0f, snapHit.toi - 1e-4f);
                setGrounded(snapHit);
            }
        }

        if (!HasFlag(motor.contacts, ContactFlags::Ground)) {
            motor.carrierId = kNullEntity;
            motor.groundNormalX = 0.0f;
            motor.groundNormalY = 1.0f;
        }

        LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
        loc.x += (curCenterX - offX) - xf.x;
        loc.y += (curCenterY - offY) - xf.y;
    }
    world.PropagateTransforms();
}

u16 DynamicsSolver2D::GatherManifolds(
    World& world,
    const SpatialIndex2D& spatial,
    const SimStepView& step,
    const PhysicsConfig& cfg,
    std::span<const ContactManifold2D> prevManifolds,
    std::span<ContactManifold2D> outManifolds,
    std::span<ContactEvent2D> outEvents,
    u16& ioEventCount,
    u64& ioSpeculativeHitCount) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) == 0ULL) {
        return 0U;
    }
    const auto& colPool = world.GetPool<Collider2D>();
    const auto entities = colPool.Entities();
    const auto cols = colPool.Components();
    if (entities.empty()) {
        return 0U;
    }

    const bool hasBodies = (world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) != 0ULL;
    const f32 worldDt = step.Dt(TimeChannel::World);
    u16 manifoldCount = 0U;

    const auto findPrevAndWarmStart = [&](ContactManifold2D& m) noexcept -> bool {
        const u64 key = m.PairKey();
        const auto it = std::lower_bound(
            prevManifolds.begin(), prevManifolds.end(), key,
            [](const ContactManifold2D& a, u64 k) noexcept { return a.PairKey() < k; });
        if (it == prevManifolds.end() || it->PairKey() != key || it->isSensor != m.isSensor) {
            return false;
        }

        const ContactManifold2D& prev = *it;
        m.persistedTicks = static_cast<u16>(std::min<u32>(65535U, prev.persistedTicks + 1U));
        for (u8 p = 0U; p < m.pointCount; ++p) {
            u8 bestPrev = p;
            f32 bestDistSq = 1e9f;
            for (u8 q = 0U; q < prev.pointCount; ++q) {
                const f32 dx = m.pointX[p] - prev.pointX[q];
                const f32 dy = m.pointY[p] - prev.pointY[q];
                const f32 dSq = dx * dx + dy * dy;
                if (dSq < bestDistSq) {
                    bestDistSq = dSq;
                    bestPrev = q;
                }
            }
            if (bestPrev < prev.pointCount && (bestDistSq <= 0.08f * 0.08f || prev.pointCount == 1U)) {
                m.normalImpulse[p] = prev.normalImpulse[bestPrev] * cfg.warmStartFactor;
                m.tangentImpulse[p] = prev.tangentImpulse[bestPrev] * cfg.warmStartFactor;
            }
        }
        return true;
    };

    if (hasBodies && spatial.WidthTiles() > 0U && spatial.HeightTiles() > 0U) {
        auto& bodyPool = world.GetPool<RigidBody2D>();
        const f32 tileSize = spatial.TileSizeMeters();
        const f32 invTile = 1.0f / tileSize;
        const f32 origX = spatial.OriginX();
        const f32 origY = spatial.OriginY();

        for (usize i = 0U; i < entities.size() && manifoldCount < outManifolds.size(); ++i) {
            const EntityId idA = entities[i];
            const Collider2D& colA = cols[i];
            if (colA.IsSensor() || colA.shape == ShapeKind2D::Segment || (colA.filter.maskBits & 0x0001U) == 0U ||
                (world.GetComponentMaskByIndex(idA.index) & (1ULL << RigidBody2D::kTypeId)) == 0ULL) {
                continue;
            }

            RigidBody2D& bodyA = bodyPool.DirectAtEntityIndex(idA.index);
            if (!bodyA.IsDynamic() || !HasFlag(bodyA.flags, BodyFlags2D::Enabled)) {
                continue;
            }

            const WorldTransform2D& xfA = world.GetWorldByIndex(idA.index);
            const Vec2 centerA = xfA.TransformPoint({colA.localBounds.OffsetX(), colA.localBounds.OffsetY()});
            const f32 hAW = colA.localBounds.HalfW() * std::abs(xfA.scaleX);
            const f32 hAH = colA.localBounds.HalfH() * std::abs(xfA.scaleY);

            const bool isRotated = (colA.shape == ShapeKind2D::Obb || colA.shape == ShapeKind2D::CapsuleY);
            const f32 boundExtX = isRotated ? (hAW * std::abs(xfA.cosRot) + hAH * std::abs(xfA.sinRot)) : hAW;
            const f32 boundExtY = isRotated ? (hAW * std::abs(xfA.sinRot) + hAH * std::abs(xfA.cosRot)) : hAH;

            const f32 specMargin = bodyA.IsBullet()
                ? (std::sqrt(bodyA.vx * bodyA.vx + bodyA.vy * bodyA.vy) * worldDt + 0.05f)
                : 0.0f;

            const i32 minTX = std::max<i32>(0, static_cast<i32>(std::floor(((centerA.x - boundExtX - specMargin) - origX) * invTile)));
            const i32 maxTX = std::min<i32>(static_cast<i32>(spatial.WidthTiles()) - 1, static_cast<i32>(std::floor(((centerA.x + boundExtX + specMargin) - origX) * invTile)));
            const i32 minTY = std::max<i32>(0, static_cast<i32>(std::floor(((centerA.y - boundExtY - specMargin) - origY) * invTile)));
            const i32 maxTY = std::min<i32>(static_cast<i32>(spatial.HeightTiles()) - 1, static_cast<i32>(std::floor(((centerA.y + boundExtY + specMargin) - origY) * invTile)));

            for (i32 ty = minTY; ty <= maxTY; ++ty) {
                for (i32 tx = minTX; tx <= maxTX && manifoldCount < outManifolds.size(); ++tx) {
                    if (!HasFlag(spatial.SampleTileCoord(tx, ty), TileMaterialFlags::Solid)) {
                        continue;
                    }

                    const f32 tileCenterX = origX + (static_cast<f32>(tx) + 0.5f) * tileSize;
                    const f32 tileCenterY = origY + (static_cast<f32>(ty) + 0.5f) * tileSize;
                    const f32 tileHalf = 0.5f * tileSize;

                    ContactManifold2D m{
                        idA,
                        EntityId{static_cast<u16>(tx), static_cast<u16>(0x8000U | (static_cast<u32>(ty) & 0x7FFFU))},
                        0.0f, 0.0f, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f},
                        colA.friction_q16, colA.restitution_q16, 0U, 0U, 0U};

                    bool matched = false;
                    if (colA.shape == ShapeKind2D::Circle) {
                        matched = Narrowphase2D::CollideCircleAabb(
                            centerA.x, centerA.y, hAW, tileCenterX, tileCenterY, tileHalf, tileHalf, m, specMargin);
                    } else if (colA.shape == ShapeKind2D::CapsuleY) {
                        const f32 segHalf = std::max(0.0f, hAH - hAW);
                        const Vec2 axisY{-xfA.sinRot, xfA.cosRot};
                        const Vec2 p0 = centerA - axisY * segHalf;
                        const Vec2 p1 = centerA + axisY * segHalf;
                        const Vec2 tilePt{
                            std::clamp(centerA.x, tileCenterX - tileHalf, tileCenterX + tileHalf),
                            std::clamp(centerA.y, tileCenterY - tileHalf, tileCenterY + tileHalf)};
                        const Vec2 ab = p1 - p0;
                        const f32 lenSq = ab.LengthSq();
                        const Vec2 closest = (lenSq > 1e-8f)
                            ? (p0 + ab * std::clamp((tilePt - p0).Dot(ab) / lenSq, 0.0f, 1.0f))
                            : p0;
                        matched = Narrowphase2D::CollideCircleAabb(
                            closest.x, closest.y, hAW, tileCenterX, tileCenterY, tileHalf, tileHalf, m, specMargin);
                    } else if (colA.shape == ShapeKind2D::Obb) {
                        matched = Narrowphase2D::CollideObbObb(
                            centerA.x, centerA.y, hAW, hAH, xfA.cosRot, xfA.sinRot,
                            tileCenterX, tileCenterY, tileHalf, tileHalf, 1.0f, 0.0f, m, specMargin);
                    } else {
                        matched = Narrowphase2D::CollideAabbAabb(
                            centerA.x, centerA.y, hAW, hAH, tileCenterX, tileCenterY, tileHalf, tileHalf, m, specMargin);
                    }
                    if (!matched) {
                        continue;
                    }

                    if (m.separation[0] > 0.0f) {
                        if (bodyA.vx * m.normalX + bodyA.vy * m.normalY <= 0.0f) {
                            continue;
                        }
                        ++ioSpeculativeHitCount;
                    }

                    const bool existed = findPrevAndWarmStart(m);
                    outManifolds[manifoldCount++] = m;
                    if (!existed && m.separation[0] <= 0.0f) {
                        AppendContactEvent(outEvents, ioEventCount, ContactEvent2D{
                            m.entityA, kNullEntity, m.pointX[0], m.pointY[0],
                            m.normalX, m.normalY, 0.0f, ContactEventKind::BeginTouch, m.pointCount, colA.filter.materialTag});
                    }
                }
            }
        }
    }

    const auto activeBoxes = spatial.ActiveBoxes();
    const auto cellOffsets = spatial.CellOffsets();
    const auto cellIndices = spatial.CellBoxIndices();

    u64 pairBloom[64]{};
    const auto testAndSetPairBloom = [&](u64 key) noexcept -> bool {
        u64 h = (key ^ (key >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        h = (h ^ (h >> 27U)) * 0x94D049BB133111EBULL;
        h ^= (h >> 31U);
        const u32 b1 = static_cast<u32>(h) & 4095U;
        const u32 b2 = static_cast<u32>(h >> 12U) & 4095U;
        const u64 m1 = 1ULL << (b1 & 63U);
        const u64 m2 = 1ULL << (b2 & 63U);
        const bool seen = ((pairBloom[b1 >> 6U] & m1) != 0ULL) && ((pairBloom[b2 >> 6U] & m2) != 0ULL);
        pairBloom[b1 >> 6U] |= m1;
        pairBloom[b2 >> 6U] |= m2;
        return seen;
    };

    const u16 entityStartManifold = manifoldCount;

    const auto testBoxPair = [&](const SpatialBox2D& boxA, const SpatialBox2D& boxB) noexcept {
        if (manifoldCount >= outManifolds.size()) {
            return;
        }
        EntityId firstId = boxA.entity;
        EntityId secondId = boxB.entity;
        if (firstId == secondId) {
            return;
        }
        if (secondId.index < firstId.index) {
            std::swap(firstId, secondId);
        }

        const Collider2D& firstCol = colPool.DirectAtEntityIndex(firstId.index);
        const Collider2D& secondCol = colPool.DirectAtEntityIndex(secondId.index);
        if (!firstCol.filter.ShouldCollide(secondCol.filter)) {
            return;
        }

        const bool hasBody1 = hasBodies && ((world.GetComponentMaskByIndex(firstId.index) & (1ULL << RigidBody2D::kTypeId)) != 0ULL);
        const bool hasBody2 = hasBodies && ((world.GetComponentMaskByIndex(secondId.index) & (1ULL << RigidBody2D::kTypeId)) != 0ULL);
        const RigidBody2D* firstBody = hasBody1 ? &world.GetPool<RigidBody2D>().DirectAtEntityIndex(firstId.index) : nullptr;
        const RigidBody2D* secondBody = hasBody2 ? &world.GetPool<RigidBody2D>().DirectAtEntityIndex(secondId.index) : nullptr;
        if ((firstBody && !HasFlag(firstBody->flags, BodyFlags2D::Enabled)) ||
            (secondBody && !HasFlag(secondBody->flags, BodyFlags2D::Enabled))) {
            return;
        }

        const bool dyn1 = (firstBody != nullptr) && firstBody->IsDynamic();
        const bool dyn2 = (secondBody != nullptr) && secondBody->IsDynamic();
        if (!dyn1 && !dyn2 && !firstCol.IsSensor() && !secondCol.IsSensor()) {
            return;
        }
        if (dyn1 && dyn2 && firstBody->IsSleeping() && secondBody->IsSleeping()) {
            return;
        }

        f32 specMargin = 0.0f;
        if (!firstCol.IsSensor() && !secondCol.IsSensor()) {
            if (firstBody && firstBody->IsBullet()) {
                specMargin = std::max(specMargin, std::sqrt(firstBody->vx * firstBody->vx + firstBody->vy * firstBody->vy) * worldDt);
            }
            if (secondBody && secondBody->IsBullet()) {
                specMargin = std::max(specMargin, std::sqrt(secondBody->vx * secondBody->vx + secondBody->vy * secondBody->vy) * worldDt);
            }
        }

        if (boxA.BoundMaxX() + specMargin < boxB.BoundMinX() ||
            boxA.BoundMinX() - specMargin > boxB.BoundMaxX() ||
            boxA.BoundMaxY() + specMargin < boxB.BoundMinY() ||
            boxA.BoundMinY() - specMargin > boxB.BoundMaxY()) {
            return;
        }

        const u64 pairKey = (static_cast<u64>(firstId.Packed()) << 32U) | static_cast<u64>(secondId.Packed());
        if (testAndSetPairBloom(pairKey)) {
            for (u16 k = entityStartManifold; k < manifoldCount; ++k) {
                if (outManifolds[k].PairKey() == pairKey) {
                    return;
                }
            }
        }

        ContactManifold2D m{};
        if (!Narrowphase2D::CollideShapes(
                firstId, firstCol, world.GetWorldByIndex(firstId.index),
                secondId, secondCol, world.GetWorldByIndex(secondId.index), m, specMargin)) {
            return;
        }

        if (m.separation[0] > 0.0f) {
            const f32 v1x = firstBody ? firstBody->vx : 0.0f;
            const f32 v1y = firstBody ? firstBody->vy : 0.0f;
            const f32 v2x = secondBody ? secondBody->vx : 0.0f;
            const f32 v2y = secondBody ? secondBody->vy : 0.0f;
            if ((v2x - v1x) * m.normalX + (v2y - v1y) * m.normalY >= 0.0f) {
                return;
            }
            ++ioSpeculativeHitCount;
        }

        const bool existed = findPrevAndWarmStart(m);
        outManifolds[manifoldCount++] = m;

        if (!existed && m.separation[0] <= 0.0f) {
            const ContactEventKind kind =
                (m.isSensor != 0U) ? ContactEventKind::SensorEnter : ContactEventKind::BeginTouch;
            AppendContactEvent(outEvents, ioEventCount, ContactEvent2D{
                m.entityA, m.entityB, m.pointX[0], m.pointY[0],
                m.normalX, m.normalY, 0.0f, kind, m.pointCount, firstCol.filter.materialTag});
        }
    };

    for (u32 cell = 0U; cell < SpatialIndex2D::kGridCells; ++cell) {
        const u32 begin = cellOffsets[cell];
        const u32 end = cellOffsets[cell + 1U];
        if (end - begin < 2U) {
            continue;
        }

        for (u32 rA = begin; rA < end; ++rA) {
            const SpatialBox2D& boxA = activeBoxes[cellIndices[rA]];
            for (u32 rB = rA + 1U; rB < end && manifoldCount < outManifolds.size(); ++rB) {
                testBoxPair(boxA, activeBoxes[cellIndices[rB]]);
            }
        }
    }

    for (const u16 overIdx : spatial.OversizedBoxIndices()) {
        const SpatialBox2D& overBox = activeBoxes[overIdx];
        for (u16 bIdx = 0U; bIdx < activeBoxes.size() && manifoldCount < outManifolds.size(); ++bIdx) {
            if (bIdx != overIdx) {
                testBoxPair(overBox, activeBoxes[bIdx]);
            }
        }
    }

    const std::span<ContactManifold2D> currentSpan{outManifolds.data(), manifoldCount};
    std::sort(currentSpan.begin(), currentSpan.end(),
        [](const ContactManifold2D& a, const ContactManifold2D& b) noexcept {
            return a.PairKey() < b.PairKey();
        });

    for (const ContactManifold2D& prev : prevManifolds) {
        if (prev.separation[0] > 0.0f) {
            continue;
        }
        const u64 prevKey = prev.PairKey();
        const auto it = std::lower_bound(
            currentSpan.begin(), currentSpan.end(), prevKey,
            [](const ContactManifold2D& a, u64 k) noexcept { return a.PairKey() < k; });
        if (it != currentSpan.end() && it->PairKey() == prevKey && it->separation[0] <= 0.0f) {
            continue;
        }

        if (hasBodies && prev.isSensor == 0U) {
            auto& bodyPool = world.GetPool<RigidBody2D>();
            const auto wakeIfValid = [&](EntityId id) noexcept {
                if (id.IsValid() && !IsTileSentinelEntity(id) && world.IsAlive(id) &&
                    (world.GetComponentMaskByIndex(id.index) & (1ULL << RigidBody2D::kTypeId)) != 0ULL) {
                    bodyPool.DirectAtEntityIndex(id.index).WakeUp();
                }
            };
            wakeIfValid(prev.entityA);
            wakeIfValid(prev.entityB);
        }
        const ContactEventKind kind =
            (prev.isSensor != 0U) ? ContactEventKind::SensorExit : ContactEventKind::EndTouch;
        const EntityId bId = IsTileSentinelEntity(prev.entityB) ? kNullEntity : prev.entityB;
        AppendContactEvent(outEvents, ioEventCount, ContactEvent2D{
            prev.entityA, bId, prev.pointX[0], prev.pointY[0],
            prev.normalX, prev.normalY, 0.0f, kind, prev.pointCount, 0U});
    }

    return manifoldCount;
}

void DynamicsSolver2D::StepRigidBodiesSoftTgs(
    World& world,
    const SimStepView& step,
    const PhysicsConfig& cfg,
    std::span<ContactManifold2D> manifolds,
    std::span<ContactEvent2D> outEvents,
    u16& ioEventCount) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) == 0ULL) {
        return;
    }
    auto& bodyPool = world.GetPool<RigidBody2D>();
    const auto entities = bodyPool.Entities();
    auto bodies = bodyPool.Components();
    if (entities.empty()) {
        return;
    }

    const f32 dt = step.Dt(TimeChannel::World);
    if (dt <= 0.0f) {
        return;
    }
    const f32 invDt = 1.0f / dt;

    for (usize i = 0U; i < entities.size(); ++i) {
        RigidBody2D& b = bodies[i];
        b.contacts = ContactFlags::None;
        if (!b.IsDynamic() || !HasFlag(b.flags, BodyFlags2D::Enabled) || b.IsSleeping()) {
            continue;
        }
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(entities[i].index));
        if (step.IsFrozen(ch)) {
            continue;
        }

        if (HasFlag(b.flags, BodyFlags2D::UseGravity)) {
            b.vx += cfg.gravityX * b.gravityScale * dt;
            b.vy += cfg.gravityY * b.gravityScale * dt;
        }

        const f32 linDamp = b.LinearDamping();
        if (linDamp > 0.0f) {
            const f32 factor = 1.0f / (1.0f + dt * linDamp);
            b.vx *= factor;
            b.vy *= factor;
        }
        const f32 angDamp = b.AngularDamping();
        if (angDamp > 0.0f) {
            b.angularVel *= 1.0f / (1.0f + dt * angDamp);
        }
    }

    const f32 omega = kTwoPi * std::max(1.0f, cfg.contactHertz);
    const f32 a1 = 2.0f * cfg.contactDampingRatio + dt * omega;
    const f32 a2 = dt * omega * a1;
    const f32 a3 = 1.0f / (1.0f + a2);
    const f32 biasRate = omega / a1;
    const f32 massScale = a2 * a3;
    const f32 impulseScale = a3;

    const auto fetchBody = [&](EntityId id) noexcept -> RigidBody2D* {
        if (!id.IsValid() || IsTileSentinelEntity(id) || !world.IsAlive(id) ||
            (world.GetComponentMaskByIndex(id.index) & (1ULL << RigidBody2D::kTypeId)) == 0ULL) {
            return nullptr;
        }
        return &bodyPool.DirectAtEntityIndex(id.index);
    };

    const auto isBodySimAwake = [&](EntityId id, const RigidBody2D* b) noexcept -> bool {
        if (!b || !b->IsDynamic() || !HasFlag(b->flags, BodyFlags2D::Enabled) || b->IsSleeping()) {
            return false;
        }
        return !step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index)));
    };

    const auto bodyWorldCenter = [&](EntityId id) noexcept -> Vec2 {
        if (!id.IsValid() || IsTileSentinelEntity(id)) {
            return {0.0f, 0.0f};
        }
        const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        return {xf.x, xf.y};
    };

    for (ContactManifold2D& m : manifolds) {
        if (m.isSensor != 0U) {
            continue;
        }
        RigidBody2D* bA = fetchBody(m.entityA);
        RigidBody2D* bB = fetchBody(m.entityB);
        if (bA && bA->IsDynamic() && bA->IsSleeping() && IsActiveMover(bB)) {
            bA->WakeUp();
        }
        if (bB && bB->IsDynamic() && bB->IsSleeping() && IsActiveMover(bA)) {
            bB->WakeUp();
        }

        const bool awakeA = isBodySimAwake(m.entityA, bA);
        const bool awakeB = isBodySimAwake(m.entityB, bB);
        if (!awakeA && !awakeB) {
            continue;
        }

        const f32 invMA = awakeA ? bA->invMass : 0.0f;
        const f32 invMB = awakeB ? bB->invMass : 0.0f;
        const f32 invIA = awakeA ? bA->invInertia : 0.0f;
        const f32 invIB = awakeB ? bB->invInertia : 0.0f;
        if (invMA + invMB + invIA + invIB <= 0.0f) {
            continue;
        }

        const Vec2 cA = bodyWorldCenter(m.entityA);
        const Vec2 cB = bodyWorldCenter(m.entityB);
        const Vec2 n{m.normalX, m.normalY};
        const Vec2 t{-m.normalY, m.normalX};

        for (u8 p = 0U; p < m.pointCount; ++p) {
            const Vec2 pt{m.pointX[p], m.pointY[p]};
            const Vec2 P = n * m.normalImpulse[p] + t * m.tangentImpulse[p];
            if (awakeA) ApplyBodyImpulse(bA, P, pt - cA, -1.0f);
            if (awakeB) ApplyBodyImpulse(bB, P, pt - cB,  1.0f);
        }
    }

    const u32 iters = std::max<u32>(1U, cfg.velocityIterations);
    for (u32 iter = 0U; iter < iters; ++iter) {
        for (ContactManifold2D& m : manifolds) {
            if (m.isSensor != 0U) {
                continue;
            }
            RigidBody2D* bA = fetchBody(m.entityA);
            RigidBody2D* bB = fetchBody(m.entityB);
            const bool awakeA = isBodySimAwake(m.entityA, bA);
            const bool awakeB = isBodySimAwake(m.entityB, bB);
            if (!awakeA && !awakeB) {
                continue;
            }

            const f32 invMA = awakeA ? bA->invMass : 0.0f;
            const f32 invMB = awakeB ? bB->invMass : 0.0f;
            const f32 invIA = awakeA ? bA->invInertia : 0.0f;
            const f32 invIB = awakeB ? bB->invInertia : 0.0f;
            if (invMA + invMB + invIA + invIB <= 0.0f) {
                continue;
            }

            const Vec2 cA = bodyWorldCenter(m.entityA);
            const Vec2 cB = bodyWorldCenter(m.entityB);
            const Vec2 n{m.normalX, m.normalY};
            const Vec2 t{-m.normalY, m.normalX};
            const f32 friction = static_cast<f32>(m.friction_q16) * (1.0f / 65535.0f);
            const f32 restitution = static_cast<f32>(m.restitution_q16) * (1.0f / 65535.0f);

            for (u8 p = 0U; p < m.pointCount; ++p) {
                const Vec2 pt{m.pointX[p], m.pointY[p]};
                const Vec2 rA = pt - cA;
                const Vec2 rB = pt - cB;
                const f32 rnA = rA.Cross(n);
                const f32 rnB = rB.Cross(n);
                const f32 kNormal = invMA + invMB + rnA * rnA * invIA + rnB * rnB * invIB;
                const f32 effMassN = (kNormal > 1e-8f) ? (1.0f / kNormal) : 0.0f;

                const f32 vn = (BodyContactVelocity(bB, rB) - BodyContactVelocity(bA, rA)).Dot(n);
                const f32 sep = m.separation[p];

                f32 deltaJn = 0.0f;
                if (sep > 0.0f) {
                    deltaJn = -effMassN * (vn + sep * invDt);
                } else {
                    f32 bias = biasRate * sep;
                    if (restitution > 0.0f && vn < -0.25f && iter == 0U && m.persistedTicks == 0U) {
                        bias = std::min(bias, restitution * vn);
                        if (-vn > 2.5f) {
                            const EntityId hitB = IsTileSentinelEntity(m.entityB) ? kNullEntity : m.entityB;
                            AppendContactEvent(outEvents, ioEventCount, ContactEvent2D{
                                m.entityA, hitB, m.pointX[p], m.pointY[p],
                                n.x, n.y, -vn, ContactEventKind::HitImpact, m.pointCount, 0U});
                        }
                    }
                    deltaJn = -effMassN * massScale * (vn + bias) - impulseScale * m.normalImpulse[p];
                }

                const f32 newJn = std::max(m.normalImpulse[p] + deltaJn, 0.0f);
                const f32 appliedJn = newJn - m.normalImpulse[p];
                m.normalImpulse[p] = newJn;

                const Vec2 Pn = n * appliedJn;
                if (awakeA) ApplyBodyImpulse(bA, Pn, rA, -1.0f);
                if (awakeB) ApplyBodyImpulse(bB, Pn, rB,  1.0f);

                const f32 vt = (BodyContactVelocity(bB, rB) - BodyContactVelocity(bA, rA)).Dot(t);
                const f32 rtA = rA.Cross(t);
                const f32 rtB = rB.Cross(t);
                const f32 kTangent = invMA + invMB + rtA * rtA * invIA + rtB * rtB * invIB;
                const f32 effMassT = (kTangent > 1e-8f) ? (1.0f / kTangent) : 0.0f;

                const f32 maxFriction = friction * m.normalImpulse[p];
                const f32 newJt = std::clamp(m.tangentImpulse[p] - effMassT * vt, -maxFriction, maxFriction);
                const f32 appliedJt = newJt - m.tangentImpulse[p];
                m.tangentImpulse[p] = newJt;

                const Vec2 Pt = t * appliedJt;
                if (awakeA) ApplyBodyImpulse(bA, Pt, rA, -1.0f);
                if (awakeB) ApplyBodyImpulse(bB, Pt, rB,  1.0f);
            }

            if (m.separation[0] <= 1e-3f) {
                const auto recordContacts = [&](RigidBody2D* b, Vec2 normal) noexcept {
                    if (!b) return;
                    if (normal.y > cfg.minGroundNormalY) b->contacts |= ContactFlags::Ground;
                    else if (normal.y < -0.7f)           b->contacts |= ContactFlags::Ceiling;
                    if (normal.x > 0.7f)                 b->contacts |= ContactFlags::WallLeft;
                    else if (normal.x < -0.7f)           b->contacts |= ContactFlags::WallRight;
                };
                recordContacts(bA, -n);
                recordContacts(bB,  n);
            }
        }
    }

    for (const ContactManifold2D& m : manifolds) {
        if (m.isSensor != 0U || m.separation[0] >= -0.005f) {
            continue;
        }
        RigidBody2D* bA = fetchBody(m.entityA);
        RigidBody2D* bB = fetchBody(m.entityB);
        const bool awakeA = isBodySimAwake(m.entityA, bA);
        const bool awakeB = isBodySimAwake(m.entityB, bB);
        const f32 invMA = awakeA ? bA->invMass : 0.0f;
        const f32 invMB = awakeB ? bB->invMass : 0.0f;
        const f32 totalInv = invMA + invMB;
        if (totalInv <= 0.0f) {
            continue;
        }

        const f32 corr = std::min(0.25f * std::max(0.0f, -m.separation[0] - 0.002f), 0.15f) / totalInv;
        if (awakeA) {
            LocalTransform2D& locA = world.ModifyLocalTransformByIndex(m.entityA.index);
            locA.x -= m.normalX * (corr * invMA);
            locA.y -= m.normalY * (corr * invMA);
        }
        if (awakeB) {
            LocalTransform2D& locB = world.ModifyLocalTransformByIndex(m.entityB.index);
            locB.x += m.normalX * (corr * invMB);
            locB.y += m.normalY * (corr * invMB);
        }
    }

    constexpr f32 kSleepLinearSq = 0.02f * 0.02f;
    constexpr f32 kSleepAngular = 0.02f;

    for (usize i = 0U; i < entities.size(); ++i) {
        RigidBody2D& b = bodies[i];
        if (!HasFlag(b.flags, BodyFlags2D::Enabled) || !b.IsDynamic() || b.IsSleeping()) {
            continue;
        }
        const EntityId id = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) {
            continue;
        }

        if (b.vx != 0.0f || b.vy != 0.0f || b.angularVel != 0.0f) {
            LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
            loc.x += b.vx * dt;
            loc.y += b.vy * dt;
            if (!HasFlag(b.flags, BodyFlags2D::FixedRotation)) {
                loc.rotationRad += b.angularVel * dt;
            }
        }

        if (HasFlag(b.flags, BodyFlags2D::CanSleep)) {
            if (b.vx * b.vx + b.vy * b.vy <= kSleepLinearSq && std::abs(b.angularVel) <= kSleepAngular) {
                if (++b.sleepTimerTicks >= cfg.sleepTicksThreshold) {
                    b.vx = 0.0f;
                    b.vy = 0.0f;
                    b.angularVel = 0.0f;
                    b.flags |= BodyFlags2D::Sleeping;
                }
            } else {
                b.sleepTimerTicks = 0U;
            }
        }
    }
    world.PropagateTransforms();
}

} // namespace en
