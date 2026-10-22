#include "gameplay/MotionSolver2D.h"

namespace en {

void MotionSolver2D::SyncPostPhysicsContactsAndTimers(
    World& world,
    const PhysicsSystem& physics,
    const SimStepView& step,
    u8 defaultCoyoteTicks) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << MotionStack2D::kTypeId)) == 0ULL ||
        (regMask & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) {
        return;
    }

    auto& motionPool = world.GetPool<MotionStack2D>();
    auto& motorPool = world.GetPool<CharacterMotor2D>();
    const bool hasAnchors = (regMask & (1ULL << SafeAnchor2D::kTypeId)) != 0ULL;
    const auto entities = motionPool.Entities();
    auto stacks = motionPool.Components();

    for (usize i = 0U; i < entities.size(); ++i) {
        const EntityId id = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) continue;
        const u64 entMask = world.GetComponentMaskByIndex(id.index);
        if ((entMask & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) continue;

        MotionStack2D& stack = stacks[i];
        CharacterMotor2D& motor = motorPool.DirectAtEntityIndex(id.index);

        // 1. Update one-way platform drop-through timer and sync BodyFlags2D::IgnoreOneWay on motor
        if (stack.oneWayDropTicks > 0U) {
            --stack.oneWayDropTicks;
            motor.flags |= BodyFlags2D::IgnoreOneWay;
        } else {
            motor.flags &= ~BodyFlags2D::IgnoreOneWay;
        }

        // 2. Clamp walk/knockback horizontal speed if blocked by a solid wall
        if (HasPhysicsFlag(motor.contacts, ContactFlags::WallLeft)) {
            if (stack.currentWalkVx < 0.0f) stack.currentWalkVx = 0.0f;
            if (stack.impulseVx < 0.0f)     stack.impulseVx = 0.0f;
            stack.wallCoyoteTicks = defaultCoyoteTicks;
            stack.wallContactSign = -1;
        } else if (HasPhysicsFlag(motor.contacts, ContactFlags::WallRight)) {
            if (stack.currentWalkVx > 0.0f) stack.currentWalkVx = 0.0f;
            if (stack.impulseVx > 0.0f)     stack.impulseVx = 0.0f;
            stack.wallCoyoteTicks = defaultCoyoteTicks;
            stack.wallContactSign = 1;
        } else if (stack.wallCoyoteTicks > 0U) {
            --stack.wallCoyoteTicks;
        } else {
            stack.wallContactSign = 0;
        }

        // 3. Refresh or decrement ground coyote grace timer
        if (motor.IsGrounded()) {
            stack.groundCoyoteTicks = defaultCoyoteTicks;
            stack.flags &= ~(MotionFlags::AirborneDash | MotionFlags::JumpCutApplied);
        } else if (stack.groundCoyoteTicks > 0U) {
            --stack.groundCoyoteTicks;
        }

        // 4. Verify 3-ray safe ground anchor when standing on stable non-one-way static ground
        if (hasAnchors && motor.IsGrounded() &&
            !HasPhysicsFlag(motor.contacts, ContactFlags::OnOneWay | ContactFlags::OnSlope) &&
            !motor.carrierId.IsValid() &&
            (entMask & (1ULL << SafeAnchor2D::kTypeId)) != 0ULL) {
            const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
            const f32 halfW = motor.bounds.HalfW() * std::abs(xf.scaleX);
            const f32 halfH = motor.bounds.HalfH() * std::abs(xf.scaleY);
            const f32 cx = xf.x + motor.bounds.OffsetX() * xf.scaleX;
            const f32 bottomY = (xf.y + motor.bounds.OffsetY() * xf.scaleY) - halfH + 0.05f;
            const f32 probeDist = 0.20f;

            const RayHit2D rLeft   = physics.Raycast(cx - halfW * 0.8f, bottomY, 0.0f, -1.0f, probeDist, motor.solidMask, id);
            const RayHit2D rCenter = physics.Raycast(cx,                bottomY, 0.0f, -1.0f, probeDist, motor.solidMask, id);
            const RayHit2D rRight  = physics.Raycast(cx + halfW * 0.8f, bottomY, 0.0f, -1.0f, probeDist, motor.solidMask, id);

            if (rLeft.Hit() && rCenter.Hit() && rRight.Hit() &&
                !rLeft.hitEntity.IsValid() && !rCenter.hitEntity.IsValid() && !rRight.hitEntity.IsValid()) {
                SafeAnchor2D& anchor = world.GetPool<SafeAnchor2D>().DirectAtEntityIndex(id.index);
                anchor.prevSafeX = anchor.safeX;
                anchor.prevSafeY = anchor.safeY;
                anchor.safeX = xf.x;
                anchor.safeY = xf.y;
            }
        }
    }
}

