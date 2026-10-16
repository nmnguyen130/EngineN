#pragma once

#include "gameplay/GameplayTypes.h"

#include <span>

namespace en::gameplay {

struct SlabImpact2D {
    f32 toi{1.0f};
    f32 normalX{0.0f};
    f32 normalY{0.0f};
    world::EntityId carrierId{world::kNullEntity};
    TileShape tileShape{TileShape::Empty};
    TileMaterialFlags material{TileMaterialFlags::None};
    bool hit{false};
};

struct RayHit2D {
    f32 distance{0.0f};
    f32 pointX{0.0f};
    f32 pointY{0.0f};
    f32 normalX{0.0f};
    f32 normalY{0.0f};
    world::EntityId entity{world::kNullEntity};
    u16 layer{0};
    bool hit{false};
};

struct ZoneSample2D {
    f32 forceVx{0.0f};
    f32 forceVy{0.0f};
    f32 dragScale{1.0f};
    bool submerged{false};
};

// Zero-allocation spatial accelerator combining an 8x8 hierarchical bitplane with a 3-pass Counting-Sort Prefix-Sum Grid.
// Non-owning view over arena memory provided by GameplayPipeline; zero OS or dynamic memory dependencies.
// Invariant: m_cellOffsets[c + 1] - m_cellOffsets[c] equals the exact number of dynamic boxes overlapping cell c.
class SpatialIndex2D final {
public:
    static constexpr u32 kGridCells   = 512;
    static constexpr u32 kMaxCellRefs = 2048;

    SpatialIndex2D() = default;

    [[nodiscard]] static constexpr usize RequiredArenaBytes() noexcept {
        return (sizeof(u16) * (kGridCells + 1u) * 2u) +
               (sizeof(world::EntityId) * kMaxCellRefs) +
               (sizeof(f32) * kMaxCellRefs * 4u) +
               (sizeof(u16) * kMaxCellRefs) +
               (sizeof(u8) * kMaxCellRefs * 2u) + 256u;
    }

    void BindArena(std::span<u8> arenaSlice) noexcept;

    [[nodiscard]] Result<void, GameplayErrc> BindCollisionMapBlob(
        const assets::BlobRecord& blob) noexcept;

    void ClearCollisionMap() noexcept;

    void RebuildDynamicGrid(const world::World& world) noexcept;

    [[nodiscard]] SlabImpact2D SweepAabb(
        world::EntityId ignoreEntity,
        f32 centerX, f32 centerY,
        f32 halfW, f32 halfH,
        f32 deltaX, f32 deltaY,
        u16 solidMask,
        bool ignoreOneWay) const noexcept;

    [[nodiscard]] bool CheckStaticPointSolid(
        f32 worldX, f32 worldY,
        bool requireSafeAnchor = false) const noexcept;

    [[nodiscard]] bool DetectLedgeCorner(
        f32 chestWallX, f32 headTopY,
        f32 clearanceCenterX) const noexcept;

    [[nodiscard]] ZoneSample2D SampleEnvironmentalZones(
        f32 centerX, f32 centerY) const noexcept;

    void ComputeSoftPushboxSeparation(
        world::EntityId selfId,
        u8 pushboxGroup,
        f32 centerX, f32 centerY,
        f32 halfW, f32 halfH,
        f32& inoutSepVx) const noexcept;

    [[nodiscard]] RayHit2D Raycast(
        f32 originX, f32 originY,
        f32 dirX, f32 dirY,
        f32 maxDist,
        u16 layerMask,
        world::EntityId ignoreId = world::kNullEntity) const noexcept;

    [[nodiscard]] u32 GatherCombatOverlaps(
        const world::World& world,
        const assets::AssetBank& assetBank,
        std::span<ResolvedHitEvent> outHits) const noexcept;

    [[nodiscard]] constexpr f32 TileSize() const noexcept { return m_tileSize; }
    [[nodiscard]] constexpr bool HasCollisionMap() const noexcept { return m_mapHeader != nullptr; }

private:
    enum class RefKind : u8 {
        Carrier = 0,
        Hurtbox = 1,
        Hitbox  = 2,
        Pushbox = 3
    };

    [[nodiscard]] static constexpr u32 HashCellCoords(i32 cx, i32 cy) noexcept {
        const u32 ux = static_cast<u32>(cx) * 73856093u;
        const u32 uy = static_cast<u32>(cy) * 19349663u;
        return (ux ^ uy) & (kGridCells - 1u);
    }

    [[nodiscard]] CookedTileCell GetTileAt(i32 tx, i32 ty) const noexcept;

    const CookedCollisionMapHeader* m_mapHeader{nullptr};
    const CookedCollisionChunk8x8* m_chunks{nullptr};
    const CookedTileCell* m_fineTiles{nullptr};
    const EnvironmentalZone2D* m_zones{nullptr};
    f32 m_tileSize{1.0f};
    f32 m_invTileSize{1.0f};

    u16* m_cellCounts{nullptr};
    u16* m_cellOffsets{nullptr};
    world::EntityId* m_sortedEntities{nullptr};
    f32* m_boxMinX{nullptr};
    f32* m_boxMinY{nullptr};
    f32* m_boxMaxX{nullptr};
    f32* m_boxMaxY{nullptr};
    u16* m_boxMask{nullptr};
    u8* m_boxKind{nullptr};
    u8* m_boxAux{nullptr};
    u32 m_totalRefs{0};
};

} // namespace en::gameplay
