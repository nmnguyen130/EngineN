#pragma once

#include "assets/AssetBank.h"
#include "renderer/RenderList.h"
#include "world/World.h"

namespace en {

// Built-in simulation, Metroidvania collision/sweep, and render extraction systems over World pools.
// Stationary entities (moved bit clear) skip interpolation; frozen channels resolve to alpha = 1.
class WorldSystems {
public:
    WorldSystems() = delete;

    static void StepVelocities(World& world, const TickStep& step) noexcept;
    static void StepVisualsAndLifetimes(World& world, const TickStep& step) noexcept;

    // Sweeps `mover` against all matching non-trigger `Collider2D` entities and slides along surfaces.
    static SweepHit SweepMove(World& world, EntityId mover, Vec2 delta) noexcept;

    // Appends all alive entities whose `Collider2D` overlaps `queryBox` and matches `layerMask` into `outHits`.
    static u32 QueryBoxOverlaps(
        const World& world,
        const Rect& queryBox,
        u16 layerMask,
        std::span<EntityId> outHits,
        EntityId ignoreEntity = kNullEntity) noexcept;

    static void ExtractRender(
        const World& world,
        const AssetBank& assetBank,
        const RenderStep& renderStep,
        RenderList& outRenderList,
        f32 defaultPixelsPerMeter = 48.0f) noexcept;
};

} // namespace en
