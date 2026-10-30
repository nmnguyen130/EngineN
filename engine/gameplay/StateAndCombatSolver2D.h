#pragma once

#include "gameplay/GameplayTypes.h"
#include "world/World.h"

namespace en {

class StateAndCombatSolver2D {
public:
    static void Step(World& world, const AssetBank& assetBank, const SimStepView& step,
                     const TickInput& input, ActionBuffer& actions,
                     const GameplayPolicyTable& policies, GameplayPipeline& pipeline) noexcept;

    [[nodiscard]] static u16 ResolveHits(World& world, const SimStepView& step,
                                         const GameplayPolicyTable& policies,
                                         GameplayPipeline& pipeline,
                                         std::span<CombatHitEvent> outEvents) noexcept;
};

} // namespace en
