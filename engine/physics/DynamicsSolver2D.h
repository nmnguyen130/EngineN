#pragma once

#include "physics/SpatialIndex2D.h"

namespace en {

// Sequential-impulse solver + MoveAndSlide character motor.
// Invoked by PhysicsSystem::Step. Respects TimeChannel freeze; deterministic manifold order.
class DynamicsSolver2D {
public:
    static bool StepKinematicBodiesAndRiders(
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

    static void StepRigidBodiesSoftTgs(
        World& world,
        const SimStepView& step,
        const PhysicsConfig& cfg,
        std::span<ContactManifold2D> manifolds,
        std::span<ContactEvent2D> outEvents,
        u16& ioEventCount) noexcept;
};

} // namespace en
