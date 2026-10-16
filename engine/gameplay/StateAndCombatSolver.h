#pragma once

#include "gameplay/GameplayTypes.h"

#include <span>

namespace en::gameplay {

// Dedicated 120 Hz Animation & Frame-Directive Subsystem.
// Steps lockstep [Sprite2D + SpriteAnimator2D] pools with native TimeChannel freeze gating
// and dispatches cooked .erig frame-edge directives (hitboxes, hurtboxes, root-motion, cancel masks) in a single pass.
// Invariant: Executes in Stage 2 before kinematics so hitbox activations have 0-tick latency.
class AnimationSubsystem final {
public:
    static void StepAnimationsAndDirectives(
        world::World& world,
        const assets::AssetBank& assetBank,
        time::SimStepView step,
        GameplayPipeline& pipeline) noexcept;
};

// Stateless evaluator for actor state machines, cancel windows, and combat hit arbitration.
// Operates in-place on World component pools and cooked AssetBank blobs with zero dynamic allocations.
class StateAndCombatSolver final {
public:
    static void StepStateMachinesAndPolicies(
        world::World& world,
        const assets::AssetBank& assetBank,
        time::SimStepView step,
        const input::TickInput& tickInput,
        input::ActionBuffer& actionBuffer,
        const GameplayPolicyTable& policies,
        GameplayPipeline& pipeline) noexcept;

    static void ResolveCombatHits(
        world::World& world,
        time::SimStepView step,
        std::span<const ResolvedHitEvent> hits,
        const GameplayPolicyTable& policies,
        GameplayPipeline& pipeline) noexcept;
};

} // namespace en::gameplay
