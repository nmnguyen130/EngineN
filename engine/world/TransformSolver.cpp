#include "world/TransformSolver.h"

namespace en {

bool TransformSolver::Init(AlignedArena& arena, u16 maxEntities) noexcept {
    if (maxEntities == 0U) return false;
    const usize wordCount = (static_cast<usize>(maxEntities) + 63U) >> 6U;

    auto localSpan = arena.Carve<LocalTransform2D>(maxEntities);
    auto worldSpan = arena.Carve<WorldTransform2D>(maxEntities);
    auto prevSpan = arena.Carve<PrevWorldSnapshot2D>(maxEntities);
    auto parentSpan = arena.Carve<u16>(maxEntities);
    auto childSpan = arena.Carve<u16>(maxEntities);
    auto siblingSpan = arena.Carve<u16>(maxEntities);
    auto dirtySpan = arena.Carve<u64>(wordCount);
    auto movedSpan = arena.Carve<u64>(wordCount);

    if (localSpan.empty() || worldSpan.empty() || prevSpan.empty() ||
        parentSpan.empty() || childSpan.empty() || siblingSpan.empty() ||
        dirtySpan.empty() || movedSpan.empty()) {
        return false;
    }

    m_local = localSpan.data();
    m_world = worldSpan.data();
    m_prev = prevSpan.data();
    m_parent = parentSpan.data();
    m_firstChild = childSpan.data();
    m_nextSibling = siblingSpan.data();
    m_dirtyWords = dirtySpan.data();
    m_movedWords = movedSpan.data();

    for (u16 i = 0U; i < maxEntities; ++i) {
        m_parent[i] = kInvalidDenseIndex;
        m_firstChild[i] = kInvalidDenseIndex;
        m_nextSibling[i] = kInvalidDenseIndex;
    }
    return true;
}

void TransformSolver::Clear(u16 maxAllocatedIndex) noexcept {
    if (!m_parent) return;
    const usize limit = static_cast<usize>(maxAllocatedIndex) + 1U;
    for (usize i = 0U; i < limit; ++i) {
        m_parent[i] = kInvalidDenseIndex;
        m_firstChild[i] = kInvalidDenseIndex;
        m_nextSibling[i] = kInvalidDenseIndex;
    }
    const usize wordCount = (limit + 63U) >> 6U;
    for (usize w = 0U; w < wordCount; ++w) {
        m_dirtyWords[w] = 0ULL;
        m_movedWords[w] = 0ULL;
    }
}

void TransformSolver::ComputeWorldFromLocal(const LocalTransform2D& loc, WorldTransform2D& outW) noexcept {
    outW.x = loc.x;
    outW.y = loc.y;
    outW.scaleX = loc.scaleX;
    outW.scaleY = loc.scaleY;
    outW.worldRotRad = loc.rotationRad;
    outW.worldDepth = loc.depthOffset;
    if (loc.rotationRad == 0.0f) {
        outW.cosRot = 1.0f;
        outW.sinRot = 0.0f;
    } else {
        FastSinCos(loc.rotationRad, outW.cosRot, outW.sinRot);
    }
}

void TransformSolver::CombineParentChild(
    const WorldTransform2D& p,
    const LocalTransform2D& c,
    WorldTransform2D& outW) noexcept {
    const f32 sx = c.x * p.scaleX;
    const f32 sy = c.y * p.scaleY;
    outW.x = p.x + (sx * p.cosRot - sy * p.sinRot);
    outW.y = p.y + (sx * p.sinRot + sy * p.cosRot);
    outW.scaleX = p.scaleX * c.scaleX;
    outW.scaleY = p.scaleY * c.scaleY;
    outW.worldRotRad = p.worldRotRad + c.rotationRad;
    outW.worldDepth = p.worldDepth + c.depthOffset;
    if (outW.worldRotRad == 0.0f) {
        outW.cosRot = 1.0f;
        outW.sinRot = 0.0f;
    } else {
        FastSinCos(outW.worldRotRad, outW.cosRot, outW.sinRot);
    }
}

void TransformSolver::InverseCombineParentChild(
    const WorldTransform2D& p,
    const WorldTransform2D& childWorld,
    LocalTransform2D& outLocal) noexcept {
    const f32 dx = childWorld.x - p.x;
    const f32 dy = childWorld.y - p.y;
    const f32 rx = dx * p.cosRot + dy * p.sinRot;
    const f32 ry = -dx * p.sinRot + dy * p.cosRot;
    outLocal.x = (p.scaleX != 0.0f) ? (rx / p.scaleX) : rx;
    outLocal.y = (p.scaleY != 0.0f) ? (ry / p.scaleY) : ry;
    outLocal.scaleX = (p.scaleX != 0.0f) ? (childWorld.scaleX / p.scaleX) : childWorld.scaleX;
    outLocal.scaleY = (p.scaleY != 0.0f) ? (childWorld.scaleY / p.scaleY) : childWorld.scaleY;
    outLocal.rotationRad = childWorld.worldRotRad - p.worldRotRad;
    outLocal.depthOffset = childWorld.worldDepth - p.worldDepth;
}

void TransformSolver::DetachFromParentList(u16 childIdx) noexcept {
    const u16 p = m_parent[childIdx];
    if (p == kInvalidDenseIndex) return;
    u16 cur = m_firstChild[p];
    u16 prev = kInvalidDenseIndex;
    while (cur != kInvalidDenseIndex) {
        if (cur == childIdx) {
            if (prev == kInvalidDenseIndex) {
                m_firstChild[p] = m_nextSibling[cur];
            } else {
                m_nextSibling[prev] = m_nextSibling[cur];
            }
            break;
        }
        prev = cur;
        cur = m_nextSibling[cur];
    }
    m_parent[childIdx] = kInvalidDenseIndex;
    m_nextSibling[childIdx] = kInvalidDenseIndex;
}

void TransformSolver::ResolveAncestorsIfDirty(u16 idx, u16 maxEntities) noexcept {
    u16 topDirty = kInvalidDenseIndex;
    u16 anc = m_parent[idx];
    while (anc != kInvalidDenseIndex) {
        if (IsDirty(anc)) topDirty = anc;
        anc = m_parent[anc];
    }
    if (topDirty != kInvalidDenseIndex) {
        EvaluateSubtree(topDirty, maxEntities, false);
    }
}

void TransformSolver::EvaluateSubtree(u16 rootIdx, u16 maxEntities, bool snapPrev) noexcept {
    TraverseSubtree(rootIdx, maxEntities, [&](u16 cur) noexcept {
        const u64 bit = (1ULL << (cur & 63U));
        if (snapPrev || (m_dirtyWords[cur >> 6U] & bit) != 0ULL) {
            const u16 p = m_parent[cur];
            if (p == kInvalidDenseIndex) {
                ComputeWorldFromLocal(m_local[cur], m_world[cur]);
            } else {
                CombineParentChild(m_world[p], m_local[cur], m_world[cur]);
            }
            m_dirtyWords[cur >> 6U] &= ~bit;
            if (snapPrev) {
                m_prev[cur] = m_world[cur].ToSnapshot();
                m_movedWords[cur >> 6U] &= ~bit;
            } else {
                m_movedWords[cur >> 6U] |= bit;
            }
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
        m_nextSibling[idx] = m_firstChild[parentIdx];
        m_firstChild[parentIdx] = idx;
        if (keepWorldTransform) {
            WorldTransform2D desiredWorld{};
            ComputeWorldFromLocal(local, desiredWorld);
            InverseCombineParentChild(m_world[parentIdx], desiredWorld, m_local[idx]);
            m_world[idx] = desiredWorld;
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
    TraverseSubtree(rootIdx, maxEntities, [&](u16 cur) noexcept {
        MarkDirty(cur);
    });
}

void TransformSolver::TeleportSubtree(u16 rootIdx, u16 maxEntities) noexcept {
    if (rootIdx == 0U || rootIdx >= maxEntities) return;
    ResolveAncestorsIfDirty(rootIdx, maxEntities);
    EvaluateSubtree(rootIdx, maxEntities, true);
}

void TransformSolver::UnlinkSlot(u16 idx, u16 maxEntities) noexcept {
    ResolveAncestorsIfDirty(idx, maxEntities);
    if (IsDirty(idx)) {
        EvaluateSubtree(idx, maxEntities, false);
    }
    DetachFromParentList(idx);
    u16 child = m_firstChild[idx];
    while (child != kInvalidDenseIndex) {
        const u16 next = m_nextSibling[child];
        // Bake current world transform into child's local transform so unparented survivors never snap to origin
        const WorldTransform2D& cw = m_world[child];
        m_local[child] = LocalTransform2D{
            cw.x, cw.y, cw.scaleX, cw.scaleY, cw.worldRotRad, cw.worldDepth};
        m_parent[child] = kInvalidDenseIndex;
        m_nextSibling[child] = kInvalidDenseIndex;
        child = next;
    }
    m_firstChild[idx] = kInvalidDenseIndex;
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
        if (curDepth > maxDepth) maxDepth = curDepth;
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
        u16 anc = parentIdx;
        usize ancestorDepth = 1U;
        while (anc != kInvalidDenseIndex) {
            if (anc == childIdx) return Err(WorldErrc::HierarchyCycle);
            anc = m_parent[anc];
            if (++ancestorDepth > kMaxHierarchyDepth) {
                return Err(WorldErrc::HierarchyDepthExceeded);
            }
        }
        const u16 childSubtreeDepth = MaxSubtreeDepth(childIdx, maxEntities);
        if (ancestorDepth + childSubtreeDepth > kMaxHierarchyDepth) {
            return Err(WorldErrc::HierarchyDepthExceeded);
        }
    }

    if (m_parent[childIdx] == parentIdx) return {};

    if (keepWorldTransform) {
        ResolveAncestorsIfDirty(childIdx, maxEntities);
        if (IsDirty(childIdx)) {
            EvaluateSubtree(childIdx, maxEntities, false);
        }
        if (parentIdx != kInvalidDenseIndex) {
            ResolveAncestorsIfDirty(parentIdx, maxEntities);
            if (IsDirty(parentIdx)) {
                EvaluateSubtree(parentIdx, maxEntities, false);
            }
        }
    }

    DetachFromParentList(childIdx);
    m_parent[childIdx] = parentIdx;
    if (parentIdx != kInvalidDenseIndex) {
        m_nextSibling[childIdx] = m_firstChild[parentIdx];
        m_firstChild[parentIdx] = childIdx;
        if (keepWorldTransform) {
            InverseCombineParentChild(m_world[parentIdx], m_world[childIdx], m_local[childIdx]);
        }
    } else {
        m_nextSibling[childIdx] = kInvalidDenseIndex;
        if (keepWorldTransform) {
            const WorldTransform2D& cw = m_world[childIdx];
            m_local[childIdx] = LocalTransform2D{
                cw.x, cw.y, cw.scaleX, cw.scaleY, cw.worldRotRad, cw.worldDepth};
        }
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
    if (frozenMask == 0U) {
        for (usize w = 0U; w < wordCount; ++w) {
            u64 moved = m_movedWords[w] & aliveWords[w];
            if (moved == 0ULL) continue;
            while (moved != 0ULL) {
                const u16 bit = static_cast<u16>(std::countr_zero(moved));
                moved &= (moved - 1ULL);
                const u16 idx = static_cast<u16>((w << 6U) + bit);
                m_prev[idx] = m_world[idx].ToSnapshot();
            }
            m_movedWords[w] = 0ULL;
        }
        return;
    }

    for (usize w = 0U; w < wordCount; ++w) {
        u64 moved = m_movedWords[w] & aliveWords[w];
        if (moved == 0ULL) continue;
        u64 stillFrozenMoved = 0ULL;
        while (moved != 0ULL) {
            const u16 bit = static_cast<u16>(std::countr_zero(moved));
            moved &= (moved - 1ULL);
            const u16 idx = static_cast<u16>((w << 6U) + bit);
            const u8 ch = timeChannels[idx] & 7U;
            if ((frozenMask & (1U << ch)) != 0U) {
                stillFrozenMoved |= (1ULL << bit);
                continue;
            }
            m_prev[idx] = m_world[idx].ToSnapshot();
        }
        m_movedWords[w] = stillFrozenMoved;
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
            const u16 bit = static_cast<u16>(std::countr_zero(candidates));
            u16 top = static_cast<u16>((w << 6U) + bit);
            while (m_parent[top] != kInvalidDenseIndex && IsDirty(m_parent[top])) {
                top = m_parent[top];
            }
            EvaluateSubtree(top, maxEnt, false);
            candidates &= m_dirtyWords[w];
        }
    }
}

} // namespace en
