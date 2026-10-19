#include "world/TransformSolver.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace en::world {

void TransformSolver::BindArena(
    std::span<LocalTransform2D> local,
    std::span<WorldTransform2D> world,
    std::span<PrevWorldSnapshot2D> prev,
    std::span<HierarchyNode> treeNodes,
    std::span<HierarchyStep> hierarchyOrder,
    std::span<u64> dirtyAndAliveWords,
    std::span<u64> movedWords) noexcept {
    m_local          = local.data();
    m_world          = world.data();
    m_prev           = prev.data();
    m_tree           = treeNodes.data();
    m_hierarchyOrder = hierarchyOrder.data();
    m_dirtyWords     = dirtyAndAliveWords.data();
    m_movedWords     = movedWords.data();
    m_maxEntities    = static_cast<u16>(local.size());
    m_wordCount      = static_cast<u16>(movedWords.size());
    Clear(0);
}

void TransformSolver::Clear(u16 maxAllocatedIndex) noexcept {
    const u32 clearSlots = (maxAllocatedIndex == 0)
        ? m_maxEntities
        : std::min<u32>(static_cast<u32>(maxAllocatedIndex) + 1u, m_maxEntities);
    const u32 clearWords = (maxAllocatedIndex == 0)
        ? m_wordCount
        : std::min<u32>((static_cast<u32>(maxAllocatedIndex) + 64u) >> 6, m_wordCount);

    std::fill_n(m_tree, clearSlots, HierarchyNode{});
    std::memset(m_dirtyWords, 0, clearWords * sizeof(u64));
    std::memset(SpatialAliveWords(), 0, clearWords * sizeof(u64));
    std::memset(m_movedWords, 0, clearWords * sizeof(u64));

    if (m_maxEntities > 0) {
        m_local[0] = LocalTransform2D{};
        m_world[0] = WorldTransform2D{};
        m_prev[0]  = PrevWorldSnapshot2D{};
    }
    m_childCount = 0;
    m_orderDirty = false;
}

void TransformSolver::InitSlot(u16 idx, const LocalTransform2D& initialLocal) noexcept {
    m_local[idx] = initialLocal;
    m_tree[idx]  = HierarchyNode{};
    ComputeRootTransform(initialLocal, m_world[idx]);
    SyncPrevToCurrent(idx);

    const u32 w = idx >> 6;
    const u64 b = 1ULL << (idx & 63u);
    SpatialAliveWords()[w] |= b;
    m_dirtyWords[w]        &= ~b;
}

void TransformSolver::DisableSlot(u16 idx) noexcept {
    const u32 w = idx >> 6;
    const u64 b = 1ULL << (idx & 63u);
    if ((SpatialAliveWords()[w] & b) == 0) {
        return;
    }
    DetachChildrenToRoot(idx);
    if (m_tree[idx].parent != 0) {
        UnlinkFromParent(idx);
        m_orderDirty = true;
    }
    const u64 mask = ~b;
    SpatialAliveWords()[w] &= mask;
    m_dirtyWords[w]        &= mask;
    m_movedWords[w]        &= mask;
}

void TransformSolver::UnlinkFromParent(u16 childIdx) noexcept {
    HierarchyNode& node = m_tree[childIdx];
    const u16 parentIdx = node.parent;
    if (parentIdx == 0) {
        return;
    }
    if (node.prevSibling != 0) {
        m_tree[node.prevSibling].nextSibling = node.nextSibling;
    } else {
        m_tree[parentIdx].firstChild = node.nextSibling;
    }
    if (node.nextSibling != 0) {
        m_tree[node.nextSibling].prevSibling = node.prevSibling;
    }
    node.parent      = 0;
    node.prevSibling = 0;
    node.nextSibling = 0;
}

void TransformSolver::DetachChildrenToRoot(u16 parentIdx) noexcept {
    u16 child = m_tree[parentIdx].firstChild;
    if (child == 0) {
        return;
    }
    while (child != 0) {
        const u16 next = m_tree[child].nextSibling;
        m_tree[child].parent      = 0;
        m_tree[child].prevSibling = 0;
        m_tree[child].nextSibling = 0;
        MarkDirty(child);
        child = next;
    }
    m_tree[parentIdx].firstChild = 0;
    m_orderDirty = true;
}

