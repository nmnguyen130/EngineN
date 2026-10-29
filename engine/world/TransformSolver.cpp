#include "world/TransformSolver.h"

namespace en {
namespace {

constexpr void UpdateTrig(WorldTransform2D& w) noexcept {
    if (w.worldRotRad == 0.0f) {
        w.cosRot = 1.0f;
        w.sinRot = 0.0f;
    } else {
        FastSinCos(w.worldRotRad, w.cosRot, w.sinRot);
    }
}

} // namespace

bool TransformSolver::Init(Arena& arena, u16 maxEntities) noexcept {
    if (maxEntities == 0U) return false;
    const u32 mark = arena.Mark();
    const usize count = maxEntities;
    const usize wordCount = (count + 63U) >> 6U;

    auto worldSpan = arena.Alloc<WorldTransform2D>(count);
    auto bitSpan   = arena.Alloc<u64>(wordCount * 2U);
    auto localSpan = arena.Alloc<LocalTransform2D>(count);
    auto prevSpan  = arena.Alloc<PrevWorldSnapshot2D>(count);
    auto linkSpan  = arena.Alloc<u16>(count * 3U);

    if (worldSpan.empty() || bitSpan.empty() || localSpan.empty() ||
        prevSpan.empty() || linkSpan.empty()) {
        arena.Rollback(mark);
        return false;
    }

    m_world       = worldSpan.data();
    m_dirtyWords  = bitSpan.data();
    m_movedWords  = m_dirtyWords + wordCount;
    m_local       = localSpan.data();
    m_prev        = prevSpan.data();
    m_parent      = linkSpan.data();
    m_firstChild  = m_parent + count;
    m_nextSibling = m_firstChild + count;

    std::fill_n(m_parent, count * 3U, kInvalidDenseIndex);
    return true;
}

void TransformSolver::Clear(u16 maxAllocatedIndex) noexcept {
    if (!m_parent) return;
    const usize limit = static_cast<usize>(maxAllocatedIndex) + 1U;
    std::fill_n(m_parent, limit, kInvalidDenseIndex);
    std::fill_n(m_firstChild, limit, kInvalidDenseIndex);
    std::fill_n(m_nextSibling, limit, kInvalidDenseIndex);

    const usize wordCount = (limit + 63U) >> 6U;
    std::fill_n(m_dirtyWords, wordCount, 0ULL);
    std::fill_n(m_movedWords, wordCount, 0ULL);
}

void TransformSolver::ComputeWorldFromLocal(
    const LocalTransform2D& loc,
    WorldTransform2D& outW) noexcept {
    outW.x           = loc.x;
    outW.y           = loc.y;
    outW.scaleX      = loc.scaleX;
    outW.scaleY      = loc.scaleY;
    outW.worldRotRad = loc.rotationRad;
    outW.worldDepth  = loc.depthOffset;
    UpdateTrig(outW);
}

void TransformSolver::CombineParentChild(
    const WorldTransform2D& p,
    const LocalTransform2D& c,
    WorldTransform2D& outW) noexcept {
    const f32 sx = c.x * p.scaleX;
    const f32 sy = c.y * p.scaleY;
    outW.x      = p.x + (sx * p.cosRot - sy * p.sinRot);
    outW.y      = p.y + (sx * p.sinRot + sy * p.cosRot);
    outW.scaleX = p.scaleX * c.scaleX;
    outW.scaleY = p.scaleY * c.scaleY;
    const f32 reflectionSign = (p.scaleX * p.scaleY < 0.0f) ? -1.0f : 1.0f;
    outW.worldRotRad = p.worldRotRad + c.rotationRad * reflectionSign;
    outW.worldDepth  = p.worldDepth + c.depthOffset;
    UpdateTrig(outW);
}

void TransformSolver::InverseCombineParentChild(
    const WorldTransform2D& p,
    const WorldTransform2D& childWorld,
    LocalTransform2D& outLocal) noexcept {
    const f32 dx = childWorld.x - p.x;
    const f32 dy = childWorld.y - p.y;
    const f32 rx = dx * p.cosRot + dy * p.sinRot;
    const f32 ry = -dx * p.sinRot + dy * p.cosRot;
    outLocal.x      = (p.scaleX != 0.0f) ? (rx / p.scaleX) : rx;
    outLocal.y      = (p.scaleY != 0.0f) ? (ry / p.scaleY) : ry;
    outLocal.scaleX = (p.scaleX != 0.0f) ? (childWorld.scaleX / p.scaleX) : childWorld.scaleX;
    outLocal.scaleY = (p.scaleY != 0.0f) ? (childWorld.scaleY / p.scaleY) : childWorld.scaleY;
    const f32 reflectionSign = (p.scaleX * p.scaleY < 0.0f) ? -1.0f : 1.0f;
    outLocal.rotationRad = (childWorld.worldRotRad - p.worldRotRad) * reflectionSign;
    outLocal.depthOffset = childWorld.worldDepth - p.worldDepth;
}

void TransformSolver::DetachFromParentList(u16 childIdx) noexcept {
    const u16 p = m_parent[childIdx];
    if (p == kInvalidDenseIndex) return;
    for (u16* link = &m_firstChild[p]; *link != kInvalidDenseIndex; link = &m_nextSibling[*link]) {
        if (*link == childIdx) {
            *link = m_nextSibling[childIdx];
            break;
        }
    }
    m_parent[childIdx] = kInvalidDenseIndex;
    m_nextSibling[childIdx] = kInvalidDenseIndex;
}

void TransformSolver::ResolveDirtyChain(u16 idx, u16 maxEntities) noexcept {
    if (idx == 0U || idx >= maxEntities) return;
    u16 topDirty = kInvalidDenseIndex;
    for (u16 anc = idx; anc != kInvalidDenseIndex; anc = m_parent[anc]) {
        if (IsDirty(anc)) topDirty = anc;
    }
    if (topDirty != kInvalidDenseIndex) {
        EvaluateDirtySubtree(topDirty, maxEntities);
    }
}

void TransformSolver::EvaluateDirtySubtree(u16 rootIdx, u16 maxEntities) noexcept {
    TraverseSubtree(rootIdx, maxEntities, [&](u16 cur) noexcept {
        const u64 bit = (1ULL << (cur & 63U));
        if ((m_dirtyWords[cur >> 6U] & bit) != 0ULL) {
            UpdateNodeWorld(cur);
            m_dirtyWords[cur >> 6U] &= ~bit;
            m_movedWords[cur >> 6U] |= bit;
        }
    });
}

void TransformSolver::InitializeSlot(
    u16 idx,
    const LocalTransform2D& local,
    u16 parentIdx,
    bool keepWorldTransform) noexcept {
    m_parent[idx] = parentIdx;
    m_firstChild[idx] = kInvalidDenseIndex;
    if (parentIdx != kInvalidDenseIndex) {
        ResolveDirtyChain(parentIdx);
        m_nextSibling[idx] = m_firstChild[parentIdx];
        m_firstChild[parentIdx] = idx;
        if (keepWorldTransform) {
            ComputeWorldFromLocal(local, m_world[idx]);
            InverseCombineParentChild(m_world[parentIdx], m_world[idx], m_local[idx]);
        } else {
            m_local[idx] = local;
            CombineParentChild(m_world[parentIdx], local, m_world[idx]);
        }
    } else {
        m_nextSibling[idx] = kInvalidDenseIndex;
        m_local[idx] = local;
        ComputeWorldFromLocal(local, m_world[idx]);
    }
    m_prev[idx] = m_world[idx].ToSnapshot();
    const u64 bit = (1ULL << (idx & 63U));
    m_dirtyWords[idx >> 6U] &= ~bit;
    m_movedWords[idx >> 6U] &= ~bit;
}

void TransformSolver::MarkSubtreeDirty(u16 rootIdx, u16 maxEntities) noexcept {
    TraverseSubtree(rootIdx, maxEntities, [&](u16 cur) noexcept { MarkDirty(cur); });
}

void TransformSolver::TeleportSubtree(u16 rootIdx, u16 maxEntities) noexcept {
    if (rootIdx == 0U || rootIdx >= maxEntities) return;
    ResolveDirtyChain(m_parent[rootIdx], maxEntities);
    TraverseSubtree(rootIdx, maxEntities, [&](u16 cur) noexcept {
        UpdateNodeWorld(cur);
        m_prev[cur] = m_world[cur].ToSnapshot();
        const u64 bit = (1ULL << (cur & 63U));
        m_dirtyWords[cur >> 6U] &= ~bit;
        m_movedWords[cur >> 6U] &= ~bit;
    });
}

void TransformSolver::UnlinkSlot(u16 idx, u16 maxEntities) noexcept {
    if (m_firstChild[idx] != kInvalidDenseIndex) {
        ResolveDirtyChain(idx, maxEntities);
        EvaluateDirtySubtree(idx, maxEntities);
        for (u16 child = m_firstChild[idx]; child != kInvalidDenseIndex;) {
            const u16 next = m_nextSibling[child];
            m_local[child] = m_world[child].ToLocal();
            m_parent[child] = kInvalidDenseIndex;
            m_nextSibling[child] = kInvalidDenseIndex;
            child = next;
        }
        m_firstChild[idx] = kInvalidDenseIndex;
    }
    DetachFromParentList(idx);
    const u64 bit = (1ULL << (idx & 63U));
    m_dirtyWords[idx >> 6U] &= ~bit;
    m_movedWords[idx >> 6U] &= ~bit;
}

u16 TransformSolver::MaxSubtreeDepth(u16 rootIdx, u16 maxEntities) const noexcept {
    if (rootIdx == 0U || rootIdx >= maxEntities) return 0U;
    u16 maxDepth = 0U;
    u16 curDepth = 0U;
    u16 cur = rootIdx;
    for (;;) {
        maxDepth = std::max(maxDepth, curDepth);
        const u16 child = m_firstChild[cur];
        if (child != kInvalidDenseIndex) {
            cur = child;
            ++curDepth;
            continue;
        }
        while (cur != rootIdx && m_nextSibling[cur] == kInvalidDenseIndex) {
            cur = m_parent[cur];
            --curDepth;
        }
        if (cur == rootIdx) break;
        cur = m_nextSibling[cur];
    }
    return maxDepth;
}

Result<void, WorldErrc> TransformSolver::SetParent(
    u16 childIdx,
    u16 parentIdx,
    u16 maxEntities,
    bool keepWorldTransform) noexcept {
    if (childIdx == parentIdx) return Err(WorldErrc::HierarchyCycle);
    if (parentIdx != kInvalidDenseIndex) {
        usize ancestorDepth = 1U;
        for (u16 anc = parentIdx; anc != kInvalidDenseIndex; anc = m_parent[anc]) {
            if (anc == childIdx) return Err(WorldErrc::HierarchyCycle);
            if (++ancestorDepth > kMaxHierarchyDepth) return Err(WorldErrc::HierarchyDepthExceeded);
        }
        if (ancestorDepth + MaxSubtreeDepth(childIdx, maxEntities) > kMaxHierarchyDepth) {
            return Err(WorldErrc::HierarchyDepthExceeded);
        }
    }

    if (m_parent[childIdx] == parentIdx) return {};

    if (keepWorldTransform) {
        ResolveDirtyChain(childIdx, maxEntities);
        ResolveDirtyChain(parentIdx, maxEntities);
    }

    DetachFromParentList(childIdx);
    m_parent[childIdx] = parentIdx;
    if (parentIdx != kInvalidDenseIndex) {
        m_nextSibling[childIdx] = m_firstChild[parentIdx];
        m_firstChild[parentIdx] = childIdx;
        if (keepWorldTransform) {
            InverseCombineParentChild(m_world[parentIdx], m_world[childIdx], m_local[childIdx]);
        }
    } else if (keepWorldTransform) {
        m_local[childIdx] = m_world[childIdx].ToLocal();
    }
    MarkSubtreeDirty(childIdx, maxEntities);
    return {};
}

void TransformSolver::SnapshotPreviousTransforms(
    std::span<const u64> aliveWords,
    const u8* timeChannels,
    u8 frozenMask,
    u16 maxAllocatedIndex) noexcept {
    const usize wordCount = std::min<usize>(
        aliveWords.size(), (static_cast<usize>(maxAllocatedIndex) + 64U) >> 6U);
    for (usize w = 0U; w < wordCount; ++w) {
        u64 moved = m_movedWords[w] & aliveWords[w];
        u64 stillFrozen = 0ULL;
        while (moved != 0ULL) {
            const u16 bit = static_cast<u16>(std::countr_zero(moved));
            moved &= (moved - 1ULL);
            const u16 idx = static_cast<u16>((w << 6U) + bit);
            if (frozenMask != 0U && ((frozenMask >> (timeChannels[idx] & 7U)) & 1U) != 0U) {
                stillFrozen |= (1ULL << bit);
                continue;
            }
            m_prev[idx] = m_world[idx].ToSnapshot();
        }
        m_movedWords[w] = stillFrozen;
    }
}

void TransformSolver::PropagateHierarchy(
    std::span<const u64> aliveWords,
    u16 maxAllocatedIndex) noexcept {
    const u16 maxEnt = static_cast<u16>(maxAllocatedIndex + 1U);
    const usize wordCount = std::min<usize>(
        aliveWords.size(), (static_cast<usize>(maxAllocatedIndex) + 64U) >> 6U);

    for (usize w = 0U; w < wordCount; ++w) {
        u64 candidates = m_dirtyWords[w] & aliveWords[w];
        while (candidates != 0ULL) {
            u16 top = static_cast<u16>((w << 6U) + std::countr_zero(candidates));
            while (m_parent[top] != kInvalidDenseIndex && IsDirty(m_parent[top])) {
                top = m_parent[top];
            }
            EvaluateDirtySubtree(top, maxEnt);
            candidates &= m_dirtyWords[w];
        }
    }
}

} // namespace en
