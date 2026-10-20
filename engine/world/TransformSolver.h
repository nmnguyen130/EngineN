#pragma once

#include "core/Memory.h"
#include "world/WorldTypes.h"

namespace en {

// Iterative topological scene-graph hierarchy and world-transform solver.
// Non-owning view over World's AlignedArena; fits in a single 64B cache line.
// Invariant: whenever bit idx in m_movedWords is 0, m_prev[idx] equals m_world[idx].ToSnapshot().
class alignas(64) TransformSolver {
public:
    [[nodiscard]] bool Init(AlignedArena& arena, u16 maxEntities) noexcept;
    void Clear(u16 maxAllocatedIndex) noexcept;

    void InitializeSlot(u16 idx, const LocalTransform2D& local) noexcept;
    void UnlinkSlot(u16 idx) noexcept;

    [[nodiscard]] Result<void, WorldErrc> SetParent(u16 childIdx, u16 parentIdx) noexcept;

    constexpr void MarkDirty(u16 idx) noexcept {
        m_dirtyWords[idx >> 6U] |= (1ULL << (idx & 63U));
    }
    void MarkSubtreeDirty(u16 rootIdx) noexcept;

    void SnapshotPreviousTransforms(
        std::span<const u64> aliveWords,
        std::span<const u8> timeChannels,
        u8 frozenMask,
        u16 maxAllocatedIndex) noexcept;

    void PropagateHierarchy(std::span<const u64> aliveWords, u16 maxAllocatedIndex) noexcept;

    [[nodiscard]] constexpr LocalTransform2D& Local(u16 idx) noexcept { return m_local[idx]; }
    [[nodiscard]] constexpr const LocalTransform2D& Local(u16 idx) const noexcept { return m_local[idx]; }
    [[nodiscard]] constexpr WorldTransform2D& World(u16 idx) noexcept { return m_world[idx]; }
    [[nodiscard]] constexpr const WorldTransform2D& World(u16 idx) const noexcept { return m_world[idx]; }
    [[nodiscard]] constexpr PrevWorldSnapshot2D& Prev(u16 idx) noexcept { return m_prev[idx]; }
    [[nodiscard]] constexpr const PrevWorldSnapshot2D& Prev(u16 idx) const noexcept { return m_prev[idx]; }

    [[nodiscard]] constexpr std::span<const WorldTransform2D> WorldSpan(u16 count) const noexcept {
        return m_world.subspan(0, count);
    }
    [[nodiscard]] constexpr std::span<const PrevWorldSnapshot2D> PrevSpan(u16 count) const noexcept {
        return m_prev.subspan(0, count);
    }
    [[nodiscard]] constexpr std::span<const u64> MovedWords() const noexcept { return m_movedWords; }
    [[nodiscard]] constexpr u16 ParentOf(u16 idx) const noexcept { return m_parent[idx]; }

private:
    static void ComputeWorldFromLocal(const LocalTransform2D& loc, WorldTransform2D& outW) noexcept;
    static void CombineParentChild(const WorldTransform2D& p, const LocalTransform2D& c, WorldTransform2D& outW) noexcept;

    std::span<LocalTransform2D> m_local{};
    std::span<WorldTransform2D> m_world{};
    std::span<PrevWorldSnapshot2D> m_prev{};
    std::span<u16> m_parent{};
    std::span<u16> m_firstChild{};
    std::span<u16> m_nextSibling{};
    std::span<u64> m_dirtyWords{};
    std::span<u64> m_movedWords{};
};

} // namespace en
