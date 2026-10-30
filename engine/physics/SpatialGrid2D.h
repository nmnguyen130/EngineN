#pragma once

#include "core/Memory.h"
#include "physics/PhysicsTypes.h"
#include "world/World.h"

namespace en {

// 24-byte spatial proxy caching an entity's world-space AABB and collision filter masks.
// Value type stored contiguously in SpatialGrid2D.
struct SpatialProxy2D {
    Rect     bounds{};
    EntityId entity{kNullEntity};
    u16      layerMask{1U};
    u8       isTrigger{0};
    u8       oneWay{0};
};
static_assert(sizeof(SpatialProxy2D) == 24);

// Uniform counting-sort spatial hash grid and 2D tilemap collision accelerator over Arena memory.
// Non-owning view over Arena slabs; merges contiguous tile runs to prevent internal edge snagging.
// Invariant: sizeof(SpatialGrid2D) == 64, and m_cellOffsets[kGridCells] == TotalCellRefs().
class SpatialGrid2D {
public:
    static constexpr u32 kGridDim   = 32U;
    static constexpr u32 kGridCells = kGridDim * kGridDim;

    [[nodiscard]] bool Init(Arena& arena, u16 maxProxies, f32 cellSize = 2.0f) noexcept;
    void Clear() noexcept;

    [[nodiscard]] Result<void, PhysicsErrc> BuildTileMap(
        Arena& arena,
        u16 width,
        u16 height,
        f32 tileSize,
        Vec2 origin,
        std::span<const TileFlag> tiles) noexcept;

    [[nodiscard]] Result<void, PhysicsErrc> LoadTileMapBlob(
        Arena& arena,
        std::span<const u8> blobBytes) noexcept;

    void SetTile(i32 tx, i32 ty, TileFlag flag) noexcept;
    [[nodiscard]] TileFlag SampleTile(i32 tx, i32 ty) const noexcept;
    [[nodiscard]] TileFlag SampleTileWorld(Vec2 worldPos) const noexcept;
    [[nodiscard]] Rect TileWorldBounds(i32 tx, i32 ty) const noexcept;

    void Rebuild(const World& world) noexcept;

    [[nodiscard]] RayHit2D Raycast(
        Vec2 origin,
        Vec2 dir,
        f32 maxDist,
        u16 queryMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity,
        bool includeTriggers = false) const noexcept;

    [[nodiscard]] u32 RaycastAll(
        Vec2 origin,
        Vec2 dir,
        f32 maxDist,
        u16 queryMask,
        std::span<RayHit2D> outHits,
        EntityId ignoreEntity = kNullEntity,
        bool includeTriggers = true) const noexcept;

    [[nodiscard]] SweepResult2D SweepBox(
        const Rect& movingBox,
        Vec2 delta,
        u16 queryMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity,
        bool ignoreOneWay = false) const noexcept;

    [[nodiscard]] u32 QueryBox(
        const Rect& queryBox,
        u16 queryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity,
        bool includeTriggers = true) const noexcept;

    [[nodiscard]] u32 QueryCircle(
        Vec2 center,
        f32 radius,
        u16 queryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity,
        bool includeTriggers = true) const noexcept;

    [[nodiscard]] constexpr std::span<const SpatialProxy2D> Proxies() const noexcept {
        return {m_proxies, m_proxyCount};
    }
    [[nodiscard]] constexpr std::span<const u32> CellOffsets() const noexcept {
        return {m_cellOffsets, kGridCells + 1U};
    }
    [[nodiscard]] constexpr std::span<const u16> CellRefs() const noexcept {
        return {m_cellRefs, TotalCellRefs()};
    }
    [[nodiscard]] constexpr std::span<const u16> OversizedRefs() const noexcept {
        if (!m_cellRefs) return {};
        return {m_cellRefs + static_cast<usize>(m_maxProxies) * 16U, m_oversizedCount};
    }
    [[nodiscard]] constexpr u32 TotalCellRefs() const noexcept {
        return m_cellOffsets ? m_cellOffsets[kGridCells] : 0U;
    }

    [[nodiscard]] constexpr u16 WidthTiles() const noexcept { return m_widthTiles; }
    [[nodiscard]] constexpr u16 HeightTiles() const noexcept { return m_heightTiles; }
    [[nodiscard]] constexpr f32 TileSize() const noexcept { return m_tileSize; }
    [[nodiscard]] constexpr Vec2 Origin() const noexcept { return {m_originX, m_originY}; }
    [[nodiscard]] constexpr f32 InvCellSize() const noexcept { return m_invCellSize; }

    [[nodiscard]] static constexpr u32 HashCell(i32 cx, i32 cy) noexcept {
        const u32 ux = static_cast<u32>(cx) * 73856093U;
        const u32 uy = static_cast<u32>(cy) * 19349663U;
        return (ux ^ uy) & (kGridCells - 1U);
    }

private:
    template <typename Fn>
    void VisitCandidates(const Rect& area, Fn&& fn) const noexcept;

    template <typename Fn>
    void VisitTileRuns(const Rect& area, Fn&& fn) const noexcept;

    SpatialProxy2D* m_proxies{nullptr};
    u32*            m_cellOffsets{nullptr};
    u16*            m_cellRefs{nullptr};
    TileFlag*       m_tiles{nullptr};
    f32             m_originX{0.0f};
    f32             m_originY{0.0f};
    f32             m_tileSize{1.0f};
    f32             m_invCellSize{0.5f};
    u16             m_widthTiles{0};
    u16             m_heightTiles{0};
    u16             m_proxyCount{0};
    u16             m_maxProxies{0};
    u16             m_oversizedCount{0};
    u16             m_tileCap{0};
    u32             m_pad{0};
};
static_assert(sizeof(SpatialGrid2D) == 64);

} // namespace en
