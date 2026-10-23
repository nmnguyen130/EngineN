#include "physics/DynamicsSolver2D.h"
#include "physics/Narrowphase2D.h"

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

} // namespace

void DynamicsSolver2D::StepKinematicBodiesAndRiders(
    World& world,
    const SpatialIndex2D& spatial,
    const SimStepView& step) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) == 0ULL) return;
    auto& bodyPool = world.GetPool<RigidBody2D>();
    if (bodyPool.Size() == 0U) return;

    const auto bodyEntities = bodyPool.Entities();
    const auto bodies = bodyPool.Components();
    const bool hasCols = (world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) != 0ULL;
    const bool hasMotors = (world.RegisteredPoolsMask() & (1ULL << CharacterMotor2D::kTypeId)) != 0ULL;

    for (usize bIdx = 0U; bIdx < bodyEntities.size(); ++bIdx) {
        const RigidBody2D& body = bodies[bIdx];
        if (!body.IsKinematic() || !HasPhysicsFlag(body.flags, BodyFlags2D::Enabled)) continue;

        const EntityId carrierId = bodyEntities[bIdx];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(carrierId.index));
        if (step.IsFrozen(ch)) continue;
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) continue;

        const f32 dx = body.vx * dt;
        const f32 dy = body.vy * dt;
        const f32 dRot = body.angularVel * dt;
        if (std::abs(dx) <= 1e-7f && std::abs(dy) <= 1e-7f && std::abs(dRot) <= 1e-7f) continue;

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
        }
        const f32 cTopY = cCenterY + cHalfH;

        LocalTransform2D& cLoc = world.ModifyLocalTransformByIndex(carrierId.index);
        cLoc.x += dx;
        cLoc.y += dy;
        if (!HasPhysicsFlag(body.flags, BodyFlags2D::FixedRotation)) {
            cLoc.rotationRad += dRot;
        }

        if (!hasMotors || !hasCol || (std::abs(dx) <= 1e-7f && std::abs(dy) <= 1e-7f)) continue;
        auto& motorPool = world.GetPool<CharacterMotor2D>();
        const auto motorEntities = motorPool.Entities();
        auto motors = motorPool.Components();

        for (usize mIdx = 0U; mIdx < motorEntities.size(); ++mIdx) {
            CharacterMotor2D& motor = motors[mIdx];
            if (!HasPhysicsFlag(motor.flags, BodyFlags2D::Enabled)) continue;
            const EntityId riderId = motorEntities[mIdx];
            const WorldTransform2D& rXf = world.GetWorldByIndex(riderId.index);
            const f32 rHalfW = motor.bounds.HalfW() * std::abs(rXf.scaleX);
            const f32 rHalfH = motor.bounds.HalfH() * std::abs(rXf.scaleY);
            const f32 rCenterX = rXf.x + motor.bounds.OffsetX() * rXf.scaleX;
            const f32 rCenterY = rXf.y + motor.bounds.OffsetY() * rXf.scaleY;
            const f32 rBottomY = rCenterY - rHalfH;

            const bool isLatchedRider = (motor.carrierId == carrierId);
            const bool isStandingOnTop =
                (rCenterX + rHalfW > cCenterX - cHalfW) &&
                (rCenterX - rHalfW < cCenterX + cHalfW) &&
                (std::abs(rBottomY - cTopY) <= 0.06f) &&
                (motor.vy <= 0.0f);

            if (!isLatchedRider && !isStandingOnTop) continue;

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
    world.PropagateTransforms();
}

