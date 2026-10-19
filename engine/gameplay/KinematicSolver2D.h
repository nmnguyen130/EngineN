#pragma once

#include "gameplay/GameplayTypes.h"
#include "gameplay/SpatialIndex2D.h"

namespace en::gameplay {

struct KinematicStepConfig {
    f32 baseGravity{-32.0f};
    f32 terminalFallVy{-26.0f};
    f32 wallSlideMaxVy{-5.5f};
    f32 skinWidth{0.001f};
    f32 cornerCorrectionMax{0.22f};
    f32 groundSnapMaxDist{0.18f};
    u8 maxCoyoteTicks{12};
    u8 maxWallCoyoteTicks{10};
};

// Stateless continuous swept-AABB kinematic solver, carrier transfer engine, and topology sensor.
// Operates directly on World component pools and SpatialIndex2D with zero dynamic memory allocation.
// Invariant: Never mutates structural ECS state; writes resolved coordinates via World::ModifyLocalTransformByIndex().
class KinematicSolver2D final {
public:
    static void StepCarriersAndRiders(
        world::World& world,
        const SpatialIndex2D& spatial,
        time::SimStepView step,
        const KinematicStepConfig& cfg = {}) noexcept;

    static void StepMotorsAndMotionStacks(
        world::World& world,
        const SpatialIndex2D& spatial,
        time::SimStepView step,
        const KinematicStepConfig& cfg = {}) noexcept;

private:
    static void ResolveSingleMotorSweep(
        world::EntityId id,
        world::LocalTransform2D& inoutTransform,
        KinematicMotor2D& inoutMotor,
        const SpatialIndex2D& spatial,
        f32 deltaX,
        f32 deltaY,
        const KinematicStepConfig& cfg) noexcept;
};

} // namespace en::gameplay
