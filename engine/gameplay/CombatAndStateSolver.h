#pragma once

#include "assets/AssetBank.h"
#include "gameplay/GameplayTypes.h"
#include "physics/SpatialIndex2D.h"

namespace en {

// Single-pass animation rig directive dispatcher, FSM evaluator, and Bloom-deduplicated combat solver.
class CombatAndStateSolver final {
public:
    static void DispatchAnimationFrameDirectives(
        World& world,
        const AssetBank& assetBank,
        const SimStepView& step,
        GameplayPipeline& pipeline) noexcept;

    static void StepStateMachinesAndPolicies(
        World& world,
        const AssetBank& assetBank,
        const SimStepView& step,
        const TickInput& tickInput,
        ActionBuffer& actionBuffer,
        const GameplayPolicyTable& policies,
        GameplayPipeline& pipeline) noexcept;

    [[nodiscard]] static u32 GatherAndResolveCombatHits(
        World& world,
        const SimStepView& step,
        std::span<ResolvedHitEvent> hitScratch,
        const GameplayPolicyTable& policies,
        GameplayPipeline& pipeline) noexcept;
};

} // namespace en