u8 TransformSolver::MaxSubtreeDepth(u16 rootIdx) const noexcept {
    if (m_tree[rootIdx].firstChild == 0) {
        return 0;
    }
    struct StackEntry {
        u16 node;
        u8 depth;
    };
    StackEntry stack[kMaxHierarchyDepth];
    u32 top = 0;
    stack[top++] = {rootIdx, 0};
    u8 maxDepth = 0;

    while (top > 0) {
        const auto [cur, d] = stack[--top];
        for (u16 child = m_tree[cur].firstChild; child != 0; child = m_tree[child].nextSibling) {
            const u8 childDepth = static_cast<u8>(d + 1u);
            maxDepth = std::max(maxDepth, childDepth);
            if (m_tree[child].firstChild != 0 && top < kMaxHierarchyDepth) {
                stack[top++] = {child, childDepth};
            }
        }
    }
    return maxDepth;
}

Result<void, WorldErrc> TransformSolver::SetParent(u16 childIdx, u16 newParentIdx) noexcept {
    if (childIdx == 0 || childIdx == newParentIdx) [[unlikely]] {
        return Err(WorldErrc::HierarchyCycleDetected);
    }
    if (m_tree[childIdx].parent == newParentIdx) {
        return {};
    }

    if (newParentIdx != 0) {
        u16 cursor = newParentIdx;
        u8 ancestorSteps = 0;
        while (cursor != 0) {
            if (cursor == childIdx) [[unlikely]] {
                return Err(WorldErrc::HierarchyCycleDetected);
            }
            cursor = m_tree[cursor].parent;
            if (++ancestorSteps > kMaxHierarchyDepth) [[unlikely]] {
                return Err(WorldErrc::HierarchyDepthExceeded);
            }
        }
        if (static_cast<u32>(ancestorSteps) + MaxSubtreeDepth(childIdx) > kMaxHierarchyDepth) [[unlikely]] {
            return Err(WorldErrc::HierarchyDepthExceeded);
        }
    }

    UnlinkFromParent(childIdx);

    if (newParentIdx != 0) {
        const u16 oldHead = m_tree[newParentIdx].firstChild;
        m_tree[childIdx].parent      = newParentIdx;
        m_tree[childIdx].prevSibling = 0;
        m_tree[childIdx].nextSibling = oldHead;
        if (oldHead != 0) {
            m_tree[oldHead].prevSibling = childIdx;
        }
        m_tree[newParentIdx].firstChild = childIdx;
    }

    MarkDirty(childIdx);
    m_orderDirty = true;
    return {};
}

void TransformSolver::RebuildHierarchyOrder(u16 maxAllocatedIndex) noexcept {
    m_childCount = 0;
    const u32 wordCount = (static_cast<u32>(maxAllocatedIndex) + 64u) >> 6;
    const u64* __restrict aliveWords = SpatialAliveWords();
    u16 stack[kMaxHierarchyDepth];

    for (u32 w = 0; w < wordCount; ++w) {
        u64 alive = aliveWords[w];
        while (alive != 0) {
            const u32 bit = static_cast<u32>(std::countr_zero(alive));
            alive &= (alive - 1u);
            const u16 rootIdx = static_cast<u16>((w << 6) + bit);
            if (m_tree[rootIdx].parent != 0 || m_tree[rootIdx].firstChild == 0) {
                continue;
            }

            u32 top = 0;
            stack[top++] = rootIdx;
            while (top > 0) {
                const u16 parentIdx = stack[--top];
                for (u16 child = m_tree[parentIdx].firstChild; child != 0; child = m_tree[child].nextSibling) {
                    m_hierarchyOrder[m_childCount++] = HierarchyStep{
                        .childIdx  = child,
                        .parentIdx = parentIdx,
                    };
                    if (m_tree[child].firstChild != 0 && top < kMaxHierarchyDepth) {
                        stack[top++] = child;
                    }
                }
            }
        }
    }
    m_orderDirty = false;
}

