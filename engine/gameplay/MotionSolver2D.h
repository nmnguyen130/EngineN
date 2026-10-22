#pragma once

#include "gameplay/GameplayTypes.h"
#include "physics/PhysicsSystem.h"

namespace en {

// Stateless priority motion blender, coyote/drop-through timer updater, soft character pushbox separator, and 3-ray safe anchor verifier.
// Operates over World component pools and PhysicsSystem spatial queries with zero dynamic memory allocations.
// Invariant: MotionStack2D root-motion deltas are consumed and zeroed once per simulation tick when blended into CharacterMotor2D.
class MotionSolver2D {
public:
    static void SyncPostPhysicsContactsAndTimers(
        World& world,
        const PhysicsSystem& physics,
        const SimStepView& step,
        u8 defaultCoyoteTicks = 8U) noexcept;

    static void BlendMotionStacksIntoMotors(
        World& world,
        const SimStepView& step,
        const GameplayConfig& cfg) noexcept;

    static void ResolveSoftCharacterPushboxes(
        World& world,
        const PhysicsSystem& physics,
        const SimStepView& step,
        const GameplayConfig& cfg) noexcept;
};

} // namespace en
