#pragma once

#include "physics/DynamicsSolver2D.h"

namespace en {

struct BlobRecord;
class AssetBank;

// Fixed-rate 2D physics orchestrator. Owns spatial index + manifold/event buffers from one Arena.
// Zero heap after Init(). sizeof == 192 (3 cache lines).
class alignas(64) PhysicsSystem {
public:
    PhysicsSystem() noexcept = default;
    ~PhysicsSystem() noexcept { Shutdown(); }

    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;
    PhysicsSystem(PhysicsSystem&&) = delete;
    PhysicsSystem& operator=(PhysicsSystem&&) = delete;

    [[nodiscard]] Result<void, PhysicsErrc> Init(
        World& world,
        const PhysicsConfig& cfg = {}) noexcept;

    void Shutdown() noexcept;
    void Clear() noexcept;

    [[nodiscard]] Result<void, PhysicsErrc> BindCollisionMapBlob(
        const AssetBank& bank,
        BlobHandle collisionBlob) noexcept;

    [[nodiscard]] Result<void, PhysicsErrc> BindCollisionMapBytes(
        std::span<const u8> blobBytes) noexcept {
        return m_spatial.BindCollisionMap(m_arena, blobBytes);
    }

    [[nodiscard]] Result<void, PhysicsErrc> BuildTileMap(
        u16 widthTiles,
        u16 heightTiles,
        f32 tileSizeMeters,
        f32 originX,
        f32 originY,
        std::span<const TileMaterialFlags> tiles) noexcept {
        return m_spatial.BuildTileMapFromGrid(
            m_arena, widthTiles, heightTiles, tileSizeMeters, originX, originY, tiles);
    }

    void Step(World& world, const SimStepView& step) noexcept;

    [[nodiscard]] RayHit2D Raycast(
        f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist,
        u16 categoryMask = 0xFFFFU,
        EntityId ignoreEntity = kNullEntity,
        bool includeSensors = true) const noexcept {
        return m_spatial.Raycast(originX, originY, dirX, dirY, maxDist, categoryMask, ignoreEntity, includeSensors);
    }

    [[nodiscard]] SweepHit2D SweepAabb(
        EntityId ignoreEntity,
        f32 centerX, f32 centerY, f32 halfW, f32 halfH,
        f32 deltaX, f32 deltaY,
        u16 solidMask = 0xFFFFU,
        bool ignoreOneWay = false) const noexcept {
        return m_spatial.SweepAabb(
            ignoreEntity, centerX, centerY, halfW, halfH, deltaX, deltaY, solidMask, ignoreOneWay);
    }

    [[nodiscard]] SweepHit2D CastCircle(
        EntityId ignoreEntity,
        f32 centerX, f32 centerY, f32 radius,
        f32 deltaX, f32 deltaY,
        u16 solidMask = 0xFFFFU,
        bool ignoreOneWay = false) const noexcept {
        return m_spatial.CastCircle(
            ignoreEntity, centerX, centerY, radius, deltaX, deltaY, solidMask, ignoreOneWay);
    }

    [[nodiscard]] u32 QueryAabb(
        f32 minX, f32 minY, f32 maxX, f32 maxY,
        u16 categoryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity,
        bool includeSensors = true) const noexcept {
        return m_spatial.QueryAabb(minX, minY, maxX, maxY, categoryMask, outEntities, ignoreEntity, includeSensors);
    }

    [[nodiscard]] u32 QueryCircle(
        f32 centerX, f32 centerY, f32 radius,
        u16 categoryMask,
        std::span<EntityId> outEntities,
        EntityId ignoreEntity = kNullEntity,
        bool includeSensors = true) const noexcept {
        return m_spatial.QueryCircle(centerX, centerY, radius, categoryMask, outEntities, ignoreEntity, includeSensors);
    }

    [[nodiscard]] constexpr const SpatialIndex2D& Spatial() const noexcept { return m_spatial; }
    [[nodiscard]] constexpr const PhysicsConfig& Config() const noexcept { return m_cfg; }
    constexpr void SetConfig(const PhysicsConfig& cfg) noexcept { m_cfg = cfg; }

    [[nodiscard]] constexpr std::span<const ContactManifold2D> ActiveManifolds() const noexcept {
        if (!m_manifolds) return {};
        return {m_manifolds + (static_cast<usize>(m_manifoldFlip) * m_cfg.maxManifolds), m_activeManifoldCount};
    }

    [[nodiscard]] constexpr std::span<const ContactEvent2D> DrainContactEvents() noexcept {
        const std::span<const ContactEvent2D> res{m_events, m_eventCount};
        m_eventCount = 0U;
        return res;
    }

    [[nodiscard]] constexpr std::span<const ContactEvent2D> PeekContactEvents() const noexcept {
        return {m_events, m_eventCount};
    }

    [[nodiscard]] constexpr u64 StepCount() const noexcept { return m_stepCount; }
    [[nodiscard]] constexpr u64 TotalResolvedContacts() const noexcept { return m_totalResolvedContacts; }
    [[nodiscard]] constexpr u64 TotalSpeculativeHits() const noexcept { return m_totalSpeculativeHits; }

    [[nodiscard]] u64 ComputeAuditHash(const World& world) const noexcept;

private:
    SpatialIndex2D m_spatial{};

    PhysicsConfig m_cfg{};
    ContactManifold2D* m_manifolds{nullptr};
    ContactEvent2D* m_events{nullptr};

    Arena m_arena{};
    u16 m_activeManifoldCount{0U};
    u16 m_prevManifoldCount{0U};
    u16 m_eventCount{0U};
    u8 m_manifoldFlip{0U};
    u8 m_initialized{0U};
    u64 m_stepCount{0ULL};
    u64 m_totalResolvedContacts{0ULL};
    u64 m_totalSpeculativeHits{0ULL};
};
static_assert(sizeof(PhysicsSystem) == 192);

} // namespace en
