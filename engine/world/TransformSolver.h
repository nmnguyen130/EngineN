#pragma once

#include "core/Memory.h"
#include "world/WorldTypes.h"

namespace en {

// Non-recursive scene-graph hierarchy and world-transform solver.
// Non-owning view over World's AlignedArena; traverses arbitrary-fanout Left-Child Right-Sibling subtrees via parent links.
// Invariant: whenever bit idx in m_movedWords is 0, m_prev[idx] equals m_world[idx].ToSnapshot().
class alignas(64) TransformSolver {
public:
    [[nodiscard]] bool Init(AlignedArena& arena, u16 maxEntities) noexcept;
    void Clear(u16 maxAllocatedIndex) noexcept;

    void InitializeSlot(
        u16 idx,
        const LocalTransform2D& local,
        u16 parentIdx = kInvalidDenseIndex,
        bool keepWorldTransform = false) noexcept;

    constexpr void LinkLoadedSlot(u16 idx, const LocalTransform2D& local, u16 parentIdx) noexcept {
        m_local[idx] = local;
        m_parent[idx] = parentIdx;
        if (parentIdx != kInvalidDenseIndex) {
            m_nextSibling[idx] = m_firstChild[parentIdx];
            m_firstChild[parentIdx] = idx;
        }
        MarkDirty(idx);
    }

    void UnlinkSlot(u16 idx, u16 maxEntities = kInvalidDenseIndex) noexcept;

    [[nodiscard]] Result<void, WorldErrc> SetParent(
        u16 childIdx,
        u16 parentIdx,
        u16 maxEntities = kInvalidDenseIndex,
        bool keepWorldTransform = false) noexcept;

    constexpr void MarkDirty(u16 idx) noexcept {
        m_dirtyWords[idx >> 6U] |= (1ULL << (idx & 63U));
    }

    [[nodiscard]] constexpr bool IsDirty(u16 idx) const noexcept {
        return (m_dirtyWords[idx >> 6U] & (1ULL << (idx & 63U))) != 0ULL;
    }

    template <typename Fn>
    constexpr void TraverseSubtree(u16 rootIdx, u16 maxEntities, Fn&& fn) const noexcept {
        if (rootIdx == 0U || rootIdx >= maxEntities) {
            return;
        }
        u16 cur = rootIdx;
        for (;;) {
            fn(cur);
            const u16 child = m_firstChild[cur];
            if (child != kInvalidDenseIndex) {
                cur = child;
                continue;
            }
            while (cur != rootIdx && m_nextSibling[cur] == kInvalidDenseIndex) {
                cur = m_parent[cur];
            }
            if (cur == rootIdx) {
                break;
            }
            cur = m_nextSibling[cur];
        }
    }

    void MarkSubtreeDirty(u16 rootIdx, u16 maxEntities = kInvalidDenseIndex) noexcept;
    void TeleportSubtree(u16 rootIdx, u16 maxEntities = kInvalidDenseIndex) noexcept;

    void SnapshotPreviousTransforms(
        std::span<const u64> aliveWords,
        const u8* timeChannels,
        u8 frozenMask,
        u16 maxAllocatedIndex) noexcept;

    void PropagateHierarchy(
        std::span<const u64> aliveWords,
        u16 maxAllocatedIndex) noexcept;

    [[nodiscard]] constexpr LocalTransform2D& Local(u16 idx) noexcept { return m_local[idx]; }
    [[nodiscard]] constexpr const LocalTransform2D& Local(u16 idx) const noexcept { return m_local[idx]; }

    [[nodiscard]] constexpr WorldTransform2D& World(u16 idx) noexcept { return m_world[idx]; }
    [[nodiscard]] constexpr const WorldTransform2D& World(u16 idx) const noexcept { return m_world[idx]; }

    [[nodiscard]] constexpr PrevWorldSnapshot2D& Prev(u16 idx) noexcept { return m_prev[idx]; }
    [[nodiscard]] constexpr const PrevWorldSnapshot2D& Prev(u16 idx) const noexcept { return m_prev[idx]; }

    [[nodiscard]] constexpr std::span<const WorldTransform2D> WorldSpan(u16 count) const noexcept {
        return {m_world, count};
    }

    [[nodiscard]] constexpr std::span<const PrevWorldSnapshot2D> PrevSpan(u16 count) const noexcept {
        return {m_prev, count};
    }

    [[nodiscard]] constexpr std::span<const u64> MovedWords(u16 wordCount) const noexcept {
        return {m_movedWords, wordCount};
    }

    [[nodiscard]] constexpr std::span<const u64> DirtyWords(u16 wordCount) const noexcept {
        return {m_dirtyWords, wordCount};
    }

    [[nodiscard]] constexpr u16 ParentOf(u16 idx) const noexcept { return m_parent[idx]; }
    [[nodiscard]] constexpr u16 FirstChildOf(u16 idx) const noexcept { return m_firstChild[idx]; }
    [[nodiscard]] constexpr u16 NextSiblingOf(u16 idx) const noexcept { return m_nextSibling[idx]; }
    [[nodiscard]] u16 MaxSubtreeDepth(u16 rootIdx, u16 maxEntities = kInvalidDenseIndex) const noexcept;

    static void ComputeWorldFromLocal(const LocalTransform2D& loc, WorldTransform2D& outW) noexcept;
    static void CombineParentChild(
        const WorldTransform2D& p,
        const LocalTransform2D& c,
        WorldTransform2D& outW) noexcept;
    static void InverseCombineParentChild(
        const WorldTransform2D& p,
        const WorldTransform2D& childWorld,
        LocalTransform2D& outLocal) noexcept;

private:
    void UpdateNodeWorld(u16 cur) noexcept {
        const u16 p = m_parent[cur];
        if (p == kInvalidDenseIndex) {
            ComputeWorldFromLocal(m_local[cur], m_world[cur]);
        } else {
            CombineParentChild(m_world[p], m_local[cur], m_world[cur]);
        }
    }
    void DetachFromParentList(u16 childIdx) noexcept;
    void ResolveDirtyChain(u16 idx, u16 maxEntities = kInvalidDenseIndex) noexcept;
    void EvaluateDirtySubtree(u16 rootIdx, u16 maxEntities = kInvalidDenseIndex) noexcept;

    LocalTransform2D* m_local{nullptr};
    WorldTransform2D* m_world{nullptr};
    PrevWorldSnapshot2D* m_prev{nullptr};
    u16* m_parent{nullptr};
    u16* m_firstChild{nullptr};
    u16* m_nextSibling{nullptr};
    u64* m_dirtyWords{nullptr};
    u64* m_movedWords{nullptr};
};
static_assert(sizeof(TransformSolver) == 64);

} // namespace en