void MotionSolver2D::BlendMotionStacksIntoMotors(
    World& world,
    const SimStepView& step,
    const GameplayConfig& cfg) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << MotionStack2D::kTypeId)) == 0ULL ||
        (regMask & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) {
        return;
    }

    auto& motionPool = world.GetPool<MotionStack2D>();
    auto& motorPool = world.GetPool<CharacterMotor2D>();
    const bool hasFsm = (regMask & (1ULL << StateMachine2D::kTypeId)) != 0ULL;
    const auto entities = motionPool.Entities();
    auto stacks = motionPool.Components();

    for (usize i = 0U; i < entities.size(); ++i) {
        const EntityId id = entities[i];
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
        if (step.IsFrozen(ch)) continue;
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) continue;

        const u64 entMask = world.GetComponentMaskByIndex(id.index);
        if ((entMask & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) continue;

        CharacterMotor2D& motor = motorPool.DirectAtEntityIndex(id.index);

        // Freeze motor motion while entity is in per-entity combat hitstop
        if (hasFsm && (entMask & (1ULL << StateMachine2D::kTypeId)) != 0ULL) {
            const StateMachine2D& fsm = world.GetPool<StateMachine2D>().DirectAtEntityIndex(id.index);
            if (fsm.hitstopTicks > 0U) {
                motor.vx = 0.0f;
                motor.vy = 0.0f;
                motor.flags &= ~BodyFlags2D::UseGravity;
                continue;
            }
            motor.flags |= BodyFlags2D::UseGravity;
        }

        MotionStack2D& stack = stacks[i];
        const f32 invDt = 1.0f / dt;

        // 1. Consume animation root-motion displacement into an instantaneous frame velocity
        const f32 rootVx = stack.RootMotionDx() * invDt;
        const f32 rootVy = stack.RootMotionDy() * invDt;
        stack.rootMotionDx_q8 = 0;
        stack.rootMotionDy_q8 = 0;

        // 2. Consume one-shot vertical launch (jump or vertical knockback) cleanly without multi-tick accumulation
        if (stack.pendingLaunchVy != 0.0f) {
            motor.vy = stack.pendingLaunchVy;
            stack.pendingLaunchVy = 0.0f;
        }

        // 3. Variable jump height: apply early-release upward velocity cut once when JumpHeld is cleared while ascending
        if (!motor.IsGrounded() && motor.vy > 0.0f &&
            !HasPhysicsFlag(stack.flags, MotionFlags::JumpHeld) &&
            !HasPhysicsFlag(stack.flags, MotionFlags::JumpCutApplied) &&
            stack.overrideTicks == 0U) {
            motor.vy *= cfg.jumpCutMultiplier;
            stack.flags |= MotionFlags::JumpCutApplied;
        }

        // 4. Priority velocity override (dash / air-stall / lunge) vs accelerated walk locomotion
        if (stack.overrideTicks > 0U) {
            --stack.overrideTicks;
            motor.vx = stack.OverrideVx() + rootVx + stack.impulseVx;
            motor.vy = stack.OverrideVy() + rootVy;
            if (stack.overrideTicks == 0U) {
                stack.activePriority = 0U;
            }
        } else {
            const f32 accel = motor.IsGrounded() ? cfg.groundAccel : cfg.airAccel;
            const f32 maxStep = accel * dt;
            const f32 diff = stack.targetWalkVx - stack.currentWalkVx;
            stack.currentWalkVx += std::clamp(diff, -maxStep, maxStep);
            motor.vx = stack.currentWalkVx + rootVx + stack.impulseVx;
            if (rootVy != 0.0f) {
                motor.vy = rootVy;
            }
        }

        // 5. Exponentially decay horizontal knockback impulse
        const f32 decay = 1.0f / (1.0f + dt * cfg.knockbackDecayRate);
        stack.impulseVx *= decay;
        if (std::abs(stack.impulseVx) < 0.02f) stack.impulseVx = 0.0f;
    }
}

