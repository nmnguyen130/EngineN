#include "world/TransformSolver.h"

#include <algorithm>
#include <cstring>

namespace en::world {

void TransformSolver::BindArena(
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
    std::span<u64> subtreeDirtyWords) noexcept {
    m_local               = local;
    m_world               = world;
    m_prev                = prev;
    m_tree                = treeNodes;
    m_depths              = depths;
    m_hierarchyOrder      = hierarchyOrder;
    m_spatialAliveWords   = spatialAliveWords;
    m_spatialDynamicWords = spatialDynamicWords;
    m_rootDirtyWords      = rootDirtyWords;
    m_childDirtyWords     = childDirtyWords;
    m_subtreeDirtyWords   = subtreeDirtyWords;
    Clear();
}

void TransformSolver::Clear() noexcept {
    std::fill(m_tree.begin(), m_tree.end(), HierarchyNode{});
    std::fill(m_depths.begin(), m_depths.end(), u8{0});
    std::fill(m_spatialAliveWords.begin(), m_spatialAliveWords.end(), 0ULL);
    std::fill(m_spatialDynamicWords.begin(), m_spatialDynamicWords.end(), 0ULL);
    std::fill(m_rootDirtyWords.begin(), m_rootDirtyWords.end(), 0ULL);
    std::fill(m_childDirtyWords.begin(), m_childDirtyWords.end(), 0ULL);
    std::fill(m_subtreeDirtyWords.begin(), m_subtreeDirtyWords.end(), 0ULL);

    if (!m_local.empty()) {
        m_local[0] = LocalTransform2D{};
        m_world[0] = WorldTransform2D{};
        m_prev[0]  = PrevWorldSnapshot2D{};
    }
    m_childCount = 0;
    m_orderDirty = false;
}

void TransformSolver::InitSlot(u16 idx, const LocalTransform2D& initialLocal, bool isStatic) noexcept {
    m_local[idx]  = initialLocal;
    m_tree[idx]   = HierarchyNode{};
    m_depths[idx] = 0;

    f32 c = 1.0f;
    f32 s = 0.0f;
    FastSinCos(initialLocal.rotationRad, c, s);

    m_world[idx] = WorldTransform2D{
        .x           = initialLocal.x,
        .y           = initialLocal.y,
        .scaleX      = initialLocal.scaleX,
        .scaleY      = initialLocal.scaleY,
        .cosRot      = c,
        .sinRot      = s,
        .worldRotRad = initialLocal.rotationRad,
        .worldDepth  = initialLocal.depthOffset,
    };
    SyncPrevToCurrent(idx);

    const u32 w = idx >> 6;
    const u64 b = 1ULL << (idx & 63u);
    m_spatialAliveWords[w] |= b;
    if (!isStatic) {
        m_spatialDynamicWords[w] |= b;
    } else {
        m_spatialDynamicWords[w] &= ~b;
    }
    m_rootDirtyWords[w]  &= ~b;
    m_childDirtyWords[w] &= ~b;
}

void TransformSolver::DisableSlot(u16 idx) noexcept {
    DetachChildrenToRoot(idx);
    if (m_tree[idx].parent != 0) {
        UnlinkFromParent(idx);
        m_orderDirty = true;
    }
    const u32 w = idx >> 6;
    const u64 mask = ~(1ULL << (idx & 63u));
    m_spatialAliveWords[w]   &= mask;
    m_spatialDynamicWords[w] &= mask;
    m_rootDirtyWords[w]      &= mask;
    m_childDirtyWords[w]     &= mask;
    m_depths[idx] = 0;
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
        m_depths[child]           = 0;
        m_rootDirtyWords[child >> 6] |= (1ULL << (child & 63u));
        child = next;
    }
    m_tree[parentIdx].firstChild = 0;
    m_orderDirty = true;
}

Result<void, WorldErrc> TransformSolver::SetParent(u16 childIdx, u16 newParentIdx) noexcept {
    if (childIdx == 0 || childIdx == newParentIdx) [[unlikely]] {
        return Err(WorldErrc::HierarchyCycleDetected);
    }
    if (m_tree[childIdx].parent == newParentIdx) {
        return Ok;
    }

    u8 newDepth = 0;
    if (newParentIdx != 0) {
        u16 cursor = newParentIdx;
        u8 steps = 1;
        while (cursor != 0) {
            if (cursor == childIdx) [[unlikely]] {
                return Err(WorldErrc::HierarchyCycleDetected);
            }
            cursor = m_tree[cursor].parent;
            if (++steps > kMaxHierarchyDepth) [[unlikely]] {
                return Err(WorldErrc::HierarchyDepthExceeded);
            }
        }
        newDepth = static_cast<u8>(m_depths[newParentIdx] + 1u);
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
        m_childDirtyWords[childIdx >> 6] |= (1ULL << (childIdx & 63u));
    } else {
        m_rootDirtyWords[childIdx >> 6] |= (1ULL << (childIdx & 63u));
    }

    m_depths[childIdx] = newDepth;
    m_orderDirty = true;
    return Ok;
}

