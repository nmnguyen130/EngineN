#pragma once

#include "assets/AssetBank.h"
#include "renderer/RenderList.h"
#include "world/World.h"

namespace en {

// Stateless Layer 2 presentation and binary prefab bridge between World, AssetBank, and RenderList.
// Keeps Layer 1 World headers decoupled from AssetBank.h and RenderList.h.
// Invariant: Stationary sprites (MovedWords bit == 0) bypass PrevWorldSnapshot2D and TimeChannel loads during Extract.
class WorldExtractor {
public:
    WorldExtractor() = delete;

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
        std::span<EntityId> outSpawnedEntities = {}) noexcept;
};

} // namespace en
