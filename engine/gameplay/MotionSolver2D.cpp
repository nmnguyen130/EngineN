#include "gameplay/MotionSolver2D.h"

namespace en {

void MotionSolver2D::Sync(
    World& world,
    const PhysicsSystem& physics,
    const SimStepView& step,
    u8 coyoteTicks) noexcept {
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
        if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index)))) {
            continue;
        }
        if ((world.GetComponentMaskByIndex(id.index) & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) {
            continue;
        }

        MotionStack2D& stack = stacks[i];
        CharacterMotor2D& motor = motorPool.DirectAtEntityIndex(id.index);

        if (stack.oneWayDropTicks > 0U) {
            --stack.oneWayDropTicks;
            motor.flags |= BodyFlags2D::IgnoreOneWay;
        } else {
            motor.flags &= ~BodyFlags2D::IgnoreOneWay;
        }

        const i8 wallSign = HasFlag(motor.contacts, ContactFlags::WallLeft)
            ? static_cast<i8>(-1)
            : (HasFlag(motor.contacts, ContactFlags::WallRight) ? static_cast<i8>(1) : static_cast<i8>(0));

        if (wallSign != 0) {
            const f32 fSign = static_cast<f32>(wallSign);
            if (stack.currentWalkVx * fSign > 0.0f) stack.currentWalkVx = 0.0f;
            if (stack.impulseVx * fSign > 0.0f) stack.impulseVx = 0.0f;
            stack.wallCoyoteTicks = coyoteTicks;
            stack.wallContactSign = wallSign;
        } else if (stack.wallCoyoteTicks > 0U) {
            --stack.wallCoyoteTicks;
        } else {
            stack.wallContactSign = 0;
        }

        if (motor.IsGrounded()) {
            stack.groundCoyoteTicks = coyoteTicks;
            stack.flags &= ~(MotionFlags::AirborneDash | MotionFlags::JumpCutApplied);
        } else if (stack.groundCoyoteTicks > 0U) {
            --stack.groundCoyoteTicks;
        }

        if (hasAnchors && motor.IsGrounded() &&
            !HasFlag(motor.contacts, ContactFlags::OnOneWay) &&
            !motor.carrierId.IsValid() &&
            (world.GetComponentMaskByIndex(id.index) & (1ULL << SafeAnchor2D::kTypeId)) != 0ULL) {
            const WorldTransform2D& xf = world.GetWorldByIndex(id.index);
            const f32 halfW = motor.bounds.HalfW() * std::abs(xf.scaleX);
            const f32 halfH = motor.bounds.HalfH() * std::abs(xf.scaleY);
            const f32 cx = xf.x + motor.bounds.OffsetX() * xf.scaleX;
            const f32 bottomY = (xf.y + motor.bounds.OffsetY() * xf.scaleY) - halfH + 0.05f;
            constexpr f32 kProbe = 0.25f;

            if (physics.Raycast(cx - halfW * 0.8f, bottomY, 0.0f, -1.0f, kProbe, motor.solidMask, id, false).Hit() &&
                physics.Raycast(cx, bottomY, 0.0f, -1.0f, kProbe, motor.solidMask, id, false).Hit() &&
                physics.Raycast(cx + halfW * 0.8f, bottomY, 0.0f, -1.0f, kProbe, motor.solidMask, id, false).Hit()) {
                SafeAnchor2D& anchor = world.GetPool<SafeAnchor2D>().DirectAtEntityIndex(id.index);
                const f32 dx = xf.x - anchor.safeX;
                const f32 dy = xf.y - anchor.safeY;
                if (dx * dx + dy * dy >= 0.0625f) {
                    anchor.prevSafeX = anchor.safeX;
                    anchor.prevSafeY = anchor.safeY;
                }
                anchor.safeX = xf.x;
                anchor.safeY = xf.y;
            }
        }
    }
}