void MotionSolver2D::ResolveSoftCharacterPushboxes(
    World& world,
    const PhysicsSystem& physics,
    const SimStepView& step,
    const GameplayConfig& cfg) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << MotionStack2D::kTypeId)) == 0ULL ||
        (regMask & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) {
        return;
    }

    auto& motionPool = world.GetPool<MotionStack2D>();
    if (motionPool.Size() < 2U) return;
    auto& motorPool = world.GetPool<CharacterMotor2D>();
    const auto entities = motionPool.Entities();
    const auto stacks = motionPool.Components();

    for (usize i = 0U; i < entities.size(); ++i) {
        const EntityId idA = entities[i];
        const MotionStack2D& sA = stacks[i];
        if (sA.pushboxGroup == 0U) continue;
        if ((world.GetComponentMaskByIndex(idA.index) & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) continue;
        const CharacterMotor2D& mA = motorPool.DirectAtEntityIndex(idA.index);
        if (!HasPhysicsFlag(mA.flags, BodyFlags2D::Enabled)) continue;

        const WorldTransform2D& xfA = world.GetWorldByIndex(idA.index);
        const f32 cAX = xfA.x + mA.bounds.OffsetX() * xfA.scaleX;
        const f32 cAY = xfA.y + mA.bounds.OffsetY() * xfA.scaleY;
        const f32 hAW = mA.bounds.HalfW() * std::abs(xfA.scaleX);
        const f32 hAH = mA.bounds.HalfH() * std::abs(xfA.scaleY);

        for (usize j = i + 1U; j < entities.size(); ++j) {
            const EntityId idB = entities[j];
            const MotionStack2D& sB = stacks[j];
            if (sB.pushboxGroup != sA.pushboxGroup) continue;
            if ((world.GetComponentMaskByIndex(idB.index) & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) continue;
            const CharacterMotor2D& mB = motorPool.DirectAtEntityIndex(idB.index);
            if (!HasPhysicsFlag(mB.flags, BodyFlags2D::Enabled)) continue;

            const WorldTransform2D& xfB = world.GetWorldByIndex(idB.index);
            const f32 cBX = xfB.x + mB.bounds.OffsetX() * xfB.scaleX;
            const f32 cBY = xfB.y + mB.bounds.OffsetY() * xfB.scaleY;
            const f32 hBW = mB.bounds.HalfW() * std::abs(xfB.scaleX);
            const f32 hBH = mB.bounds.HalfH() * std::abs(xfB.scaleY);

            const f32 overlapX = (hAW + hBW) - std::abs(cBX - cAX);
            const f32 overlapY = (hAH + hBH) - std::abs(cBY - cAY);
            if (overlapX <= 0.0f || overlapY <= 0.0f) continue;

            const f32 dt = step.Dt(TimeChannel::World);
            const f32 dir = (cBX >= cAX) ? 1.0f : -1.0f;
            const f32 push = std::min(overlapX * 0.5f, cfg.pushboxSeparationSpeed * dt);

            const SweepHit2D hitA = physics.SweepAabb(idA, cAX, cAY, hAW, hAH, -dir * push, 0.0f, mA.solidMask, true);
            const SweepHit2D hitB = physics.SweepAabb(idB, cBX, cBY, hBW, hBH,  dir * push, 0.0f, mB.solidMask, true);

            if (!hitA.Hit()) world.ModifyLocalTransformByIndex(idA.index).x -= dir * push;
            if (!hitB.Hit()) world.ModifyLocalTransformByIndex(idB.index).x += dir * push;
        }
    }
    world.PropagateTransforms();
}

} // namespace en
