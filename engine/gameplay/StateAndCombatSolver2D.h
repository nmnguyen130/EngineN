#pragma once

#include "gameplay/GameplayTypes.h"
#include "world/World.h"

namespace en {

// Stateless finite-state-machine stepper, frame-directive dispatcher, and bloom-deduplicated combat resolver.
// Non-owning simulation stage helper invoked by GameplayPipeline::Tick.
// Invariant: combat overlap checks strictly ignore frozen TimeChannels and already-recorded defender hits per swing.
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
