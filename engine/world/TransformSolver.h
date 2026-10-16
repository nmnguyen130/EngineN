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
// Non-owning over arena memory slabs carved by World::Init().
// Invariant: In m_hierarchyOrder[0..m_childCount), every parentIdx appears before its descendants.
class TransformSolver final {
public:
    void BindArena(
        std::span<LocalTransform2D> local,
        std::span<WorldTransform2D> world,
        std::span<PrevWorldSnapshot2D> prev,
        std::span<HierarchyNode> treeNodes,
        std::span<u8> depths,
        std::span<HierarchyStep> hierarchyOrder,
        std::span<u64> spatialAliveWords,
        std::span<u64> spatialDynamicWords,
        std::span<u64> rootDirtyWords,
        std::span<u64> childDirtyWords,
        std::span<u64> subtreeDirtyWords) noexcept;

    void Clear() noexcept;

    void InitSlot(u16 idx, const LocalTransform2D& initialLocal, bool isStatic) noexcept;
    void DisableSlot(u16 idx) noexcept;

    [[nodiscard]] constexpr const LocalTransform2D& GetLocal(u16 idx) const noexcept { return m_local[idx]; }
    [[nodiscard]] constexpr const WorldTransform2D& GetWorld(u16 idx) const noexcept { return m_world[idx]; }
    [[nodiscard]] constexpr const PrevWorldSnapshot2D& GetPrevWorld(u16 idx) const noexcept { return m_prev[idx]; }
    [[nodiscard]] constexpr std::span<const WorldTransform2D> WorldSpan() const noexcept { return m_world; }
    [[nodiscard]] constexpr std::span<const PrevWorldSnapshot2D> PrevWorldSpan() const noexcept { return m_prev; }
    [[nodiscard]] constexpr u16 GetParentIndex(u16 idx) const noexcept { return m_tree[idx].parent; }
    [[nodiscard]] constexpr u8 GetDepth(u16 idx) const noexcept { return m_depths[idx]; }

    [[nodiscard]] constexpr LocalTransform2D& ModifyLocal(u16 idx) noexcept {
        MarkDirty(idx);
        return m_local[idx];
    }

    constexpr void SetLocal(u16 idx, const LocalTransform2D& local) noexcept {
        m_local[idx] = local;
        MarkDirty(idx);
    }

    constexpr void MarkDirty(u16 idx) noexcept {
        const u32 w = idx >> 6;
        const u64 b = 1ULL << (idx & 63u);
        if (m_tree[idx].parent == 0) {
            m_rootDirtyWords[w] |= b;
        } else {
            m_childDirtyWords[w] |= b;
        }
    }

    constexpr void SyncPrevToCurrent(u16 idx) noexcept {
        const WorldTransform2D& w = m_world[idx];
        m_prev[idx] = PrevWorldSnapshot2D{
            .x           = w.x,
            .y           = w.y,
            .scaleX      = w.scaleX,
            .scaleY      = w.scaleY,
            .worldRotRad = w.worldRotRad,
            .worldDepth  = w.worldDepth,
        };
    }

    [[nodiscard]] Result<void, WorldErrc> SetParent(u16 childIdx, u16 newParentIdx) noexcept;
    void DetachChildrenToRoot(u16 parentIdx) noexcept;

    void SnapshotPreviousTransforms(
        u16 maxAllocatedIndex,
        u8 activeChannelMask,
        std::span<const time::TimeChannel> entityChannels) noexcept;

    void Propagate(u16 maxAllocatedIndex) noexcept;

private:
    void UnlinkFromParent(u16 childIdx) noexcept;
    void RebuildHierarchyOrder(u16 maxAllocatedIndex) noexcept;
    void AppendSubtreeDFS(u16 parentIdx) noexcept;

    std::span<LocalTransform2D> m_local{};
    std::span<WorldTransform2D> m_world{};
    std::span<PrevWorldSnapshot2D> m_prev{};
    std::span<HierarchyNode> m_tree{};
    std::span<u8> m_depths{};
    std::span<HierarchyStep> m_hierarchyOrder{};

    std::span<u64> m_spatialAliveWords{};
    std::span<u64> m_spatialDynamicWords{};
    std::span<u64> m_rootDirtyWords{};
    std::span<u64> m_childDirtyWords{};
    std::span<u64> m_subtreeDirtyWords{};

    u32 m_childCount{0};
    bool m_orderDirty{false};
};

} // namespace en::world