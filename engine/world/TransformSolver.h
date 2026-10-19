#pragma once

#include "world/WorldTypes.h"

#include <span>

namespace en::world {

struct HierarchyNode {
    u16 parent{0};
    u16 firstChild{0};
    u16 nextSibling{0};
    u16 prevSibling{0};
};
static_assert(sizeof(HierarchyNode) == 8);

struct HierarchyStep {
    u16 childIdx{0};
    u16 parentIdx{0};
};
static_assert(sizeof(HierarchyStep) == 4);

// Manages direct-indexed SoA spatial transforms and a flat topological hierarchy array.
// Non-owning 64B L1-aligned descriptor (1 cache line) over arena slabs carved by World::Init().
// Invariant: If bit idx in m_movedWords is 0, m_prev[idx] is identical to m_world[idx].ToSnapshot().
class alignas(64) TransformSolver final {
public:
    void BindArena(
        std::span<LocalTransform2D> local,
        std::span<WorldTransform2D> world,
        std::span<PrevWorldSnapshot2D> prev,
        std::span<HierarchyNode> treeNodes,
        std::span<HierarchyStep> hierarchyOrder,
        std::span<u64> dirtyAndAliveWords,
        std::span<u64> movedWords) noexcept;

    void Clear(u16 maxAllocatedIndex = 0) noexcept;

    void InitSlot(u16 idx, const LocalTransform2D& initialLocal) noexcept;
    void DisableSlot(u16 idx) noexcept;

    [[nodiscard]] constexpr const LocalTransform2D& GetLocal(u16 idx) const noexcept { return m_local[idx]; }
    [[nodiscard]] constexpr const WorldTransform2D& GetWorld(u16 idx) const noexcept { return m_world[idx]; }
    [[nodiscard]] constexpr const PrevWorldSnapshot2D& GetPrevWorld(u16 idx) const noexcept { return m_prev[idx]; }
    [[nodiscard]] constexpr std::span<const WorldTransform2D> WorldSpan() const noexcept { return {m_world, m_maxEntities}; }
    [[nodiscard]] constexpr std::span<const PrevWorldSnapshot2D> PrevWorldSpan() const noexcept { return {m_prev, m_maxEntities}; }
    [[nodiscard]] constexpr std::span<const u64> MovedWordsSpan() const noexcept { return {m_movedWords, m_wordCount}; }
    [[nodiscard]] constexpr u16 GetParentIndex(u16 idx) const noexcept { return m_tree[idx].parent; }

    [[nodiscard]] constexpr u8 GetDepth(u16 idx) const noexcept {
        u8 depth = 0;
        for (u16 cur = m_tree[idx].parent; cur != 0 && depth < kMaxHierarchyDepth; cur = m_tree[cur].parent) {
            ++depth;
        }
        return depth;
    }

    [[nodiscard]] constexpr LocalTransform2D& ModifyLocal(u16 idx) noexcept {
        MarkDirty(idx);
        return m_local[idx];
    }

    constexpr void SetLocal(u16 idx, const LocalTransform2D& local) noexcept {
        m_local[idx] = local;
        MarkDirty(idx);
    }

    constexpr void MarkDirty(u16 idx) noexcept {
        m_dirtyWords[idx >> 6] |= (1ULL << (idx & 63u));
    }

    constexpr void SyncPrevToCurrent(u16 idx) noexcept {
        m_prev[idx] = m_world[idx].ToSnapshot();
        m_movedWords[idx >> 6] &= ~(1ULL << (idx & 63u));
    }

    [[nodiscard]] Result<void, WorldErrc> SetParent(u16 childIdx, u16 newParentIdx) noexcept;
    void DetachChildrenToRoot(u16 parentIdx) noexcept;

    void SnapshotPreviousTransforms(
        u16 maxAllocatedIndex,
        u8 activeChannelMask,
        const time::TimeChannel* entityChannels) noexcept;

    void Propagate(u16 maxAllocatedIndex) noexcept;

private:
    [[nodiscard]] constexpr u64* SpatialAliveWords() noexcept { return m_dirtyWords + m_wordCount; }
    [[nodiscard]] constexpr const u64* SpatialAliveWords() const noexcept { return m_dirtyWords + m_wordCount; }

    static constexpr void ComputeRootTransform(const LocalTransform2D& loc, WorldTransform2D& wrld) noexcept {
        wrld.x           = loc.x;
        wrld.y           = loc.y;
        wrld.scaleX      = loc.scaleX;
        wrld.scaleY      = loc.scaleY;
        wrld.worldRotRad = NormalizeAngleRad(loc.rotationRad);
        wrld.worldDepth  = loc.depthOffset;
        FastSinCos(wrld.worldRotRad, wrld.cosRot, wrld.sinRot);
    }

    void UnlinkFromParent(u16 childIdx) noexcept;
    void RebuildHierarchyOrder(u16 maxAllocatedIndex) noexcept;
    [[nodiscard]] u8 MaxSubtreeDepth(u16 rootIdx) const noexcept;

    LocalTransform2D* m_local{nullptr};
    WorldTransform2D* m_world{nullptr};
    PrevWorldSnapshot2D* m_prev{nullptr};
    HierarchyNode* m_tree{nullptr};
    HierarchyStep* m_hierarchyOrder{nullptr};
    u64* m_dirtyWords{nullptr};
    u64* m_movedWords{nullptr};

    u16 m_maxEntities{0};
    u16 m_wordCount{0};
    u16 m_childCount{0};
    bool m_orderDirty{false};
    u8 m_reserved{0};
};
static_assert(sizeof(TransformSolver) == 64);

} // namespace en::world
