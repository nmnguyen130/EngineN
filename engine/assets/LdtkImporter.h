#pragma once

#include "assets/AssetTypes.h"
#include "core/Memory.h"
#include "physics/PhysicsTypes.h"

namespace en {

// 24-byte entity spawn descriptor extracted from an LDtk "entityInstances" layer.
// Value type; worldPos and size are converted to Y-up meters using the level's pixelsPerMeter.
struct LdtkEntityDesc {
    StringId identifier{};
    Vec2     worldPos{};
    Vec2     size{1.0f, 1.0f};
    u32      iidHash{0};
    i32      customInt{0};
};
static_assert(sizeof(LdtkEntityDesc) == 28 || sizeof(LdtkEntityDesc) == 32);

// Non-owning view over an imported LDtk level's collision grid and entity descriptors allocated in an Arena.
struct LdtkLevelView {
    std::span<const TileFlag>       tiles{};
    std::span<const LdtkEntityDesc> entities{};
    Rect                            worldBounds{};
    u16                             widthTiles{0};
    u16                             heightTiles{0};
    f32                             tileSizeMeters{1.0f};
};

// Zero-allocation LDtk level JSON importer and binary TileMap blob cooker.
// Maps LDtk IntGrid values (Solid, OneWay, Hazard, Climbable, Slopes) to Y-up TileFlag grids and LdtkEntityDesc spans.
class LdtkImporter {
public:
    LdtkImporter() = delete;

    // Maps an LDtk IntGrid integer value to EngineN's TileFlag bitmask.
    [[nodiscard]] static constexpr TileFlag MapIntGridValue(i32 v) noexcept {
        switch (v) {
            case 1:  return TileFlag::Solid;
            case 2:  return TileFlag::OneWay;
            case 3:  return TileFlag::Hazard;
            case 4:  return TileFlag::Climbable;
            case 5:  return TileFlag::Slope45Up;
            case 6:  return TileFlag::Slope45Down;
            case 7:  return TileFlag::Slope22UpLo;
            case 8:  return TileFlag::Slope22UpHi;
            case 9:  return TileFlag::Slope22DownLo;
            case 10: return TileFlag::Slope22DownHi;
            default: return TileFlag::Empty;
        }
    }

    // Parses an LDtk level JSON string into Y-up TileFlag and LdtkEntityDesc arrays allocated from arena.
    [[nodiscard]] static Result<LdtkLevelView, AssetErrc> ParseLevelJson(
        std::string_view json,
        Arena& arena,
        f32 pixelsPerMeter = 16.0f,
        u32 maxEntities = 128U) noexcept;

    // Cooks an imported LdtkLevelView into a deterministic binary TileMapHeader blob for AssetBank & SceneDirector.
    [[nodiscard]] static Result<usize, AssetErrc> CookTileMapBlob(
        const LdtkLevelView& level,
        std::span<u8> outBlob) noexcept;
};

} // namespace en
