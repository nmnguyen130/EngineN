#pragma once

#include "gameplay/GameplayTypes.h"
#include "world/World.h"

namespace en {

// Stateless finite-state-machine stepper, frame-directive dispatcher, and O(1) Bloom-deduplicated combat resolver.
// Operates directly over dense World component pools with zero dynamic memory allocations.
// Invariant: each animation frame directive executes at most once per frame transition, and a swingId never hits the same defender twice.
class StateAndCombatSolver2D {
public:
    static void StepStateMachinesAndDirectives(
        World& world,
        const AssetBank& assetBank,
        const SimStepView& step,
        const TickInput& input,
        ActionBuffer& actions,
        const GameplayPolicyTable& policies,
        GameplayPipeline& pipeline) noexcept;

    [[nodiscard]] static u16 ResolveCombatOverlaps(
        World& world,
        const SimStepView& step,
        const GameplayPolicyTable& policies,
        GameplayPipeline& pipeline,
        std::span<CombatHitEvent> outEvents) noexcept;
};

} // namespace en
