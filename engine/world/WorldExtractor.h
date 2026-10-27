#pragma once

#include "assets/AssetBank.h"
#include "renderer/RenderList.h"
#include "world/World.h"

namespace en {

// Stateless presentation, visual/lifetime stepping, and prefab bridge between World, AssetBank, and RenderList.
// Keeps World storage decoupled from AssetBank.h, RenderList.h, and visual/lifetime tick systems.
// Invariant: Stationary sprites (MovedWords bit == 0) bypass PrevWorldSnapshot2D and TimeChannel reads during Extract.
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
