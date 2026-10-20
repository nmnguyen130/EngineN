#pragma once

#include "assets/AssetTypes.h"
#include "physics/PhysicsTypes.h"
#include "world/World.h"

namespace en {

enum class SpatialRebuildMode : u8 {
    All = 0,
    KinematicsOnly = 1,
    CombatOnly = 2,
};

struct alignas(8) GridBox {
    f32 minX{0.0f};
    f32 minY{0.0f};
    f32 maxX{0.0f};
    f32 maxY{0.0f};
    EntityId entity{kNullEntity};
    u16 layerMask{1};
    u16 flags{0};

    [[nodiscard]] constexpr Rect ToRect() const noexcept {
        return {minX, minY, maxX - minX, maxY - minY};
    }
};
static_assert(sizeof(GridBox) == 24);

// Chunked 8x8 bitmask tilemap and dynamic spatial accelerator.
class alignas(64) SpatialIndex2D {
public:
    static constexpr u32 kGridDim = 32;
    static constexpr u32 kGridCells = kGridDim * kGridDim;
    static constexpr u32 kMaxBoxes = 1024;

    [[nodiscard]] bool Init(AlignedArena& arena, u16 maxMapWidth = 256, u16 maxMapHeight = 128) noexcept;
    void ClearTilemap() noexcept;

    [[nodiscard]] Result<void, WorldErrc> BindCollisionMapBlob(const BlobRecord& blob) noexcept;
    void SetupProceduralGrid(u16 width, u16 height, f32 tileSize = 1.0f, Vec2 origin = {0.0f, 0.0f}) noexcept;

    void SetTile(i32 tx, i32 ty, TileType type) noexcept;
    [[nodiscard]] TileType GetTile(i32 tx, i32 ty) const noexcept;
    [[nodiscard]] u8 ComputeAutoTileMask4(i32 tx, i32 ty, TileType matchType = TileType::Solid) const noexcept;
    [[nodiscard]] bool IsChunkSolidEmpty(i32 cx, i32 cy) const noexcept;

    void RebuildDynamicGrid(const World& world, SpatialRebuildMode mode = SpatialRebuildMode::All) noexcept;

    [[nodiscard]] SlabImpact2D SweepAabb(
        EntityId ignoreEntity,
        Vec2 center,
        Vec2 halfExtents,
        Vec2 delta,
        u16 solidMask = 1,
        bool ignoreOneWay = false) const noexcept;

    [[nodiscard]] RayHit2D Raycast(
        Vec2 origin,
        Vec2 direction,
        f32 maxDist,
        u16 layerMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity) const noexcept;

    [[nodiscard]] RayHit2D Linecast(
        Vec2 start,
        Vec2 end,
        u16 layerMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity) const noexcept {
        const Vec2 delta = end - start;
        const f32 dist = delta.Length();
        if (dist <= 1e-6f) return RayHit2D{};
        return Raycast(start, delta / dist, dist, layerMask, ignoreEntity);
    }

    [[nodiscard]] bool HasLineOfSight(
        Vec2 start,
        Vec2 end,
        u16 solidMask = 1U,
        EntityId ignoreEntity = kNullEntity) const noexcept {
        return !Linecast(start, end, solidMask, ignoreEntity).hit;
    }

    [[nodiscard]] RayHit2D CircleCast(
        Vec2 origin,
        f32 radius,
        Vec2 direction,
        f32 maxDist,
        u16 layerMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity) const noexcept;

    [[nodiscard]] u32 OverlapAabb(
        const Rect& area,
        u16 layerMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity) const noexcept;

    [[nodiscard]] u32 OverlapCircle(
        Circle circle,
        u16 layerMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity) const noexcept;

    [[nodiscard]] ContactFlags SampleEnvironmentFlags(const Rect& worldBox) const noexcept;
    [[nodiscard]] f32 SampleSlopeSurfaceY(i32 tx, i32 ty, f32 worldX, Vec2& outNormal) const noexcept;

    [[nodiscard]] constexpr u16 MapWidth() const noexcept { return m_width; }
    [[nodiscard]] constexpr u16 MapHeight() const noexcept { return m_height; }
    [[nodiscard]] constexpr f32 TileSize() const noexcept { return m_tileSize; }
    [[nodiscard]] constexpr Vec2 Origin() const noexcept { return m_origin; }
    [[nodiscard]] constexpr std::span<const GridBox> ActiveBoxes() const noexcept {
        return m_boxes.subspan(0, m_boxCount);
    }

private:
    std::span<TileType> m_tiles{};
    std::span<u64> m_solidChunks{};
    std::span<GridBox> m_boxes{};
    u16 m_maxWidth{0};
    u16 m_maxHeight{0};
    u16 m_width{0};
    u16 m_height{0};
    u16 m_chunksX{0};
    u16 m_chunksY{0};
    u32 m_boxCount{0};
    f32 m_tileSize{1.0f};
    f32 m_invTileSize{1.0f};
    Vec2 m_origin{0.0f, 0.0f};
};

enum class TriggerEventKind : u8 {
    OnEnter = 0,
    OnStay = 1,
    OnExit = 2,
};

struct alignas(4) TriggerContactEvent {
    EntityId triggerEntity{kNullEntity};
    EntityId actorEntity{kNullEntity};
    u16 triggerTag{0};
    TriggerEventKind kind{TriggerEventKind::OnEnter};
    u8 flags{0};
};
static_assert(sizeof(TriggerContactEvent) == 12);

// Tracks overlapping TriggerVolume2D pairs across ticks and emits OnEnter, OnStay, and OnExit events.
class ContactListener2D {
public:
    static constexpr usize kMaxActivePairs = 64;
    struct PairRecord {
        u64 pairKey{0};
        EntityId triggerEntity{kNullEntity};
        EntityId actorEntity{kNullEntity};
        u16 triggerTag{0};
        bool presentCurrTick{false};
        u8 flags{0};
    };

    constexpr void Clear() noexcept { m_pairCount = 0; }

    [[nodiscard]] u32 StepAndCollectTriggerEvents(
        const World& world,
        const SpatialIndex2D& spatial,
        std::span<TriggerContactEvent> outEvents) noexcept;

    [[nodiscard]] constexpr usize ActivePairCount() const noexcept { return m_pairCount; }

private:
    std::array<PairRecord, kMaxActivePairs> m_pairs{};
    usize m_pairCount{0};
};

} // namespace en
