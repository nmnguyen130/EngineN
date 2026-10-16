#pragma once

#include "world/WorldTypes.h"

#include <algorithm>
#include <span>
#include <utility>

namespace en::world {

// Non-virtual function pointer table for type-erased pool operations.
struct PoolOps {
    void* poolInstance{nullptr};
    void (*removeUncheckedFn)(void* pool, EntityId id) noexcept {nullptr};
    Result<void, WorldErrc> (*emplaceRawFn)(void* pool, EntityId id, const void* rawComponent) noexcept {nullptr};
    void (*clearFn)(void* pool) noexcept {nullptr};
    u64 (*hashCanonicalFn)(const void* pool, u16 maxEntityIndex) noexcept {nullptr};
};

// Manages contiguous dense component storage with O(1) sparse indexing and 64-bit presence bitsets.
// Non-owning over backing arena slabs carved by World::Init().
// Invariant: For all i in [0, m_size), m_sparse[m_denseEntities[i].index] == i.
template <Component T>
class ComponentPool final {
public:
    void BindArena(
        std::span<u16> sparse,
        std::span<u64> presenceWords,
        std::span<EntityId> denseEntities,
        std::span<T> denseComponents) noexcept {
        m_sparse          = sparse;
        m_presenceWords   = presenceWords;
        m_denseEntities   = denseEntities;
        m_denseComponents = denseComponents;
        m_capacity        = static_cast<u16>(denseComponents.size());
        Clear();
    }

    void Clear() noexcept {
        std::fill(m_sparse.begin(), m_sparse.end(), kInvalidDenseIndex);
        std::fill(m_presenceWords.begin(), m_presenceWords.end(), 0ULL);
        m_size       = 0;
        m_groupCount = 0;
    }

    [[nodiscard]] constexpr bool Contains(EntityId id) const noexcept {
        const u16 denseIdx = m_sparse[id.index];
        return denseIdx < m_size && m_denseEntities[denseIdx] == id;
    }

    template <typename Self>
    [[nodiscard]] constexpr auto* TryGet(this Self&& self, EntityId id) noexcept {
        const u16 denseIdx = self.m_sparse[id.index];
        if (denseIdx < self.m_size && self.m_denseEntities[denseIdx] == id) [[likely]] {
            return &self.m_denseComponents[denseIdx];
        }
        return static_cast<decltype(&self.m_denseComponents[0])>(nullptr);
    }

    template <typename Self>
    [[nodiscard]] constexpr auto& DirectAtEntityIndex(this Self&& self, u16 entityIndex) noexcept {
        return self.m_denseComponents[self.m_sparse[entityIndex]];
    }

    template <typename... Args>
    [[nodiscard]] Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
        const u16 existing = m_sparse[id.index];
        if (existing < m_size && m_denseEntities[existing] == id) {
            m_denseComponents[existing] = T{std::forward<Args>(args)...};
            return &m_denseComponents[existing];
        }
        if (m_size >= m_capacity) [[unlikely]] {
            return Err(WorldErrc::ComponentPoolExhausted);
        }
        const u16 denseIdx = m_size++;
        m_sparse[id.index] = denseIdx;
        m_presenceWords[id.index >> 6] |= (1ULL << (id.index & 63u));
        m_denseEntities[denseIdx]   = id;
        m_denseComponents[denseIdx] = T{std::forward<Args>(args)...};
        return &m_denseComponents[denseIdx];
    }

    bool Remove(EntityId id) noexcept {
        const u16 denseIdx = m_sparse[id.index];
        if (denseIdx >= m_size || m_denseEntities[denseIdx] != id) [[unlikely]] {
            return false;
        }
        RemoveAtDense(denseIdx, id.index);
        return true;
    }

    void RemoveUnchecked(EntityId id) noexcept {
        const u16 denseIdx = m_sparse[id.index];
        if (denseIdx < m_size) [[likely]] {
            RemoveAtDense(denseIdx, id.index);
        }
    }

    constexpr void SwapDenseSlots(u16 idxA, u16 idxB) noexcept {
        if (idxA == idxB) {
            return;
        }
        const EntityId entA = m_denseEntities[idxA];
        const EntityId entB = m_denseEntities[idxB];
        std::swap(m_denseEntities[idxA], m_denseEntities[idxB]);
        std::swap(m_denseComponents[idxA], m_denseComponents[idxB]);
        m_sparse[entA.index] = idxB;
        m_sparse[entB.index] = idxA;
    }

    constexpr u16 PromoteToGroup(u16 entityIndex) noexcept {
        const u16 currentIdx = m_sparse[entityIndex];
        const u16 groupSlot  = m_groupCount++;
        if (currentIdx != groupSlot) {
            SwapDenseSlots(currentIdx, groupSlot);
        }
        return groupSlot;
    }

    constexpr void DemoteFromGroup(u16 entityIndex) noexcept {
        const u16 currentIdx = m_sparse[entityIndex];
        if (currentIdx < m_groupCount) {
            const u16 groupLast = --m_groupCount;
            if (currentIdx != groupLast) {
                SwapDenseSlots(currentIdx, groupLast);
            }
        }
    }

    [[nodiscard]] constexpr u16 Size() const noexcept { return m_size; }
    [[nodiscard]] constexpr u16 GroupSize() const noexcept { return m_groupCount; }
    [[nodiscard]] constexpr u16 DenseIndexOf(u16 entityIndex) const noexcept { return m_sparse[entityIndex]; }
    [[nodiscard]] constexpr u64 PresenceWord(u32 wordIdx) const noexcept { return m_presenceWords[wordIdx]; }
    [[nodiscard]] constexpr std::span<const EntityId> Entities() const noexcept { return m_denseEntities.first(m_size); }

    template <typename Self>
    [[nodiscard]] constexpr auto Components(this Self&& self) noexcept {
        return self.m_denseComponents.first(self.m_size);
    }

private:
    constexpr void RemoveAtDense(u16 targetIdx, u16 entityIndex) noexcept {
        if (targetIdx < m_groupCount) {
            const u16 groupLast = --m_groupCount;
            if (targetIdx != groupLast) {
                SwapDenseSlots(targetIdx, groupLast);
                targetIdx = groupLast;
            }
        }
        const u16 lastIdx = --m_size;
        if (targetIdx != lastIdx) {
            SwapDenseSlots(targetIdx, lastIdx);
        }
        m_sparse[entityIndex] = kInvalidDenseIndex;
        m_presenceWords[entityIndex >> 6] &= ~(1ULL << (entityIndex & 63u));
    }

    std::span<u16> m_sparse{};
    std::span<u64> m_presenceWords{};
    std::span<EntityId> m_denseEntities{};
    std::span<T> m_denseComponents{};
    u16 m_size{0};
    u16 m_capacity{0};
    u16 m_groupCount{0};
};

} // namespace en::world