#pragma once

#include "physics/SpatialIndex2D.h"

namespace en {

// Continuous 2D swept-AABB character motor, moving carrier, joint, and Verlet rope solver.
class KinematicSolver2D final {
public:
    static void StepCarriersAndRiders(
        World& world,
        const SpatialIndex2D& spatial,
        const SimStepView& step,
        const KinematicStepConfig& cfg = {}) noexcept;

    static void StepMotorsAndMotionStacks(
        World& world,
        const SpatialIndex2D& spatial,
        const SimStepView& step,
        const KinematicStepConfig& cfg = {}) noexcept;

    static void StepConstraintsAndVerlet(
        World& world,
        const SpatialIndex2D& spatial,
        const SimStepView& step,
        const KinematicStepConfig& cfg = {}) noexcept;

    // Continuous Collision Detection (CCD) for high-velocity projectiles preventing tunneling.
    [[nodiscard]] static SlabImpact2D SweepProjectileCCD(
        const SpatialIndex2D& spatial,
        EntityId ignoreEntity,
        Vec2 startPos,
        Vec2 velocity,
        f32 dt,
        Vec2 halfExtents,
        u16 hitMask = 1U) noexcept;
};

} // namespace en
