#pragma once

#include "gameplay/GameplayTypes.h"

#include <span>

namespace en::gameplay {

// Stateless evaluator for animation frame-edge directives, cancel windows, and combat hit arbitration.
// Operates in-place on World component pools and cooked AssetBank blobs with zero dynamic allocations.
// Invariant: Frame directives execute prior to kinematic sweeps so hitbox activations have 0-tick latency.
class StateAndCombatSolver {
public:
    static void GateFrozenAnimators(
        world::World& world,
        time::SimStepView step,
        bool restoreAfterStep) noexcept;

    static void DispatchAnimationFrameDirectives(
        world::World& world,
        const assets::AssetBank& assetBank,
        time::SimStepView step,
        GameplayPipeline& pipeline) noexcept;

    static void StepStateMachinesAndPolicies(
        world::World& world,
        const assets::AssetBank& assetBank,
        time::SimStepView step,
        const platform::input::TickInput& tickInput,
        platform::input::ActionBuffer& actionBuffer,
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
