#pragma once

#include "assets/AssetBank.h"
#include "renderer/RenderList.h"
#include "world/World.h"

namespace en {

// Presentation + visual/lifetime step + prefab bridge.
// Stationary (moved bit clear) skips prev/alpha. Frozen channels force alpha=1.
class WorldExtractor {
public:
    WorldExtractor() = delete;

    static void StepAmbientVelocities(World& world, const SimStepView& step) noexcept;
    static void StepVisualsAndLifetimes(World& world, const SimStepView& step) noexcept;

    static void Extract(
        const World& world,
        const AssetBank& assetBank,
        const RenderTimeView& timeView,
        RenderList& outRenderList,
        f32 defaultPixelsPerMeter = 48.0f) noexcept;

    [[nodiscard]] static Result<EntityId, WorldErrc> InstantiatePrefab(
        World& world,
        const AssetBank& assetBank,
        BlobHandle prefabBlob,
        const LocalTransform2D& rootTransform = {},
        std::span<EntityId> outSpawnedEntities = {}) noexcept {
        return world.InstantiatePrefab(assetBank, prefabBlob, rootTransform, outSpawnedEntities);
    }
};

} // namespace en
