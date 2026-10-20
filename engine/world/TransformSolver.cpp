#include "world/TransformSolver.h"

namespace en {

bool TransformSolver::Init(AlignedArena& arena, u16 maxEntities) noexcept {
    const usize wordCount = (static_cast<usize>(maxEntities) + 63U) / 64U;
    m_local = arena.Carve<LocalTransform2D>(maxEntities);
    m_world = arena.Carve<WorldTransform2D>(maxEntities);
    m_prev = arena.Carve<PrevWorldSnapshot2D>(maxEntities);
    m_parent = arena.Carve<u16>(maxEntities);
    m_firstChild = arena.Carve<u16>(maxEntities);
    m_nextSibling = arena.Carve<u16>(maxEntities);
    m_dirtyWords = arena.Carve<u64>(wordCount);
    m_movedWords = arena.Carve<u64>(wordCount);

    if (m_local.empty() || m_world.empty() || m_prev.empty() ||
        m_parent.empty() || m_firstChild.empty() || m_nextSibling.empty() ||
        m_dirtyWords.empty() || m_movedWords.empty()) {
        return false;
    }
    for (u16 i = 0; i < maxEntities; ++i) {
        m_parent[i] = kInvalidDenseIndex;
        m_firstChild[i] = kInvalidDenseIndex;
        m_nextSibling[i] = kInvalidDenseIndex;
    }
    return true;
}

void TransformSolver::Clear(u16 maxAllocatedIndex) noexcept {
    const usize limit = std::min<usize>(maxAllocatedIndex + 1U, m_parent.size());
    for (usize i = 0; i < limit; ++i) {
        m_parent[i] = kInvalidDenseIndex;
        m_firstChild[i] = kInvalidDenseIndex;
        m_nextSibling[i] = kInvalidDenseIndex;
    }
    for (u64& w : m_dirtyWords) w = 0;
    for (u64& w : m_movedWords) w = 0;
}

void TransformSolver::ComputeWorldFromLocal(const LocalTransform2D& loc, WorldTransform2D& outW) noexcept {
    outW.x = loc.x;
    outW.y = loc.y;
    outW.scaleX = loc.scaleX;
    outW.scaleY = loc.scaleY;
    outW.worldRotRad = loc.rotationRad;
    outW.worldDepth = loc.depthOffset;
    FastSinCos(loc.rotationRad, outW.cosRot, outW.sinRot);
}

void TransformSolver::CombineParentChild(
    const WorldTransform2D& p, const LocalTransform2D& c, WorldTransform2D& outW) noexcept {
    const f32 sx = c.x * p.scaleX;
    const f32 sy = c.y * p.scaleY;
    outW.x = p.x + (sx * p.cosRot - sy * p.sinRot);
    outW.y = p.y + (sx * p.sinRot + sy * p.cosRot);
    outW.scaleX = p.scaleX * c.scaleX;
    outW.scaleY = p.scaleY * c.scaleY;
    outW.worldRotRad = p.worldRotRad + c.rotationRad;
    outW.worldDepth = p.worldDepth + c.depthOffset;
    FastSinCos(outW.worldRotRad, outW.cosRot, outW.sinRot);
}

void TransformSolver::InitializeSlot(u16 idx, const LocalTransform2D& local) noexcept {
    m_local[idx] = local;
    m_parent[idx] = kInvalidDenseIndex;
    m_firstChild[idx] = kInvalidDenseIndex;
    m_nextSibling[idx] = kInvalidDenseIndex;
    ComputeWorldFromLocal(local, m_world[idx]);
    m_prev[idx] = m_world[idx].ToSnapshot();
    m_dirtyWords[idx >> 6U] &= ~(1ULL << (idx & 63U));
    m_movedWords[idx >> 6U] &= ~(1ULL << (idx & 63U));
}

void TransformSolver::MarkSubtreeDirty(u16 rootIdx) noexcept {
    if (rootIdx == 0 || rootIdx >= m_local.size()) return;
    MarkDirty(rootIdx);
    u16 stack[64]{};
    u32 sp = 0;
    if (m_firstChild[rootIdx] != kInvalidDenseIndex) {
        stack[sp++] = m_firstChild[rootIdx];
    }
    while (sp > 0) {
        const u16 cur = stack[--sp];
        MarkDirty(cur);
        if (m_nextSibling[cur] != kInvalidDenseIndex && sp < 64) {
            stack[sp++] = m_nextSibling[cur];
        }
        if (m_firstChild[cur] != kInvalidDenseIndex && sp < 64) {
            stack[sp++] = m_firstChild[cur];
        }
    }
}

void TransformSolver::UnlinkSlot(u16 idx) noexcept {
    const u16 p = m_parent[idx];
    if (p != kInvalidDenseIndex) {
        u16 cur = m_firstChild[p];
        u16 prev = kInvalidDenseIndex;
        while (cur != kInvalidDenseIndex) {
            if (cur == idx) {
                if (prev == kInvalidDenseIndex) m_firstChild[p] = m_nextSibling[cur];
                else m_nextSibling[prev] = m_nextSibling[cur];
                break;
            }
            prev = cur;
            cur = m_nextSibling[cur];
        }
        m_parent[idx] = kInvalidDenseIndex;
        m_nextSibling[idx] = kInvalidDenseIndex;
    }
    u16 child = m_firstChild[idx];
    while (child != kInvalidDenseIndex) {
        const u16 next = m_nextSibling[child];
        m_parent[child] = kInvalidDenseIndex;
        m_nextSibling[child] = kInvalidDenseIndex;
        MarkDirty(child);
        child = next;
    }
    m_firstChild[idx] = kInvalidDenseIndex;
    m_dirtyWords[idx >> 6U] &= ~(1ULL << (idx & 63U));
    m_movedWords[idx >> 6U] &= ~(1ULL << (idx & 63U));
}

Result<void, WorldErrc> TransformSolver::SetParent(u16 childIdx, u16 parentIdx) noexcept {
    if (childIdx == parentIdx) return Err(WorldErrc::HierarchyCycle);
    if (parentIdx != kInvalidDenseIndex) {
        u16 anc = parentIdx;
        usize depth = 1;
        while (anc != kInvalidDenseIndex) {
            if (anc == childIdx) return Err(WorldErrc::HierarchyCycle);
            anc = m_parent[anc];
            if (++depth > kMaxHierarchyDepth) return Err(WorldErrc::HierarchyDepthExceeded);
        }
    }
    const u16 oldP = m_parent[childIdx];
    if (oldP == parentIdx) return {};
    if (oldP != kInvalidDenseIndex) {
        u16 cur = m_firstChild[oldP];
        u16 prev = kInvalidDenseIndex;
        while (cur != kInvalidDenseIndex) {
            if (cur == childIdx) {
                if (prev == kInvalidDenseIndex) m_firstChild[oldP] = m_nextSibling[cur];
                else m_nextSibling[prev] = m_nextSibling[cur];
                break;
            }
            prev = cur;
            cur = m_nextSibling[cur];
        }
    }
    m_parent[childIdx] = parentIdx;
    if (parentIdx != kInvalidDenseIndex) {
        m_nextSibling[childIdx] = m_firstChild[parentIdx];
        m_firstChild[parentIdx] = childIdx;
    } else {
        m_nextSibling[childIdx] = kInvalidDenseIndex;
    }
    MarkDirty(childIdx);
    return {};
}

void TransformSolver::SnapshotPreviousTransforms(
    std::span<const u64> aliveWords,
    std::span<const u8> timeChannels,
    u8 frozenMask,
    u16 maxAllocatedIndex) noexcept {
    const usize wordCount = std::min(m_movedWords.size(), (static_cast<usize>(maxAllocatedIndex) + 64U) / 64U);
    for (usize w = 0; w < wordCount; ++w) {
        u64 moved = m_movedWords[w] & aliveWords[w];
        if (moved == 0) continue;
        u64 stillFrozenMoved = 0;
        while (moved != 0) {
            const u16 bit = static_cast<u16>(std::countr_zero(moved));
            moved &= (moved - 1ULL);
            const u16 idx = static_cast<u16>((w << 6U) + bit);
            const u8 ch = timeChannels[idx] & 7U;
            if ((frozenMask & (1U << ch)) != 0) {
                stillFrozenMoved |= (1ULL << bit);
                continue;
            }
            m_prev[idx] = m_world[idx].ToSnapshot();
        }
        m_movedWords[w] = stillFrozenMoved;
    }
}

void TransformSolver::PropagateHierarchy(std::span<const u64> aliveWords, u16 maxAllocatedIndex) noexcept {
    const usize wordCount = std::min(m_dirtyWords.size(), (static_cast<usize>(maxAllocatedIndex) + 64U) / 64U);
    struct StackEntry {
        u16 idx;
        bool parentDirty;
    };
    std::array<StackEntry, kMaxHierarchyDepth * 4> stack{};

    for (usize w = 0; w < wordCount; ++w) {
        u64 candidates = m_dirtyWords[w] & aliveWords[w];
        while (candidates != 0) {
            const u16 bit = static_cast<u16>(std::countr_zero(candidates));
            candidates &= (candidates - 1ULL);
            u16 root = static_cast<u16>((w << 6U) + bit);
            while (m_parent[root] != kInvalidDenseIndex) {
                root = m_parent[root];
            }

            usize sp = 0;
            stack[sp++] = StackEntry{root, false};
            while (sp > 0) {
                const StackEntry cur = stack[--sp];
                const u16 u = cur.idx;
                const u64 uBit = (1ULL << (u & 63U));
                const bool selfDirty = (m_dirtyWords[u >> 6U] & uBit) != 0;
                const bool needUpdate = cur.parentDirty || selfDirty;
                if (needUpdate) {
                    const u16 p = m_parent[u];
                    if (p == kInvalidDenseIndex) {
                        ComputeWorldFromLocal(m_local[u], m_world[u]);
                    } else {
                        CombineParentChild(m_world[p], m_local[u], m_world[u]);
                    }
                    m_dirtyWords[u >> 6U] &= ~uBit;
                    m_movedWords[u >> 6U] |= uBit;
                }
                u16 child = m_firstChild[u];
                while (child != kInvalidDenseIndex && sp < stack.size()) {
                    stack[sp++] = StackEntry{child, needUpdate};
                    child = m_nextSibling[child];
                }
            }
            candidates &= m_dirtyWords[w];
        }
    }
}

} // namespace en