void MotionSolver2D::Blend(
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
        if ((world.GetComponentMaskByIndex(id.index) & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) {
            continue;
        }

        MotionStack2D& stack = stacks[i];
        CharacterMotor2D& motor = motorPool.DirectAtEntityIndex(id.index);

        if (hasFsm && (world.GetComponentMaskByIndex(id.index) & (1ULL << StateMachine2D::kTypeId)) != 0ULL) {
            const StateMachine2D& fsm = world.GetPool<StateMachine2D>().DirectAtEntityIndex(id.index);
            if (fsm.hitstopTicks > 0U) {
                // Preserve vertical velocity across hitstop so jump arcs resume cleanly.
                if (motor.vy != 0.0f && stack.pendingLaunchVy == 0.0f) {
                    stack.pendingLaunchVy = motor.vy;
                }
                motor.vx = 0.0f;
                motor.vy = 0.0f;
                motor.flags &= ~BodyFlags2D::UseGravity;
                continue;
            }
            motor.flags |= BodyFlags2D::UseGravity;
        }

        const f32 invDt = 1.0f / dt;
        const f32 rootVx = stack.RootMotionDx() * invDt;
        const f32 rootVy = stack.RootMotionDy() * invDt;
        stack.rootMotionDx_q8 = 0;
        stack.rootMotionDy_q8 = 0;

        if (stack.pendingLaunchVy != 0.0f) {
            motor.vy = stack.pendingLaunchVy;
            stack.pendingLaunchVy = 0.0f;
        }

        if (!motor.IsGrounded() && motor.vy > 0.0f &&
            !HasFlag(stack.flags, MotionFlags::JumpHeld) &&
            !HasFlag(stack.flags, MotionFlags::JumpCutApplied) &&
            stack.overrideTicks == 0U) {
            motor.vy *= cfg.jumpCutMultiplier;
            stack.flags |= MotionFlags::JumpCutApplied;
        }

        if (stack.overrideTicks > 0U) {
            --stack.overrideTicks;
            motor.vx = stack.OverrideVx() + rootVx + stack.impulseVx;
            motor.vy = stack.OverrideVy() + rootVy;
            if (stack.overrideTicks == 0U) stack.activePriority = 0U;
        } else {
            const f32 maxStep = (motor.IsGrounded() ? cfg.groundAccel : cfg.airAccel) * dt;
            stack.currentWalkVx += std::clamp(stack.targetWalkVx - stack.currentWalkVx, -maxStep, maxStep);
            motor.vx = stack.currentWalkVx + rootVx + stack.impulseVx;
            if (rootVy != 0.0f) motor.vy = rootVy;
        }

        stack.impulseVx *= 1.0f / (1.0f + dt * cfg.knockbackDecayRate);
        if (std::abs(stack.impulseVx) < 0.02f) stack.impulseVx = 0.0f;
    }
}

void MotionSolver2D::Separate(
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
    const f32 dt = step.Dt(TimeChannel::World);
    bool anyPushed = false;

    for (usize i = 0U; i < entities.size(); ++i) {
        const EntityId idA = entities[i];
        const MotionStack2D& sA = stacks[i];
        if (sA.pushboxGroup == 0U ||
            step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(idA.index))) ||
            (world.GetComponentMaskByIndex(idA.index) & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) {
            continue;
        }
        const CharacterMotor2D& mA = motorPool.DirectAtEntityIndex(idA.index);
        if (!HasFlag(mA.flags, BodyFlags2D::Enabled)) continue;

        const WorldTransform2D& xfA = world.GetWorldByIndex(idA.index);
        const f32 cAX = xfA.x + mA.bounds.OffsetX() * xfA.scaleX;
        const f32 cAY = xfA.y + mA.bounds.OffsetY() * xfA.scaleY;
        const f32 hAW = mA.bounds.HalfW() * std::abs(xfA.scaleX);
        const f32 hAH = mA.bounds.HalfH() * std::abs(xfA.scaleY);

        for (usize j = i + 1U; j < entities.size(); ++j) {
            const EntityId idB = entities[j];
            const MotionStack2D& sB = stacks[j];
            if (sB.pushboxGroup != sA.pushboxGroup ||
                step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(idB.index))) ||
                (world.GetComponentMaskByIndex(idB.index) & (1ULL << CharacterMotor2D::kTypeId)) == 0ULL) {
                continue;
            }
            const CharacterMotor2D& mB = motorPool.DirectAtEntityIndex(idB.index);
            if (!HasFlag(mB.flags, BodyFlags2D::Enabled)) continue;

            const WorldTransform2D& xfB = world.GetWorldByIndex(idB.index);
            const f32 cBX = xfB.x + mB.bounds.OffsetX() * xfB.scaleX;
            const f32 cBY = xfB.y + mB.bounds.OffsetY() * xfB.scaleY;
            const f32 hBW = mB.bounds.HalfW() * std::abs(xfB.scaleX);
            const f32 hBH = mB.bounds.HalfH() * std::abs(xfB.scaleY);

            const f32 overlapX = (hAW + hBW) - std::abs(cBX - cAX);
            const f32 overlapY = (hAH + hBH) - std::abs(cBY - cAY);
            if (overlapX <= 0.0f || overlapY <= 0.0f) continue;

            const f32 dir = (std::abs(cBX - cAX) > 1e-5f)
                ? ((cBX > cAX) ? 1.0f : -1.0f)
                : ((idB.index > idA.index) ? 1.0f : -1.0f);
            const f32 push = std::min(overlapX * 0.5f, cfg.pushboxSeparationSpeed * dt);

            if (!physics.SweepAabb(idA, cAX, cAY, hAW, hAH, -dir * push, 0.0f, mA.solidMask, true).Hit()) {
                world.ModifyLocalTransformByIndex(idA.index).x -= dir * push;
                anyPushed = true;
            }
            if (!physics.SweepAabb(idB, cBX, cBY, hBW, hBH, dir * push, 0.0f, mB.solidMask, true).Hit()) {
                world.ModifyLocalTransformByIndex(idB.index).x += dir * push;
                anyPushed = true;
            }
        }
    }
    if (anyPushed) world.PropagateTransforms();
}

} // namespace en