void TransformSolver::AppendSubtreeDFS(u16 parentIdx) noexcept {
    u16 child = m_tree[parentIdx].firstChild;
    while (child != 0) {
        m_depths[child] = static_cast<u8>(m_depths[parentIdx] + 1u);
        m_hierarchyOrder[m_childCount++] = HierarchyStep{
            .childIdx  = child,
            .parentIdx = parentIdx,
        };
        if (m_tree[child].firstChild != 0) {
            AppendSubtreeDFS(child);
        }
        child = m_tree[child].nextSibling;
    }
}

void TransformSolver::RebuildHierarchyOrder() noexcept {
    m_childCount = 0;
    const u32 wordCount = static_cast<u32>(m_spatialAliveWords.size());
    for (u32 w = 0; w < wordCount; ++w) {
        u64 alive = m_spatialAliveWords[w];
        while (alive != 0) {
            const u32 bit = static_cast<u32>(std::countr_zero(alive));
            alive &= (alive - 1u);
            const u16 idx = static_cast<u16>((w << 6) + bit);
            if (m_tree[idx].parent == 0 && m_tree[idx].firstChild != 0) {
                AppendSubtreeDFS(idx);
            }
        }
    }
    m_orderDirty = false;
}

void TransformSolver::SnapshotPreviousTransforms(
    u16 maxAllocatedIndex,
    u8 activeChannelMask,
    std::span<const time::TimeChannel> entityChannels) noexcept {
    if (activeChannelMask == 0) {
        return;
    }
    const u32 wordCount = (static_cast<u32>(maxAllocatedIndex) + 63u) >> 6;
    for (u32 w = 0; w < wordCount; ++w) {
        u64 mask = m_spatialAliveWords[w] & m_spatialDynamicWords[w];
        while (mask != 0) {
            const u32 bit = static_cast<u32>(std::countr_zero(mask));
            mask &= (mask - 1u);
            const u16 idx = static_cast<u16>((w << 6) + bit);
            const u8 ch = static_cast<u8>(entityChannels[idx]);
            if ((activeChannelMask & (1u << ch)) != 0) {
                SyncPrevToCurrent(idx);
            }
        }
    }
}

void TransformSolver::Propagate(u16 maxAllocatedIndex) noexcept {
    if (m_orderDirty) [[unlikely]] {
        RebuildHierarchyOrder();
    }

    const u32 wordCount = (static_cast<u32>(maxAllocatedIndex) + 63u) >> 6;
    bool anyRootUpdated = false;

    for (u32 w = 0; w < wordCount; ++w) {
        u64 dirty = m_rootDirtyWords[w] & m_spatialAliveWords[w];
        if (dirty == 0) {
            continue;
        }
        m_rootDirtyWords[w] = 0;
        m_subtreeDirtyWords[w] |= dirty;
        anyRootUpdated = true;

        while (dirty != 0) {
            const u32 bit = static_cast<u32>(std::countr_zero(dirty));
            dirty &= (dirty - 1u);
            const u16 idx = static_cast<u16>((w << 6) + bit);

            const LocalTransform2D& loc = m_local[idx];
            WorldTransform2D& wrld      = m_world[idx];
            wrld.x           = loc.x;
            wrld.y           = loc.y;
            wrld.scaleX      = loc.scaleX;
            wrld.scaleY      = loc.scaleY;
            wrld.worldRotRad = NormalizeAngleRad(loc.rotationRad);
            wrld.worldDepth  = loc.depthOffset;
            FastSinCos(wrld.worldRotRad, wrld.cosRot, wrld.sinRot);
        }
    }

    if (m_childCount == 0) {
        if (anyRootUpdated) {
            std::memset(m_subtreeDirtyWords.data(), 0, wordCount * sizeof(u64));
        }
        return;
    }

    for (u32 i = 0; i < m_childCount; ++i) {
        const auto [childIdx, parentIdx] = m_hierarchyOrder[i];
        const u32 cw = childIdx >> 6;
        const u64 cb = 1ULL << (childIdx & 63u);
        const bool parentDirty = (m_subtreeDirtyWords[parentIdx >> 6] & (1ULL << (parentIdx & 63u))) != 0;
        const bool childDirty  = (m_childDirtyWords[cw] & cb) != 0;

        if (!parentDirty && !childDirty) {
            continue;
        }
        m_childDirtyWords[cw]  &= ~cb;
        m_subtreeDirtyWords[cw] |= cb;

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
    }

    std::memset(m_subtreeDirtyWords.data(), 0, wordCount * sizeof(u64));
}

} // namespace en::world