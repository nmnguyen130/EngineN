#pragma once

#include "core/Memory.h"
#include "physics/PhysicsTypes.h"
#include "world/World.h"

namespace en {

// 24-byte (zero padding) spatial grid proxy record for cache-coherent broadphase and ray/sweep queries.
// For Aabb/Circle/CapsuleY/Obb: (minX, minY, maxX, maxY) is the world AABB.
// For Segment: (minX, minY) is world endpoint p0 and (maxX, maxY) is world endpoint p1.
struct alignas(8) SpatialBox2D {
    f32 minX{0.0f};
    f32 minY{0.0f};
    f32 maxX{0.0f};
    f32 maxY{0.0f};
    EntityId entity{kNullEntity};
    u16 categoryBits{0x0001U};
    ShapeKind2D shape{ShapeKind2D::Aabb};
    ColliderFlags2D flags{ColliderFlags2D::None};

    [[nodiscard]] constexpr f32 BoundMinX() const noexcept { return (shape == ShapeKind2D::Segment) ? std::min(minX, maxX) : minX; }
    [[nodiscard]] constexpr f32 BoundMinY() const noexcept { return (shape == ShapeKind2D::Segment) ? std::min(minY, maxY) : minY; }
    [[nodiscard]] constexpr f32 BoundMaxX() const noexcept { return (shape == ShapeKind2D::Segment) ? std::max(minX, maxX) : maxX; }
    [[nodiscard]] constexpr f32 BoundMaxY() const noexcept { return (shape == ShapeKind2D::Segment) ? std::max(minY, maxY) : maxY; }
};
static_assert(sizeof(SpatialBox2D) == 24);

// 64-byte (1 cache line exact, zero padding) hybrid 2-level hierarchical tile bitplane and 3-pass counting-sort uniform grid.
// Non-owning view over PhysicsSystem's AlignedArena; zero heap allocations and zero stack buffers during RebuildDynamicGrid.
// Invariant: m_cellOffsets[kGridCells] equals the total number of scattered cell references in the active build.
class alignas(64) SpatialIndex2D {
public:
    static constexpr u32 kGridDim = 32U;
    static constexpr u32 kGridCells = kGridDim * kGridDim; // 1024 cells

    [[nodiscard]] bool Init(AlignedArena& arena, u16 maxProxies, f32 cellSizeMeters = 2.0f) noexcept;
    void Clear() noexcept;

    [[nodiscard]] Result<void, PhysicsErrc> BindCollisionMap(std::span<const u8> blobBytes) noexcept;
    [[nodiscard]] Result<void, PhysicsErrc> BuildTileMapFromGrid(
        AlignedArena& arena,
        u16 widthTiles,
        u16 heightTiles,
        f32 tileSizeMeters,
        f32 originX,
        f32 originY,
        std::span<const TileMaterialFlags> tiles) noexcept;

    void RebuildDynamicGrid(const World& world, f32 dt = 0.0f) noexcept;

    [[nodiscard]] TileMaterialFlags SampleTileAtWorld(f32 worldX, f32 worldY) const noexcept;
    [[nodiscard]] TileMaterialFlags SampleTileCoord(i32 tx, i32 ty) const noexcept;

    [[nodiscard]] RayHit2D Raycast(
        f32 originX,
        f32 originY,
        f32 dirX,
        f32 dirY,
        f32 maxDist,
        u16 categoryMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity) const noexcept;

    [[nodiscard]] SweepHit2D SweepAabb(
        EntityId ignoreEntity,
        f32 centerX,
        f32 centerY,
        f32 halfW,
        f32 halfH,
        f32 deltaX,
        f32 deltaY,
        u16 solidMask = 0xFFFFU,
        bool ignoreOneWay = false) const noexcept;

    [[nodiscard]] SweepHit2D CastCircle(
        EntityId ignoreEntity,
        f32 centerX,
        f32 centerY,
        f32 radius,
        f32 deltaX,
        f32 deltaY,
        u16 solidMask = 0xFFFFU,
        bool ignoreOneWay = false) const noexcept {
        return SweepAabb(ignoreEntity, centerX, centerY, radius, radius, deltaX, deltaY, solidMask, ignoreOneWay);
    }

    [[nodiscard]] u32 QueryAabb(
        f32 minX,
        f32 minY,
        f32 maxX,
        f32 maxY,
        u16 categoryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity) const noexcept;

    [[nodiscard]] u32 QueryCircle(
        f32 centerX,
        f32 centerY,
        f32 radius,
        u16 categoryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity) const noexcept;

    [[nodiscard]] constexpr std::span<const SpatialBox2D> ActiveBoxes() const noexcept {
        return {m_boxes, m_boxCount};
    }

    [[nodiscard]] constexpr std::span<const u32> CellOffsets() const noexcept {
        return {m_cellOffsets, kGridCells + 1U};
    }

    [[nodiscard]] constexpr std::span<const u16> CellBoxIndices() const noexcept {
        return {m_cellBoxIndices, TotalCellRefs()};
    }

    [[nodiscard]] constexpr u32 TotalCellRefs() const noexcept {
        return m_cellOffsets ? m_cellOffsets[kGridCells] : 0U;
    }

    [[nodiscard]] constexpr f32 TileSizeMeters() const noexcept { return m_tileSize; }
    [[nodiscard]] constexpr f32 InvCellSize() const noexcept { return m_invCellSize; }
    [[nodiscard]] constexpr f32 OriginX() const noexcept { return m_originX; }
    [[nodiscard]] constexpr f32 OriginY() const noexcept { return m_originY; }
    [[nodiscard]] constexpr u16 WidthTiles() const noexcept { return m_widthTiles; }
    [[nodiscard]] constexpr u16 HeightTiles() const noexcept { return m_heightTiles; }
    [[nodiscard]] constexpr u16 ChunkCols() const noexcept { return static_cast<u16>((m_widthTiles + 7U) >> 3U); }
    [[nodiscard]] constexpr u16 ChunkRows() const noexcept { return static_cast<u16>((m_heightTiles + 7U) >> 3U); }

    [[nodiscard]] static constexpr u32 HashCellCoords(i32 cx, i32 cy) noexcept {
        const u32 ux = static_cast<u32>(cx) * 73856093U;
        const u32 uy = static_cast<u32>(cy) * 19349663U;
        return (ux ^ uy) & (kGridCells - 1U);
    }

private:
    SpatialBox2D* m_boxes{nullptr};       // 8B (first half: active, second half: staging)
    u32* m_cellOffsets{nullptr};          // 8B (kGridCells + 1 entries)
    u16* m_cellBoxIndices{nullptr};       // 8B (scattered proxy indices)
    const u64* m_chunkBitplanes{nullptr}; // 8B (1 u64 per 8x8 tile chunk)
    const u8* m_tileFlags{nullptr};       // 8B (widthTiles * heightTiles bytes)
    f32 m_originX{0.0f};                  // 4B
    f32 m_originY{0.0f};                  // 4B
    f32 m_tileSize{1.0f};                 // 4B
    f32 m_invCellSize{0.5f};              // 4B
    u16 m_widthTiles{0U};                 // 2B
    u16 m_heightTiles{0U};                // 2B
    u16 m_boxCount{0U};                   // 2B
    u16 m_maxBoxes{0U};                   // 2B
};
static_assert(sizeof(SpatialIndex2D) == 64);

} // namespace en
