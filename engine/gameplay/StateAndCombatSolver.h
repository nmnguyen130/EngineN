#pragma once

#include "gameplay/GameplayTypes.h"

#include <span>

namespace en::gameplay {

// Stateless evaluator for cooked .erig frame directives, actor state machines, cancel windows, and combat hits.
// Operates directly on dense World component pools and cooked AssetBank blobs with zero dynamic allocations.
// Invariant: DispatchAnimationFrameDirectives executes in Stage 2 before kinematics for 0-tick hitbox latency.
class StateAndCombatSolver final {
public:
    static void DispatchAnimationFrameDirectives(
        world::World& world,
        const assets::AssetBank& assetBank,
        time::SimStepView step,
        GameplayPipeline& pipeline) noexcept;

    static void StepAnimationsAndDirectives(
        world::World& world,
        const assets::AssetBank& assetBank,
        time::SimStepView step,
        GameplayPipeline& pipeline) noexcept {
        DispatchAnimationFrameDirectives(world, assetBank, step, pipeline);
    }

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

using AnimationSubsystem = StateAndCombatSolver;

} // namespace en::gameplay