void DynamicsSolver2D::StepCharacterMotors(
    World& world,
    const SpatialIndex2D& spatial,
    const SimStepView& step,
    const PhysicsConfig& cfg) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) return;
    auto& motorPool = world.GetPool<CharacterMotor2D>();
    const auto entities = motorPool.Entities();
    auto motors = motorPool.Components();

    for (usize i = 0U; i < entities.size(); ++i) {
        CharacterMotor2D& motor = motors[i];
        if (!HasPhysicsFlag(motor.flags, BodyFlags2D::Enabled)) continue;

        const EntityId id = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) continue;
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) continue;

        const bool ignoreOneWay = HasPhysicsFlag(motor.flags, BodyFlags2D::IgnoreOneWay);
        const bool wasGrounded = HasPhysicsFlag(motor.contacts, ContactFlags::Ground);
        const ContactFlags preservedCrush = motor.contacts & ContactFlags::Crushed;
        motor.contacts = preservedCrush;

        if (HasPhysicsFlag(motor.flags, BodyFlags2D::UseGravity)) {
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

        // 3-iteration Collide-and-Slide (MoveAndSlide) with arbitrary slope tangent projection and stair step-up
        for (u32 iter = 0U; iter < 3U; ++iter) {
            if (std::abs(remDX) <= 1e-6f && std::abs(remDY) <= 1e-6f) break;

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

            // Stair / ledge step-up check when hitting a vertical wall horizontally
            if (std::abs(hit.normalX) > 0.9f && cfg.maxStepHeight > 1e-3f && (wasGrounded || motor.vy <= 0.0f)) {
                const SweepHit2D upHit = spatial.SweepAabb(
                    id, curCenterX, curCenterY, halfW, halfH, 0.0f, cfg.maxStepHeight, motor.solidMask, true);
                if (!upHit.Hit()) {
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
                            motor.contacts |= ContactFlags::Ground;
                            motor.groundNormalX = downHit.normalX;
                            motor.groundNormalY = downHit.normalY;
                            remDX -= stepAdvance;
                            remDY = 0.0f;
                            continue;
                        }
                    }
                }
            }

            if (hit.normalY >= cfg.minGroundNormalY) {
                motor.contacts |= ContactFlags::Ground;
                motor.groundNormalX = hit.normalX;
                motor.groundNormalY = hit.normalY;
                if (hit.normalY < 0.999f) {
                    motor.contacts |= ContactFlags::OnSlope;
                }
                if (hit.isOneWay != 0U) {
                    motor.contacts |= ContactFlags::OnOneWay;
                }
                motor.carrierId = hit.hitEntity;

                // Project remaining horizontal displacement along the surface tangent without losing horizontal speed
                if (std::abs(hit.normalY) > 1e-4f) {
                    remDY = (-hit.normalX / hit.normalY) * remDX;
                } else {
                    remDY = 0.0f;
                }
                if (motor.vy < 0.0f) motor.vy = 0.0f;
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

        // Downward ground/slope snapping when walking down a ramp
        if (!HasPhysicsFlag(motor.contacts, ContactFlags::Ground) &&
            wasGrounded && motor.vy <= 0.0f && cfg.groundSnapDist > 0.0f) {
            const SweepHit2D snapHit = spatial.SweepAabb(
                id, curCenterX, curCenterY, halfW, halfH, 0.0f, -cfg.groundSnapDist, motor.solidMask, ignoreOneWay);
            if (snapHit.Hit() && snapHit.normalY >= cfg.minGroundNormalY && snapHit.toi > 0.0f && snapHit.toi < 1.0f) {
                curCenterY -= cfg.groundSnapDist * std::max(0.0f, snapHit.toi - 1e-4f);
                motor.contacts |= ContactFlags::Ground;
                if (snapHit.normalY < 0.999f) {
                    motor.contacts |= ContactFlags::OnSlope;
                }
                motor.groundNormalX = snapHit.normalX;
                motor.groundNormalY = snapHit.normalY;
                motor.carrierId = snapHit.hitEntity;
                motor.vy = 0.0f;
            }
        }

        if (!HasPhysicsFlag(motor.contacts, ContactFlags::Ground)) {
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
    if ((world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) == 0ULL) return 0U;
    const auto& colPool = world.GetPool<Collider2D>();
    const auto entities = colPool.Entities();
    const auto cols = colPool.Components();
    if (entities.empty()) return 0U;

    const bool hasBodies = (world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) != 0ULL;
    u16 manifoldCount = 0U;

    const auto findPrevAndWarmStart = [&](ContactManifold2D& m) noexcept -> bool {
        const u64 key = m.PairKey();
        for (const ContactManifold2D& prev : prevManifolds) {
            if (prev.PairKey() == key && prev.isSensor == m.isSensor) {
                m.persistedTicks = static_cast<u8>(std::min<u32>(255U, prev.persistedTicks + 1U));
                for (u8 p = 0U; p < m.pointCount && p < prev.pointCount; ++p) {
                    m.normalImpulse[p] = prev.normalImpulse[p] * cfg.warmStartFactor;
                    m.tangentImpulse[p] = prev.tangentImpulse[p] * cfg.warmStartFactor;
                }
                return true;
            }
        }
        return false;
    };

    // 1. Dynamic RigidBody2D vs Static Tilemap (including Speculative Contacts for BulletCCD bodies)
    if (hasBodies && spatial.WidthTiles() > 0U && spatial.HeightTiles() > 0U) {
        auto& bodyPool = world.GetPool<RigidBody2D>();
        const f32 tileSize = spatial.TileSizeMeters();
        const f32 invTile = 1.0f / tileSize;
        const f32 origX = spatial.OriginX();
        const f32 origY = spatial.OriginY();

        for (usize i = 0U; i < entities.size(); ++i) {
            if (manifoldCount >= outManifolds.size()) break;
            const EntityId idA = entities[i];
            const Collider2D& colA = cols[i];
            if (colA.IsSensor() || colA.shape == ShapeKind2D::Segment || (colA.filter.maskBits & 0x0001U) == 0U) continue;
            if ((world.GetComponentMaskByIndex(idA.index) & (1ULL << RigidBody2D::kTypeId)) == 0ULL) continue;

            RigidBody2D& bodyA = bodyPool.DirectAtEntityIndex(idA.index);
            if (!bodyA.IsDynamic() || !HasPhysicsFlag(bodyA.flags, BodyFlags2D::Enabled)) continue;

            const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(idA.index));
            const f32 dt = step.Dt(ch);
            const WorldTransform2D& xfA = world.GetWorldByIndex(idA.index);
            const Vec2 centerA = xfA.TransformPoint({colA.localBounds.OffsetX(), colA.localBounds.OffsetY()});
            const f32 hAW = colA.localBounds.HalfW() * std::abs(xfA.scaleX);
            const f32 hAH = colA.localBounds.HalfH() * std::abs(xfA.scaleY);

            const f32 specMargin = bodyA.IsBullet()
                ? (std::sqrt(bodyA.vx * bodyA.vx + bodyA.vy * bodyA.vy) * dt + 0.05f)
                : 0.0f;

            const i32 minTX = std::max<i32>(0, static_cast<i32>(std::floor(((centerA.x - hAW - specMargin) - origX) * invTile)));
            const i32 maxTX = std::min<i32>(static_cast<i32>(spatial.WidthTiles()) - 1, static_cast<i32>(std::floor(((centerA.x + hAW + specMargin) - origX) * invTile)));
            const i32 minTY = std::max<i32>(0, static_cast<i32>(std::floor(((centerA.y - hAH - specMargin) - origY) * invTile)));
            const i32 maxTY = std::min<i32>(static_cast<i32>(spatial.HeightTiles()) - 1, static_cast<i32>(std::floor(((centerA.y + hAH + specMargin) - origY) * invTile)));

            for (i32 ty = minTY; ty <= maxTY; ++ty) {
                for (i32 tx = minTX; tx <= maxTX; ++tx) {
                    if (manifoldCount >= outManifolds.size()) break;
                    const TileMaterialFlags tFlags = spatial.SampleTileCoord(tx, ty);
                    if (!HasPhysicsFlag(tFlags, TileMaterialFlags::Solid)) continue;

                    const f32 tileCenterX = origX + (static_cast<f32>(tx) + 0.5f) * tileSize;
                    const f32 tileCenterY = origY + (static_cast<f32>(ty) + 0.5f) * tileSize;
                    const f32 tileHalf = 0.5f * tileSize;

                    ContactManifold2D m{};
                    m.entityA = idA;
                    m.entityB = EntityId{static_cast<u16>(((ty & 0x7F) << 8) | (tx & 0xFF)), 0xFFFFU};
                    m.friction_q16 = colA.friction_q16;
                    m.restitution_q16 = colA.restitution_q16;

                    bool matched = false;
                    if (colA.shape == ShapeKind2D::Circle) {
                        matched = Narrowphase2D::CollideCircleAabb(
                            centerA.x, centerA.y, hAW, tileCenterX, tileCenterY, tileHalf, tileHalf, m, specMargin);
                    } else if (colA.shape == ShapeKind2D::Obb) {
                        matched = Narrowphase2D::CollideObbObb(
                            centerA.x, centerA.y, hAW, hAH, xfA.cosRot, xfA.sinRot,
                            tileCenterX, tileCenterY, tileHalf, tileHalf, 1.0f, 0.0f, m, specMargin);
                    } else {
                        matched = Narrowphase2D::CollideAabbAabb(
                            centerA.x, centerA.y, hAW, hAH, tileCenterX, tileCenterY, tileHalf, tileHalf, m, specMargin);
                    }
                    if (!matched) continue;

                    if (m.separation[0] > 0.0f) {
                        const f32 vn = bodyA.vx * m.normalX + bodyA.vy * m.normalY;
                        if (vn <= 0.0f) continue;
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

    // 2. Broadphase cell-bucket pair generation over SpatialIndex2D (eliminates O(N^2) all-pairs scan)
    const auto activeBoxes = spatial.ActiveBoxes();
    const auto cellOffsets = spatial.CellOffsets();
    const auto cellIndices = spatial.CellBoxIndices();
    const u16 entityStartManifold = manifoldCount;

    for (u32 cell = 0U; cell < SpatialIndex2D::kGridCells; ++cell) {
        const u32 begin = cellOffsets[cell];
        const u32 end = cellOffsets[cell + 1U];
        if (end - begin < 2U) continue;

        for (u32 rA = begin; rA < end; ++rA) {
            const SpatialBox2D& boxA = activeBoxes[cellIndices[rA]];
            const EntityId idA = boxA.entity;

            for (u32 rB = rA + 1U; rB < end; ++rB) {
                if (manifoldCount >= outManifolds.size()) break;
                const SpatialBox2D& boxB = activeBoxes[cellIndices[rB]];
                const EntityId idB = boxB.entity;
                if (idA == idB) continue;

                // Canonicalize entity ordering so PairKey() is deterministic
                EntityId firstId = idA;
                EntityId secondId = idB;
                if (secondId.index < firstId.index) {
                    std::swap(firstId, secondId);
                }

                const Collider2D& firstCol = colPool.DirectAtEntityIndex(firstId.index);
                const Collider2D& secondCol = colPool.DirectAtEntityIndex(secondId.index);
                if (!firstCol.filter.ShouldCollide(secondCol.filter)) continue;

                const bool hasBody1 = hasBodies && ((world.GetComponentMaskByIndex(firstId.index) & (1ULL << RigidBody2D::kTypeId)) != 0ULL);
                const bool hasBody2 = hasBodies && ((world.GetComponentMaskByIndex(secondId.index) & (1ULL << RigidBody2D::kTypeId)) != 0ULL);
                const RigidBody2D* firstBody = hasBody1 ? &world.GetPool<RigidBody2D>().DirectAtEntityIndex(firstId.index) : nullptr;
                const RigidBody2D* secondBody = hasBody2 ? &world.GetPool<RigidBody2D>().DirectAtEntityIndex(secondId.index) : nullptr;
                const bool dyn1 = (firstBody != nullptr) && firstBody->IsDynamic();
                const bool dyn2 = (secondBody != nullptr) && secondBody->IsDynamic();

                if (!dyn1 && !dyn2 && !firstCol.IsSensor() && !secondCol.IsSensor()) continue;
                if (dyn1 && dyn2 && firstBody->IsSleeping() && secondBody->IsSleeping()) continue;

                // Deduplicate pairs that span multiple shared spatial grid cells
                const u64 pairKey = (static_cast<u64>(firstId.Packed()) << 32U) | static_cast<u64>(secondId.Packed());
                bool alreadyTested = false;
                for (u16 k = entityStartManifold; k < manifoldCount; ++k) {
                    if (outManifolds[k].PairKey() == pairKey) {
                        alreadyTested = true;
                        break;
                    }
                }
                if (alreadyTested) continue;

                f32 specMargin = 0.0f;
                if (!firstCol.IsSensor() && !secondCol.IsSensor()) {
                    if (firstBody && firstBody->IsBullet()) {
                        const f32 dt1 = step.Dt(static_cast<TimeChannel>(world.GetTimeChannelByIndex(firstId.index)));
                        specMargin = std::max(specMargin, std::sqrt(firstBody->vx * firstBody->vx + firstBody->vy * firstBody->vy) * dt1);
                    }
                    if (secondBody && secondBody->IsBullet()) {
                        const f32 dt2 = step.Dt(static_cast<TimeChannel>(world.GetTimeChannelByIndex(secondId.index)));
                        specMargin = std::max(specMargin, std::sqrt(secondBody->vx * secondBody->vx + secondBody->vy * secondBody->vy) * dt2);
                    }
                }

                if (boxA.BoundMaxX() + specMargin < boxB.BoundMinX() ||
                    boxA.BoundMinX() - specMargin > boxB.BoundMaxX() ||
                    boxA.BoundMaxY() + specMargin < boxB.BoundMinY() ||
                    boxA.BoundMinY() - specMargin > boxB.BoundMaxY()) {
                    continue;
                }

                const WorldTransform2D& xf1 = world.GetWorldByIndex(firstId.index);
                const WorldTransform2D& xf2 = world.GetWorldByIndex(secondId.index);

                ContactManifold2D m{};
                if (!Narrowphase2D::CollideShapes(
                        firstId, firstCol, xf1, secondId, secondCol, xf2, m, specMargin)) {
                    continue;
                }

                if (m.separation[0] > 0.0f) {
                    const f32 v1x = firstBody ? firstBody->vx : 0.0f;
                    const f32 v1y = firstBody ? firstBody->vy : 0.0f;
                    const f32 v2x = secondBody ? secondBody->vx : 0.0f;
                    const f32 v2y = secondBody ? secondBody->vy : 0.0f;
                    const f32 relVn = (v2x - v1x) * m.normalX + (v2y - v1y) * m.normalY;
                    if (relVn >= 0.0f) continue;
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
            }
        }
    }

    // 3. Emit EndTouch / SensorExit for previous manifolds that disappeared this tick
    const std::span<const ContactManifold2D> currentSpan{outManifolds.data(), manifoldCount};
    for (const ContactManifold2D& prev : prevManifolds) {
        if (prev.separation[0] > 0.0f) continue;
        const u64 prevKey = prev.PairKey();
        bool stillPresent = false;
        for (const ContactManifold2D& cur : currentSpan) {
            if (cur.PairKey() == prevKey && cur.separation[0] <= 0.0f) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent) {
            const ContactEventKind kind =
                (prev.isSensor != 0U) ? ContactEventKind::SensorExit : ContactEventKind::EndTouch;
            const EntityId bId = (prev.entityB.generation == 0xFFFFU) ? kNullEntity : prev.entityB;
            AppendContactEvent(outEvents, ioEventCount, ContactEvent2D{
                prev.entityA, bId, prev.pointX[0], prev.pointY[0],
                prev.normalX, prev.normalY, 0.0f, kind, prev.pointCount, 0U});
        }
    }

    return manifoldCount;
}

void DynamicsSolver2D::StepRigidBodiesAndJointsSoftTgs(
    World& world,
    const SimStepView& step,
    const PhysicsConfig& cfg,
    std::span<ContactManifold2D> manifolds,
    std::span<ContactEvent2D> outEvents,
    u16& ioEventCount) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << RigidBody2D::kTypeId)) == 0ULL) return;
    auto& bodyPool = world.GetPool<RigidBody2D>();
    const auto entities = bodyPool.Entities();
    auto bodies = bodyPool.Components();
    if (entities.empty()) return;

    const f32 dt = step.Dt(TimeChannel::World);
    if (dt <= 0.0f) return;
    const f32 invDt = 1.0f / dt;

    // 1. Integrate gravity and exponential linear/angular damping for awake dynamic bodies
    for (usize i = 0U; i < entities.size(); ++i) {
        RigidBody2D& b = bodies[i];
        b.contacts = ContactFlags::None;
        if (!b.IsDynamic() || !HasPhysicsFlag(b.flags, BodyFlags2D::Enabled) || b.IsSleeping()) {
            continue;
        }
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(entities[i].index));
        if (step.IsFrozen(ch)) continue;
        const f32 bodyDt = step.Dt(ch);

        if (HasPhysicsFlag(b.flags, BodyFlags2D::UseGravity)) {
            b.vx += cfg.gravityX * b.gravityScale * bodyDt;
            b.vy += cfg.gravityY * b.gravityScale * bodyDt;
        }

        const f32 linDamp = b.LinearDamping();
        if (linDamp > 0.0f) {
            const f32 factor = 1.0f / (1.0f + bodyDt * linDamp);
            b.vx *= factor;
            b.vy *= factor;
        }
        const f32 angDamp = b.AngularDamping();
        if (angDamp > 0.0f) {
            b.angularVel *= 1.0f / (1.0f + bodyDt * angDamp);
        }
    }

    // 2. Box2D v3 Soft-Step harmonic oscillator parameters
    const f32 omega = kTwoPi * std::max(1.0f, cfg.contactHertz);
    const f32 a1 = 2.0f * cfg.contactDampingRatio + dt * omega;
    const f32 a2 = dt * omega * a1;
    const f32 a3 = 1.0f / (1.0f + a2);
    const f32 biasRate = omega / a1;
    const f32 massScale = a2 * a3;
    const f32 impulseScale = a3;

    const auto fetchBody = [&](EntityId id) noexcept -> RigidBody2D* {
        if (!id.IsValid() || id.generation == 0xFFFFU || !world.IsAlive(id)) return nullptr;
        if ((world.GetComponentMaskByIndex(id.index) & (1ULL << RigidBody2D::kTypeId)) == 0ULL) return nullptr;
        return &bodyPool.DirectAtEntityIndex(id.index);
    };

    const auto bodyWorldCenter = [&](EntityId id) noexcept -> Vec2 {
        if (!id.IsValid() || id.generation == 0xFFFFU) return {0.0f, 0.0f};
        const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        return {xf.x, xf.y};
    };

    // 3. Apply warm-started contact impulses (both linear and rotational lever-arm torque)
    for (ContactManifold2D& m : manifolds) {
        if (m.isSensor != 0U) continue;
        RigidBody2D* bA = fetchBody(m.entityA);
        RigidBody2D* bB = fetchBody(m.entityB);
        if (bA && bA->IsDynamic() && (!bB || !bB->IsSleeping())) bA->WakeUp();
        if (bB && bB->IsDynamic() && (!bA || !bA->IsSleeping())) bB->WakeUp();

        const f32 invMA = (bA && bA->IsDynamic()) ? bA->invMass : 0.0f;
        const f32 invMB = (bB && bB->IsDynamic()) ? bB->invMass : 0.0f;
        const f32 invIA = (bA && bA->IsDynamic()) ? bA->invInertia : 0.0f;
        const f32 invIB = (bB && bB->IsDynamic()) ? bB->invInertia : 0.0f;
        if (invMA + invMB + invIA + invIB <= 0.0f) continue;

        const Vec2 cA = bodyWorldCenter(m.entityA);
        const Vec2 cB = bodyWorldCenter(m.entityB);
        const Vec2 n{m.normalX, m.normalY};
        const Vec2 t{-m.normalY, m.normalX};

        for (u8 p = 0U; p < m.pointCount; ++p) {
            const Vec2 pt{m.pointX[p], m.pointY[p]};
            const Vec2 rA = pt - cA;
            const Vec2 rB = pt - cB;
            const Vec2 P = n * m.normalImpulse[p] + t * m.tangentImpulse[p];
            if (bA && bA->IsDynamic()) {
                bA->vx -= P.x * invMA;
                bA->vy -= P.y * invMA;
                bA->angularVel -= rA.Cross(P) * invIA;
            }
            if (bB && bB->IsDynamic()) {
                bB->vx += P.x * invMB;
                bB->vy += P.y * invMB;
                bB->angularVel += rB.Cross(P) * invIB;
            }
        }
    }

    // Warm-start DistanceJoint2D constraints
    const bool hasJoints = (world.RegisteredPoolsMask() & (1ULL << DistanceJoint2D::kTypeId)) != 0ULL;
    if (hasJoints) {
        auto& jointPool = world.GetPool<DistanceJoint2D>();
        auto joints = jointPool.Components();
        for (DistanceJoint2D& j : joints) {
            if (j.enabled == 0U) continue;
            RigidBody2D* bA = fetchBody(j.entityA);
            RigidBody2D* bB = fetchBody(j.entityB);
            if (!bA && !bB) continue;
            if (bA && bA->IsDynamic()) bA->WakeUp();
            if (bB && bB->IsDynamic()) bB->WakeUp();

            j.impulse *= cfg.warmStartFactor;
            if (std::abs(j.impulse) <= 1e-7f) continue;

            const WorldTransform2D& xfA = world.GetWorldByIndex(j.entityA.index);
            const WorldTransform2D& xfB = world.GetWorldByIndex(j.entityB.index);
            const Vec2 pA = xfA.TransformPoint(j.LocalAnchorA());
            const Vec2 pB = xfB.TransformPoint(j.LocalAnchorB());
            const Vec2 rA = pA - Vec2{xfA.x, xfA.y};
            const Vec2 rB = pB - Vec2{xfB.x, xfB.y};
            const Vec2 u = (pB - pA).Normalized();
            const Vec2 P = u * j.impulse;

            if (bA && bA->IsDynamic()) {
                bA->vx -= P.x * bA->invMass;
                bA->vy -= P.y * bA->invMass;
                bA->angularVel -= rA.Cross(P) * bA->invInertia;
            }
            if (bB && bB->IsDynamic()) {
                bB->vx += P.x * bB->invMass;
                bB->vy += P.y * bB->invMass;
                bB->angularVel += rB.Cross(P) * bB->invInertia;
            }
        }
    }

    // 4. Sequential Impulse Soft-Step velocity iterations (with full rotational lever arms + joints)
    const u32 iters = std::max<u32>(1U, cfg.velocityIterations);
    for (u32 iter = 0U; iter < iters; ++iter) {
        // 4a. Solve DistanceJoint2D constraints
        if (hasJoints) {
            auto& jointPool = world.GetPool<DistanceJoint2D>();
            auto joints = jointPool.Components();
            for (DistanceJoint2D& j : joints) {
                if (j.enabled == 0U || !world.IsAlive(j.entityA) || !world.IsAlive(j.entityB)) continue;
                RigidBody2D* bA = fetchBody(j.entityA);
                RigidBody2D* bB = fetchBody(j.entityB);
                const f32 invMA = (bA && bA->IsDynamic()) ? bA->invMass : 0.0f;
                const f32 invMB = (bB && bB->IsDynamic()) ? bB->invMass : 0.0f;
                const f32 invIA = (bA && bA->IsDynamic()) ? bA->invInertia : 0.0f;
                const f32 invIB = (bB && bB->IsDynamic()) ? bB->invInertia : 0.0f;
                if (invMA + invMB + invIA + invIB <= 0.0f) continue;

                const WorldTransform2D& xfA = world.GetWorldByIndex(j.entityA.index);
                const WorldTransform2D& xfB = world.GetWorldByIndex(j.entityB.index);
                const Vec2 pA = xfA.TransformPoint(j.LocalAnchorA());
                const Vec2 pB = xfB.TransformPoint(j.LocalAnchorB());
                const Vec2 d = pB - pA;
                const f32 curLen = d.Length();
                const Vec2 u = (curLen > 1e-6f) ? (d * (1.0f / curLen)) : Vec2{1.0f, 0.0f};
                const Vec2 rA = pA - Vec2{xfA.x, xfA.y};
                const Vec2 rB = pB - Vec2{xfB.x, xfB.y};
                const f32 crAu = rA.Cross(u);
                const f32 crBu = rB.Cross(u);
                const f32 invK = invMA + invMB + crAu * crAu * invIA + crBu * crBu * invIB;
                if (invK <= 1e-8f) continue;
                const f32 effMass = 1.0f / invK;

                const Vec2 vA = (bA ? Vec2{bA->vx, bA->vy} : Vec2{}) + CrossScalarVec(bA ? bA->angularVel : 0.0f, rA);
                const Vec2 vB = (bB ? Vec2{bB->vx, bB->vy} : Vec2{}) + CrossScalarVec(bB ? bB->angularVel : 0.0f, rB);
                const f32 vn = (vB - vA).Dot(u);

                f32 deltaJ = 0.0f;
                if (j.hertz_q6 > 0U) {
                    const f32 jOmega = kTwoPi * j.Hertz();
                    const f32 jZeta = j.DampingRatio();
                    const f32 ja1 = 2.0f * jZeta + dt * jOmega;
                    const f32 ja2 = dt * jOmega * ja1;
                    const f32 ja3 = 1.0f / (1.0f + ja2);
                    const f32 jBias = (jOmega / ja1) * (curLen - j.minLength);
                    deltaJ = -effMass * (ja2 * ja3) * (vn + jBias) - ja3 * j.impulse;
                    j.impulse += deltaJ;
                } else {
                    if (curLen < j.minLength) {
                        const f32 bias = biasRate * (curLen - j.minLength);
                        deltaJ = -effMass * (vn + bias);
                        const f32 prevJ = j.impulse;
                        j.impulse = std::min(prevJ + deltaJ, 0.0f);
                        deltaJ = j.impulse - prevJ;
                    } else if (curLen > j.maxLength) {
                        const f32 bias = biasRate * (curLen - j.maxLength);
                        deltaJ = -effMass * (vn + bias);
                        const f32 prevJ = j.impulse;
                        j.impulse = std::max(prevJ + deltaJ, 0.0f);
                        deltaJ = j.impulse - prevJ;
                    }
                }

                const Vec2 P = u * deltaJ;
                if (bA && bA->IsDynamic()) {
                    bA->vx -= P.x * invMA;
                    bA->vy -= P.y * invMA;
                    bA->angularVel -= rA.Cross(P) * invIA;
                }
                if (bB && bB->IsDynamic()) {
                    bB->vx += P.x * invMB;
                    bB->vy += P.y * invMB;
                    bB->angularVel += rB.Cross(P) * invIB;
                }
            }
        }

        // 4b. Solve ContactManifold2D rotational + linear constraints
        for (ContactManifold2D& m : manifolds) {
            if (m.isSensor != 0U) continue;
            RigidBody2D* bA = fetchBody(m.entityA);
            RigidBody2D* bB = fetchBody(m.entityB);
            const f32 invMA = (bA && bA->IsDynamic()) ? bA->invMass : 0.0f;
            const f32 invMB = (bB && bB->IsDynamic()) ? bB->invMass : 0.0f;
            const f32 invIA = (bA && bA->IsDynamic()) ? bA->invInertia : 0.0f;
            const f32 invIB = (bB && bB->IsDynamic()) ? bB->invInertia : 0.0f;
            if (invMA + invMB + invIA + invIB <= 0.0f) continue;

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

                const Vec2 vA = (bA ? Vec2{bA->vx, bA->vy} : Vec2{}) + CrossScalarVec(bA ? bA->angularVel : 0.0f, rA);
                const Vec2 vB = (bB ? Vec2{bB->vx, bB->vy} : Vec2{}) + CrossScalarVec(bB ? bB->angularVel : 0.0f, rB);
                const f32 vn = (vB - vA).Dot(n);
                const f32 sep = m.separation[p];

                f32 deltaJn = 0.0f;
                if (sep > 0.0f) {
                    const f32 specBias = sep * invDt;
                    deltaJn = -effMassN * (vn + specBias);
                } else {
                    f32 bias = biasRate * sep;
                    if (restitution > 0.0f && vn < -1.0f && iter == 0U && m.persistedTicks == 0U) {
                        bias = std::min(bias, restitution * vn);
                        if (-vn > 2.5f) {
                            const EntityId hitB = (m.entityB.generation == 0xFFFFU) ? kNullEntity : m.entityB;
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
                if (bA && bA->IsDynamic()) {
                    bA->vx -= Pn.x * invMA;
                    bA->vy -= Pn.y * invMA;
                    bA->angularVel -= rA.Cross(Pn) * invIA;
                }
                if (bB && bB->IsDynamic()) {
                    bB->vx += Pn.x * invMB;
                    bB->vy += Pn.y * invMB;
                    bB->angularVel += rB.Cross(Pn) * invIB;
                }

                // Re-evaluate relative tangential velocity and apply rotational Coulomb friction
                const Vec2 vA2 = (bA ? Vec2{bA->vx, bA->vy} : Vec2{}) + CrossScalarVec(bA ? bA->angularVel : 0.0f, rA);
                const Vec2 vB2 = (bB ? Vec2{bB->vx, bB->vy} : Vec2{}) + CrossScalarVec(bB ? bB->angularVel : 0.0f, rB);
                const f32 vt = (vB2 - vA2).Dot(t);
                const f32 rtA = rA.Cross(t);
                const f32 rtB = rB.Cross(t);
                const f32 kTangent = invMA + invMB + rtA * rtA * invIA + rtB * rtB * invIB;
                const f32 effMassT = (kTangent > 1e-8f) ? (1.0f / kTangent) : 0.0f;

                const f32 deltaJt = -effMassT * vt;
                const f32 maxFriction = friction * m.normalImpulse[p];
                const f32 newJt = std::clamp(m.tangentImpulse[p] + deltaJt, -maxFriction, maxFriction);
                const f32 appliedJt = newJt - m.tangentImpulse[p];
                m.tangentImpulse[p] = newJt;

                const Vec2 Pt = t * appliedJt;
                if (bA && bA->IsDynamic()) {
                    bA->vx -= Pt.x * invMA;
                    bA->vy -= Pt.y * invMA;
                    bA->angularVel -= rA.Cross(Pt) * invIA;
                }
                if (bB && bB->IsDynamic()) {
                    bB->vx += Pt.x * invMB;
                    bB->vy += Pt.y * invMB;
                    bB->angularVel += rB.Cross(Pt) * invIB;
                }
            }

            if (m.separation[0] <= 1e-3f) {
                if (bA) {
                    if (n.y < -cfg.minGroundNormalY) bA->contacts |= ContactFlags::Ground;
                    else if (n.y > 0.7f)             bA->contacts |= ContactFlags::Ceiling;
                    if (n.x < -0.7f)                 bA->contacts |= ContactFlags::WallLeft;
                    else if (n.x > 0.7f)             bA->contacts |= ContactFlags::WallRight;
                }
                if (bB) {
                    if (n.y > cfg.minGroundNormalY)  bB->contacts |= ContactFlags::Ground;
                    else if (n.y < -0.7f)            bB->contacts |= ContactFlags::Ceiling;
                    if (n.x > 0.7f)                  bB->contacts |= ContactFlags::WallLeft;
                    else if (n.x < -0.7f)            bB->contacts |= ContactFlags::WallRight;
                }
            }
        }
    }

    // 5. Residual positional split for deep initial overlaps + integrate dynamic velocities & update sleeping
    for (const ContactManifold2D& m : manifolds) {
        if (m.isSensor != 0U || m.separation[0] >= -0.005f) continue;
        RigidBody2D* bA = fetchBody(m.entityA);
        RigidBody2D* bB = fetchBody(m.entityB);
        const f32 invMA = (bA && bA->IsDynamic()) ? bA->invMass : 0.0f;
        const f32 invMB = (bB && bB->IsDynamic()) ? bB->invMass : 0.0f;
        const f32 totalInv = invMA + invMB;
        if (totalInv <= 0.0f) continue;

        const f32 slop = 0.002f;
        const f32 corr = std::min(0.25f * std::max(0.0f, -m.separation[0] - slop), 0.15f) / totalInv;
        if (bA && bA->IsDynamic()) {
            LocalTransform2D& locA = world.ModifyLocalTransformByIndex(m.entityA.index);
            locA.x -= m.normalX * (corr * invMA);
            locA.y -= m.normalY * (corr * invMA);
        }
        if (bB && bB->IsDynamic()) {
            LocalTransform2D& locB = world.ModifyLocalTransformByIndex(m.entityB.index);
            locB.x += m.normalX * (corr * invMB);
            locB.y += m.normalY * (corr * invMB);
        }
    }

    constexpr f32 kSleepLinearSq = 0.02f * 0.02f;
    constexpr f32 kSleepAngular = 0.02f;

    for (usize i = 0U; i < entities.size(); ++i) {
        RigidBody2D& b = bodies[i];
        if (!HasPhysicsFlag(b.flags, BodyFlags2D::Enabled) || !b.IsDynamic() || b.IsSleeping()) {
            continue;
        }
        const EntityId id = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) continue;
        const f32 bodyDt = step.Dt(ch);

        if (b.vx != 0.0f || b.vy != 0.0f || b.angularVel != 0.0f) {
            LocalTransform2D& loc = world.ModifyLocalTransformByIndex(id.index);
            loc.x += b.vx * bodyDt;
            loc.y += b.vy * bodyDt;
            if (!HasPhysicsFlag(b.flags, BodyFlags2D::FixedRotation)) {
                loc.rotationRad += b.angularVel * bodyDt;
            }
        }

        if (HasPhysicsFlag(b.flags, BodyFlags2D::CanSleep)) {
            const f32 speedSq = b.vx * b.vx + b.vy * b.vy;
            if (speedSq <= kSleepLinearSq && std::abs(b.angularVel) <= kSleepAngular) {
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
