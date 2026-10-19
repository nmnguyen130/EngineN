#include "game/movement/TraversalSystem.h"

#include <algorithm>
#include <cmath>

namespace en::veil {

void TraversalSystem::Step(
    gameplay::KinematicMotor2D& motor,
    gameplay::MotionStack2D& motion,
    TraversalMutationComponent& traversal,
    const ResonanceLanternComponent& lantern,
    f32 moveInputX,
    bool jumpJustPressed,
    bool evadeJustPressed,
    world::LocalTransform2D* optTransform,
    const world::LocalTransform2D* optTargetTransform) noexcept {
    if (traversal.traversalCooldownTicks > 0) {
        --traversal.traversalCooldownTicks;
    }
    if (traversal.invulnerableTicks > 0) {
        --traversal.invulnerableTicks;
    }

    if (optTransform != nullptr) {
        // Floor boundary clamp: player feet (offset 1.0m) cannot drop below ground y = 0.0m
        if (optTransform->y < 1.001f) {
            optTransform->y = 1.001f;
            motor.vy = std::max(0.0f, motor.vy);
            motor.contacts |= gameplay::ContactFlags::Ground;
        }
        // Arena left and right wall clamps (-19.0m to +27.0m)
        if (optTransform->x < -19.0f) {
            optTransform->x = -19.0f;
            motor.vx = std::max(0.0f, motor.vx);
        } else if (optTransform->x > 27.0f) {
            optTransform->x = 27.0f;
            motor.vx = std::min(0.0f, motor.vx);
        }
    }

    const EchoSpec& spec = GetEchoSpec(lantern.LeadEcho());
    const bool isGrounded = gameplay::HasContact(motor.contacts, gameplay::ContactFlags::Ground);

    if (evadeJustPressed && traversal.traversalCooldownTicks == 0) {
        if (isGrounded || traversal.airDashesRemaining > 0) {
            if (!isGrounded) {
                --traversal.airDashesRemaining;
            }
            ExecuteTraversalMutation(motor, motion, traversal, spec, optTransform, optTargetTransform);
            return;
        }
    }

    if (traversal.traversalActiveTicks > 0) {
        --traversal.traversalActiveTicks;
        return;
    }

    motion.targetWalkVx = moveInputX * kMaxRunSpeed;
    if (moveInputX > 0.05f) {
        traversal.facingRight = true;
        motion.SetFacingSign(1);
    } else if (moveInputX < -0.05f) {
        traversal.facingRight = false;
        motion.SetFacingSign(-1);
    }

    if (isGrounded) {
        traversal.coyoteTicksRemaining = kCoyoteWindowTicks;
        traversal.airJumpsRemaining = 1;
        traversal.airDashesRemaining = 1;
    } else if (traversal.coyoteTicksRemaining > 0) {
        --traversal.coyoteTicksRemaining;
    }

    if (jumpJustPressed) {
        if (isGrounded || traversal.coyoteTicksRemaining > 0) {
            motor.contacts &= ~gameplay::ContactFlags::Ground;
            traversal.coyoteTicksRemaining = 0;
            motor.vy = kJumpVelocity;
        } else if (traversal.airJumpsRemaining > 0) {
            --traversal.airJumpsRemaining;
            motor.vy = kJumpVelocity * 0.92f;
        }
    }
}

void TraversalSystem::ExecuteTraversalMutation(
    gameplay::KinematicMotor2D& motor,
    gameplay::MotionStack2D& motion,
    TraversalMutationComponent& traversal,
    const EchoSpec& spec,
    world::LocalTransform2D* optTransform,
    const world::LocalTransform2D* optTargetTransform) noexcept {
    traversal.activeTraversal = spec.traversal;
    traversal.traversalActiveTicks = spec.traversalDurationTicks;
    traversal.traversalCooldownTicks = spec.traversalCooldownTicks;
    const f32 dir = traversal.facingRight ? 1.0f : -1.0f;

    switch (spec.traversal) {
        case TraversalType::BlazeJetDash:
            motor.vx = dir * spec.traversalSpeed;
            motor.vy = 0.0f;
            motion.PushOverride(2, dir * spec.traversalSpeed, 0.0f, spec.traversalDurationTicks, 0);
            traversal.invulnerableTicks = 8;
            break;

        case TraversalType::TectonicQuakePlunge:
            motor.contacts &= ~gameplay::ContactFlags::Ground;
            motor.vx = dir * 4.0f;
            motor.vy = -spec.traversalSpeed;
            motion.PushOverride(2, dir * 4.0f, -spec.traversalSpeed, spec.traversalDurationTicks, 256);
            traversal.invulnerableTicks = 6;
            break;

        case TraversalType::PiezoBlinkStep:
            if (optTransform != nullptr) {
                optTransform->x += dir * 4.5f;
            }
            motor.vx = dir * 12.0f;
            motor.vy = 0.0f;
            motion.PushOverride(3, dir * 12.0f, 0.0f, 1, 0);
            traversal.traversalActiveTicks = 1;
            traversal.invulnerableTicks = 10;
            break;

        case TraversalType::CymaticIceVault:
            motor.contacts &= ~gameplay::ContactFlags::Ground;
            motor.vx = dir * 6.0f;
            motor.vy = spec.traversalSpeed;
            motion.PushOverride(2, dir * 6.0f, spec.traversalSpeed, spec.traversalDurationTicks, 128);
            traversal.airJumpsRemaining = static_cast<u8>(traversal.airJumpsRemaining + 1);
            break;

        case TraversalType::SlipstreamUpdraft:
            motor.contacts &= ~gameplay::ContactFlags::Ground;
            motor.vx = dir * 5.0f;
            motor.vy = spec.traversalSpeed;
            motion.PushOverride(2, dir * 5.0f, spec.traversalSpeed, spec.traversalDurationTicks, 96);
            break;

        case TraversalType::CavitationWireSwing:
            motor.contacts &= ~gameplay::ContactFlags::Ground;
            motor.vx = dir * spec.traversalSpeed;
            motor.vy = 8.5f;
            motion.PushOverride(2, dir * spec.traversalSpeed, 8.5f, spec.traversalDurationTicks, 192);
            break;

        case TraversalType::PhotonRayGrapple:
            motor.contacts &= ~gameplay::ContactFlags::Ground;
            motor.vx = dir * spec.traversalSpeed;
            motor.vy = 6.0f;
            motion.PushOverride(3, dir * spec.traversalSpeed, 6.0f, spec.traversalDurationTicks, 0);
            traversal.invulnerableTicks = 8;
            break;

        case TraversalType::NullPhaseStep:
            motor.vx = dir * spec.traversalSpeed;
            motor.vy = 0.0f;
            motion.PushOverride(3, dir * spec.traversalSpeed, 0.0f, spec.traversalDurationTicks, 0);
            traversal.invulnerableTicks = 12;
            break;

        case TraversalType::GravitonSlingshot:
            motor.contacts &= ~gameplay::ContactFlags::Ground;
            motor.vx = dir * 11.0f;
            motor.vy = spec.traversalSpeed;
            motion.PushOverride(2, dir * 11.0f, spec.traversalSpeed, spec.traversalDurationTicks, 64);
            break;

        case TraversalType::TemporalAnchorRecall:
            if (!traversal.hasTemporalAnchor) {
                if (optTransform != nullptr) {
                    traversal.temporalAnchorX = optTransform->x;
                    traversal.temporalAnchorY = optTransform->y;
                }
                traversal.hasTemporalAnchor = true;
                motor.vx = dir * spec.traversalSpeed;
                motor.vy = 0.0f;
                motion.PushOverride(2, dir * spec.traversalSpeed, 0.0f, spec.traversalDurationTicks, 0);
            } else {
                if (optTransform != nullptr) {
                    optTransform->x = traversal.temporalAnchorX;
                    optTransform->y = traversal.temporalAnchorY;
                }
                traversal.hasTemporalAnchor = false;
                motor.vx = 0.0f;
                motor.vy = 4.0f;
                motion.PushOverride(3, 0.0f, 4.0f, 2, 0);
                traversal.traversalActiveTicks = 2;
                traversal.invulnerableTicks = 12;
            }
            break;

        case TraversalType::PulseTetherSwap:
            if (optTargetTransform != nullptr && optTransform != nullptr) {
                const f32 targetX = optTargetTransform->x;
                const f32 targetY = optTargetTransform->y;
                optTransform->x = targetX;
                optTransform->y = targetY;
            } else if (optTransform != nullptr) {
                optTransform->x += dir * 5.0f;
            }
            traversal.traversalActiveTicks = 2;
            traversal.invulnerableTicks = 10;
            break;

        case TraversalType::MachRecoilPogo:
            motor.contacts &= ~gameplay::ContactFlags::Ground;
            motor.vx = dir * 3.5f;
            motor.vy = spec.traversalSpeed;
            motion.PushOverride(2, dir * 3.5f, spec.traversalSpeed, spec.traversalDurationTicks, 256);
            traversal.airDashesRemaining = 1;
            break;
    }
}

} // namespace en::veil
