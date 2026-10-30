#pragma once

#include "core/Memory.h"
#include "physics/PhysicsTypes.h"
#include "world/World.h"

namespace en {

// Spatial grid proxy record for broadphase and ray/sweep queries.
// Stores world AABB (minX, minY, maxX, maxY) for standard shapes, or world endpoints p0->p1 for Segment.
struct alignas(8) SpatialBox2D {
    f32 minX{0.0f};
    f32 minY{0.0f};
    f32 maxX{0.0f};
    f32 maxY{0.0f};
    EntityId entity{kNullEntity};
    u16 categoryBits{0x0001U};
    ShapeKind2D shape{ShapeKind2D::Aabb};
    TileMaterialFlags flags{TileMaterialFlags::Solid};

    [[nodiscard]] constexpr f32 BoundMinX() const noexcept { return (shape == ShapeKind2D::Segment) ? std::min(minX, maxX) : minX; }
    [[nodiscard]] constexpr f32 BoundMinY() const noexcept { return (shape == ShapeKind2D::Segment) ? std::min(minY, maxY) : minY; }
    [[nodiscard]] constexpr f32 BoundMaxX() const noexcept { return (shape == ShapeKind2D::Segment) ? std::max(minX, maxX) : maxX; }
    [[nodiscard]] constexpr f32 BoundMaxY() const noexcept { return (shape == ShapeKind2D::Segment) ? std::max(minY, maxY) : maxY; }
    [[nodiscard]] constexpr bool IsSensor() const noexcept { return !HasFlag(flags, TileMaterialFlags::Solid); }
    [[nodiscard]] constexpr bool IsOneWay() const noexcept { return HasFlag(flags, TileMaterialFlags::OneWay); }
};
static_assert(sizeof(SpatialBox2D) == 24);

// Uniform spatial grid + 8x8 tile bitplanes + oversized proxies. Memory from PhysicsSystem arena.
// sizeof == 64. CellOffsets()[kGridCells] == TotalCellRefs().
class alignas(64) SpatialIndex2D {
public:
    static constexpr u32 kGridDim = 32U;
    static constexpr u32 kGridCells = kGridDim * kGridDim;

    [[nodiscard]] bool Init(Arena& arena, u16 maxProxies, f32 cellSizeMeters = 2.0f) noexcept;
    void Clear() noexcept;

    [[nodiscard]] Result<void, PhysicsErrc> BindCollisionMap(
        Arena& arena,
        std::span<const u8> blobBytes) noexcept;

    [[nodiscard]] Result<void, PhysicsErrc> BuildTileMapFromGrid(
        Arena& arena,
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
        f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist,
        u16 categoryMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity,
        bool includeSensors = true) const noexcept;

    [[nodiscard]] SweepHit2D SweepAabb(
        EntityId ignoreEntity,
        f32 centerX, f32 centerY, f32 halfW, f32 halfH,
        f32 deltaX, f32 deltaY,
        u16 solidMask = 0xFFFFU,
        bool ignoreOneWay = false) const noexcept;

    [[nodiscard]] SweepHit2D CastCircle(
        EntityId ignoreEntity,
        f32 centerX, f32 centerY, f32 radius,
        f32 deltaX, f32 deltaY,
        u16 solidMask = 0xFFFFU,
        bool ignoreOneWay = false) const noexcept;

    [[nodiscard]] u32 QueryAabb(
        f32 minX, f32 minY, f32 maxX, f32 maxY,
        u16 categoryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity,
        bool includeSensors = true) const noexcept;

    [[nodiscard]] u32 QueryCircle(
        f32 centerX, f32 centerY, f32 radius,
        u16 categoryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity,
        bool includeSensors = true) const noexcept;

    [[nodiscard]] constexpr std::span<const SpatialBox2D> ActiveBoxes() const noexcept {
        return {m_boxes, m_boxCount};
    }

    [[nodiscard]] constexpr std::span<const u32> CellOffsets() const noexcept {
        return {m_cellOffsets, kGridCells + 1U};
    }

    [[nodiscard]] constexpr std::span<const u16> CellBoxIndices() const noexcept {
        return {m_cellBoxIndices, TotalCellRefs()};
    }

    [[nodiscard]] std::span<const u16> OversizedBoxIndices() const noexcept {
        if (!m_boxes || !m_cellOffsets) return {};
        return {reinterpret_cast<const u16*>(m_boxes + m_maxBoxes), m_cellOffsets[kGridCells + 1U]};
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
    SpatialBox2D* m_boxes{nullptr};
    u32* m_cellOffsets{nullptr};
    u16* m_cellBoxIndices{nullptr};
    const u64* m_chunkBitplanes{nullptr};
    const u8* m_tileFlags{nullptr};
    f32 m_originX{0.0f};
    f32 m_originY{0.0f};
    f32 m_tileSize{1.0f};
    f32 m_invCellSize{0.5f};
    u16 m_widthTiles{0U};
    u16 m_heightTiles{0U};
    u16 m_boxCount{0U};
    u16 m_maxBoxes{0U};
};
static_assert(sizeof(SpatialIndex2D) == 64);

} // namespace en
