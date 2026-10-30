#pragma once

#include "assets/AssetBank.h"
#include "renderer/RenderList.h"
#include "world/World.h"

namespace en {

// Built-in kinematic velocity integration, visual/lifetime ticking, and render extraction over World pools.
// Stationary entities (moved bit clear) skip interpolation; frozen channels resolve to alpha = 1.
class WorldSystems {
public:
    WorldSystems() = delete;

    static void StepVelocities(World& world, const TickStep& step) noexcept;
    static void StepVisualsAndLifetimes(World& world, const TickStep& step) noexcept;

    static void ExtractRender(
        const World& world,
        const AssetBank& assetBank,
        const RenderStep& renderStep,
        RenderList& outRenderList,
        f32 defaultPixelsPerMeter = 48.0f) noexcept;
};

} // namespace en
