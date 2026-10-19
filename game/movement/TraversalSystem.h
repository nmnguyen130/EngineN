#pragma once

#include "game/core/Components.h"
#include "game/core/EchoPantheon.h"
#include "gameplay/GameplayTypes.h"
#include "world/WorldTypes.h"

namespace en::veil {

// Solves 120 Hz platformer kinematics, horizontal running, jump arcs, and 12-Echo Traversal Mutations.
// Stateless system class; operates on non-owning references to motor, motion stack, and components.
// Invariant: Floor boundaries and grounding flags remain synchronized with KinematicMotor2D contacts.
class TraversalSystem {
public:
    static constexpr f32 kMaxRunSpeed = 10.5f;
    static constexpr f32 kJumpVelocity = 15.2f;
    static constexpr f32 kGravity = -38.0f;
    static constexpr u16 kCoyoteWindowTicks = 8;

    static void Step(
        gameplay::KinematicMotor2D& motor,
        gameplay::MotionStack2D& motion,
        TraversalMutationComponent& traversal,
        const ResonanceLanternComponent& lantern,
        f32 moveInputX,
        bool jumpJustPressed,
        bool evadeJustPressed,
        world::LocalTransform2D* optTransform = nullptr,
        const world::LocalTransform2D* optTargetTransform = nullptr) noexcept;

    static void ExecuteTraversalMutation(
        gameplay::KinematicMotor2D& motor,
        gameplay::MotionStack2D& motion,
        TraversalMutationComponent& traversal,
        const EchoSpec& spec,
        world::LocalTransform2D* optTransform = nullptr,
        const world::LocalTransform2D* optTargetTransform = nullptr) noexcept;
};

} // namespace en::veil
