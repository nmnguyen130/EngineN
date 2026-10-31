#pragma once

#include "assets/AssetBank.h"
#include "physics/SpatialGrid2D.h"

namespace en {

// Deterministic 2D Metroidvania physics solver managing tilemaps, moving platforms, swept slide, and triggers.
// Move-disabled owner of an Arena; registers Collider2D and PhysicsBody2D pools on Init.
// Invariant: sizeof(PhysicsSystem) == 152, and zero heap allocations occur after Init.
class PhysicsSystem {
public:
    struct ActivePair {
        u64 key{0};
        u16 userTag{0};
        u8  isTrigger{0};
        u8  pad{0};
    };
    static_assert(sizeof(ActivePair) == 16);

    PhysicsSystem() noexcept = default;
    ~PhysicsSystem() noexcept { Shutdown(); }

    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    [[nodiscard]] Result<void, PhysicsErrc> Init(
        World& world,
        const PhysicsConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;
    void Clear() noexcept;

    [[nodiscard]] Result<void, PhysicsErrc> BuildTileMap(
        u16 width,
        u16 height,
        f32 tileSize,
        Vec2 origin,
        std::span<const TileFlag> tiles) noexcept {
        return m_grid.BuildTileMap(m_arena, width, height, tileSize, origin, tiles);
    }

    [[nodiscard]] Result<void, PhysicsErrc> BindCollisionMapBlob(
        const AssetBank& bank,
        BlobHandle blob) noexcept {
        const BlobRecord& rec = bank.GetBlob(blob);
        return (rec.data && rec.size > 0U)
            ? m_grid.LoadTileMapBlob(m_arena, rec.Bytes())
            : Err(PhysicsErrc::CorruptedBlob);
    }

    void SetTile(i32 tx, i32 ty, TileFlag flag) noexcept { m_grid.SetTile(tx, ty, flag); }
    [[nodiscard]] TileFlag SampleTile(i32 tx, i32 ty) const noexcept { return m_grid.SampleTile(tx, ty); }
    [[nodiscard]] TileFlag SampleTileWorld(Vec2 pos) const noexcept { return m_grid.SampleTileWorld(pos); }

    void Step(World& world, const TickStep& step) noexcept;

    // Sweeps `mover` by `delta` against tilemaps and SpatialGrid2D colliders, sliding along surfaces.
    SweepResult2D SweepMove(
        World& world,
        EntityId mover,
        Vec2 delta,
        bool ignoreOneWay = false) noexcept;

    [[nodiscard]] RayHit2D Raycast(
        Vec2 origin,
        Vec2 dir,
        f32 maxDist,
        u16 queryMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity,
        bool includeTriggers = false) const noexcept {
        return m_grid.Raycast(origin, dir, maxDist, queryMask, ignoreEntity, includeTriggers);
    }

    [[nodiscard]] u32 RaycastAll(
        Vec2 origin,
        Vec2 dir,
        f32 maxDist,
        u16 queryMask,
        std::span<RayHit2D> outHits,
        EntityId ignoreEntity = kNullEntity,
        bool includeTriggers = true) const noexcept {
        return m_grid.RaycastAll(origin, dir, maxDist, queryMask, outHits, ignoreEntity, includeTriggers);
    }

    [[nodiscard]] SweepResult2D SweepBox(
        const Rect& movingBox,
        Vec2 delta,
        u16 queryMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity,
        bool ignoreOneWay = false) const noexcept {
        return m_grid.SweepBox(movingBox, delta, queryMask, ignoreEntity, ignoreOneWay);
    }

    [[nodiscard]] u32 QueryBox(
        const Rect& queryBox,
        u16 queryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity,
        bool includeTriggers = true) const noexcept {
        return m_grid.QueryBox(queryBox, queryMask, outEntities, ignoreEntity, includeTriggers);
    }

    [[nodiscard]] u32 QueryCircle(
        Vec2 center,
        f32 radius,
        u16 queryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity,
        bool includeTriggers = true) const noexcept {
        return m_grid.QueryCircle(center, radius, queryMask, outEntities, ignoreEntity, includeTriggers);
    }

    [[nodiscard]] constexpr std::span<const ContactEvent2D> ContactEvents() const noexcept {
        return {m_events, m_eventCount};
    }
    [[nodiscard]] constexpr std::span<const ContactEvent2D> DrainContactEvents() noexcept {
        const std::span<const ContactEvent2D> out{m_events, m_eventCount};
        m_eventCount = 0U;
        return out;
    }

    [[nodiscard]] constexpr const SpatialGrid2D& Grid() const noexcept { return m_grid; }
    [[nodiscard]] constexpr const PhysicsConfig& Config() const noexcept { return m_cfg; }
    constexpr void SetConfig(const PhysicsConfig& cfg) noexcept { m_cfg = cfg; }
    [[nodiscard]] constexpr u64 StepCount() const noexcept { return m_stepCount; }
    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_initialized != 0U; }

    [[nodiscard]] u64 ComputeAuditHash(const World& world) const noexcept;

private:
    void StepKinematicPlatforms(World& world, const TickStep& step) noexcept;
    void StepDynamicBodies(World& world, const TickStep& step) noexcept;
    void GatherContactEvents(const World& world) noexcept;

    SpatialGrid2D   m_grid{};
    PhysicsConfig   m_cfg{};
    Arena           m_arena{};
    ContactEvent2D* m_events{nullptr};
    ActivePair*     m_pairs{nullptr};
    u64             m_stepCount{0};
    u16             m_eventCount{0};
    u16             m_prevPairCount{0};
    u8              m_pairFlip{0};
    u8              m_initialized{0};
    u16             m_pad{0};
};
static_assert(sizeof(PhysicsSystem) == 152);

} // namespace en
