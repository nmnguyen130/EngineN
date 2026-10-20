#pragma once

#include "game/core/VeilTypes.h"

namespace en::veil {

// Enemy AI (Behavior Tree & A* NavGrid), Boss choreography, projectiles, combat hits, and HUD renderer.
// Stateless gameplay policy and presentation extractor for Veil of the Silent.
// Invariant: combat hits and projectile collisions use deferred ECS commands during pool iteration.
class VeilAiAndWorldSystem final {
public:
    static void BuildEnemyBehaviorTree(BehaviorTree2D& outTree) noexcept;

    static EntityId SpawnEnemy(
        World& world,
        Vec2 pos,
        EnemyKind kind,
        AtlasHandle mainAtlas,
        BlobHandle enemyRigBlob) noexcept;

    static void StepEnemiesAndProjectiles(
        World& world,
        const SimStepView& step,
        GameplayPipeline& pipeline,
        VeilSessionState& session) noexcept;

    static void ResolveCombatHits(
        World& world,
        const SimStepView& step,
        std::span<const ResolvedHitEvent> hits,
        GameplayPipeline& pipeline,
        VeilSessionState& session) noexcept;

    static void RenderHudAndEffects(
        RenderContext& ctx,
        VeilSessionState& session) noexcept;
};

} // namespace en::veil
