#pragma once

#include "assets/AssetBank.h"
#include "world/World.h"

#include <span>

namespace en::renderer { class RenderList; }

namespace en::world {

// Binary header for zero-copy cooked prefab/scene blobs stored in AssetBank (Residency::Sim).
struct alignas(16) CookedPrefabHeader {
    static constexpr u32 kMagic      = 0x42465045u; // 'EPFB'
    static constexpr u32 kSchemaHash = 0x20260502u;

    u32 magic{kMagic};
    u16 version{1};
    u16 entityCount{0};
    u32 totalBytes{0};
    u32 reserved{0};
};
static_assert(sizeof(CookedPrefabHeader) == 16);

struct alignas(16) CookedPrefabEntityRecord {
    LocalTransform2D localTransform{};              // 24B
    renderer::Color tint{1.0f, 1.0f, 1.0f, 1.0f};   // 16B
    assets::AssetId atlasOrTextureId{};             // 8B: Resolved via AssetBank on spawn
    u16 parentLocalIndex{0xFFFF};                   // 2B: 0xFFFF = Root of prefab
    u16 firstFrame{0};                              // 2B
    u16 frameCount{1};                              // 2B
    u16 ticksPerFrame{12};                          // 2B
    u16 baseDepth{32768};                           // 2B
    i16 ySortScale{0};                              // 2B
    u8 timeChannel{0};                              // 1B
    u8 layer{0};                                    // 1B
    u8 passId{0};                                   // 1B
    u8 flags{0};                                    // 1B: Bit 0 = HasSprite, Bit 1 = HasAnimator, Bit 2 = StaticSpatial
};
static_assert(sizeof(CookedPrefabEntityRecord) == 64);

// Layer 2 stateless bridge: extracts interpolated render commands and instantiates cooked binary prefabs.
// Non-owning utility; mutates only outRenderList during Extract() or target World during SpawnPrefab().
class WorldExtractor final {
public:
    static void Extract(
        const World& world,
        const assets::AssetBank& assetBank,
        time::RenderTimeView timeView,
        renderer::RenderList& outRenderList) noexcept;

    [[nodiscard]] static Result<EntityId, WorldErrc> InstantiatePrefab(
        World& world,
        const assets::AssetBank& assetBank,
        assets::BlobHandle prefabBlob,
        const LocalTransform2D& rootTransform = {},
        std::span<EntityId> outSpawnedEntities = {}) noexcept;
};

} // namespace en::world