void TransformSolver::SnapshotPreviousTransforms(
    u16 maxAllocatedIndex,
    u8 activeChannelMask,
    const time::TimeChannel* entityChannels) noexcept {
    if (activeChannelMask == 0 || maxAllocatedIndex == 0) {
        return;
    }
    const u32 wordCount = (static_cast<u32>(maxAllocatedIndex) + 64u) >> 6;
    const auto* __restrict worldPtr = m_world;
    auto* __restrict prevPtr        = m_prev;
    u64* __restrict movedWords      = m_movedWords;

    for (u32 w = 0; w < wordCount; ++w) {
        u64 moved = movedWords[w];
        if (moved == 0) {
            continue;
        }
        if (activeChannelMask == 0xFFu) [[likely]] {
            movedWords[w] = 0;
            do {
                const u32 bit = static_cast<u32>(std::countr_zero(moved));
                moved &= (moved - 1u);
                const u16 idx = static_cast<u16>((w << 6) + bit);
                prevPtr[idx] = worldPtr[idx].ToSnapshot();
            } while (moved != 0);
        } else {
            u64 syncedBits = 0;
            do {
                const u32 bit = static_cast<u32>(std::countr_zero(moved));
                moved &= (moved - 1u);
                const u16 idx = static_cast<u16>((w << 6) + bit);
                if ((activeChannelMask & (1u << static_cast<u8>(entityChannels[idx]))) != 0) {
                    prevPtr[idx] = worldPtr[idx].ToSnapshot();
                    syncedBits |= (1ULL << bit);
                }
            } while (moved != 0);
            movedWords[w] &= ~syncedBits;
        }
    }
}

void TransformSolver::Propagate(u16 maxAllocatedIndex) noexcept {
    if (maxAllocatedIndex == 0) {
        return;
    }
    if (m_orderDirty) [[unlikely]] {
        RebuildHierarchyOrder(maxAllocatedIndex);
    }

    const u32 wordCount = (static_cast<u32>(maxAllocatedIndex) + 64u) >> 6;
    const u64* __restrict aliveWords = SpatialAliveWords();
    const bool hasChildren = (m_childCount != 0);

    for (u32 w = 0; w < wordCount; ++w) {
        u64 dirty = m_dirtyWords[w] & aliveWords[w];
        if (dirty == 0) {
            continue;
        }
        if (!hasChildren) [[likely]] {
            m_dirtyWords[w] = 0;
            m_movedWords[w] |= dirty;
        }
        do {
            const u32 bit = static_cast<u32>(std::countr_zero(dirty));
            dirty &= (dirty - 1u);
            const u16 idx = static_cast<u16>((w << 6) + bit);
            if (hasChildren && m_tree[idx].parent != 0) {
                continue;
            }
            ComputeRootTransform(m_local[idx], m_world[idx]);
            if (hasChildren) {
                const u64 b = 1ULL << bit;
                m_movedWords[w] |= b;
                if (m_tree[idx].firstChild == 0) {
                    m_dirtyWords[w] &= ~b;
                }
            }
        } while (dirty != 0);
    }

    if (!hasChildren) [[likely]] {
        return;
    }

    for (u16 i = 0; i < m_childCount; ++i) {
        const auto [childIdx, parentIdx] = m_hierarchyOrder[i];
        const u32 cw = childIdx >> 6;
        const u64 cb = 1ULL << (childIdx & 63u);
        const bool parentDirty = (m_dirtyWords[parentIdx >> 6] & (1ULL << (parentIdx & 63u))) != 0;
        const bool childDirty  = (m_dirtyWords[cw] & cb) != 0;

        if (!parentDirty && !childDirty) {
            continue;
        }

        const WorldTransform2D& p = m_world[parentIdx];
        const LocalTransform2D& l = m_local[childIdx];
        WorldTransform2D& c       = m_world[childIdx];

        const f32 ox = l.x * p.scaleX;
        const f32 oy = l.y * p.scaleY;
        c.x           = p.x + (ox * p.cosRot - oy * p.sinRot);
        c.y           = p.y + (ox * p.sinRot + oy * p.cosRot);
        c.scaleX      = p.scaleX * l.scaleX;
        c.scaleY      = p.scaleY * l.scaleY;
        c.worldRotRad = NormalizeAngleRad(p.worldRotRad + l.rotationRad);
        c.worldDepth  = p.worldDepth + l.depthOffset;
        FastSinCos(c.worldRotRad, c.cosRot, c.sinRot);

        m_movedWords[cw] |= cb;
        if (m_tree[childIdx].firstChild != 0) {
            m_dirtyWords[cw] |= cb;
        } else {
            m_dirtyWords[cw] &= ~cb;
        }
    }

    std::memset(m_dirtyWords, 0, wordCount * sizeof(u64));
}

} // namespace en::world
