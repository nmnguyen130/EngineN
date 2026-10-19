#pragma once

#include "gameplay/GameplayTypes.h"
#include "game/core/Components.h"

namespace en::veil {

// GameplayPolicyTable callbacks wiring actor kinematic movement, 12-Echo Traversal Mutations,
// and hit resolution into EngineN's deterministic 120Hz GameplayPipeline.
// Non-owning policy utility passed to EngineN SceneDirector.
class GameplayPolicies {
public:
    static void EvaluateActor(
        const gameplay::ActorPolicyContext& ctx,
        gameplay::KinematicMotor2D& motor,
        gameplay::MotionStack2D& motion,
        gameplay::StateMachine2D& fsm,
        gameplay::HitboxRig2D* optRig) noexcept;

    static void ResolveCombatHits(
        world::World& world,
        time::SimStepView step,
        std::span<const gameplay::ResolvedHitEvent> hits,
        gameplay::GameplayPipeline& pipeline) noexcept;

    [[nodiscard]] static gameplay::GameplayPolicyTable CreatePolicyTable() noexcept;
};

} // namespace en::veil
