#pragma once

#include "physics/SpatialIndex2D.h"

namespace en {

// Stateless 2D rotational Soft-Step (TGS) sequential impulse solver, distance/spring joint solver, speculative CCD integrator, and MoveAndSlide character controller.
// Pure algorithmic layer operating over World component pools and SpatialIndex2D with zero dynamic allocations.
// Invariant: sleeping bodies retain zero velocity and are woken deterministically upon manifold creation, joint tension, or external impulse.
class DynamicsSolver2D {
public:
    static void StepKinematicBodiesAndRiders(
        World& world,
        const SpatialIndex2D& spatial,
        const SimStepView& step) noexcept;

    static void StepCharacterMotors(
        World& world,
        const SpatialIndex2D& spatial,
        const SimStepView& step,
        const PhysicsConfig& cfg) noexcept;

    [[nodiscard]] static u16 GatherManifolds(
        World& world,
        const SpatialIndex2D& spatial,
        const SimStepView& step,
        const PhysicsConfig& cfg,
        std::span<const ContactManifold2D> prevManifolds,
        std::span<ContactManifold2D> outManifolds,
        std::span<ContactEvent2D> outEvents,
        u16& ioEventCount,
        u64& ioSpeculativeHitCount) noexcept;

    static void StepRigidBodiesAndJointsSoftTgs(
        World& world,
        const SimStepView& step,
        const PhysicsConfig& cfg,
        std::span<ContactManifold2D> manifolds,
        std::span<ContactEvent2D> outEvents,
        u16& ioEventCount) noexcept;
};

} // namespace en
