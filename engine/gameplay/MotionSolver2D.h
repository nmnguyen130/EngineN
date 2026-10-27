#pragma once

#include "gameplay/GameplayTypes.h"
#include "physics/PhysicsSystem.h"

namespace en {

// Stateless priority motion blender, coyote/drop-through timer updater, character pushbox separator, and safe anchor verifier.
// Non-owning simulation stage helper invoked by GameplayPipeline::Tick.
// Invariant: respects per-entity TimeChannel freeze states and hitstop freeze ticks.
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
