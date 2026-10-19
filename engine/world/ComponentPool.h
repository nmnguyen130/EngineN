#pragma once

#include "world/WorldTypes.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

namespace en::world {

// 32B cache-aligned function pointer table for type-erased pool operations (2 per 64B cache line).
// removeOrClearFn removes a single entity when id.IsValid(), or clears the pool when id == kNullEntity.
struct alignas(32) PoolOps {
    void* poolInstance{nullptr};
    void (*removeOrClearFn)(void* pool, EntityId id) noexcept {nullptr};
    Result<void, WorldErrc> (*emplaceRawFn)(void* pool, EntityId id, const void* rawComponent) noexcept {nullptr};
    u64 (*hashCanonicalFn)(const void* pool, u16 maxEntityIndex) noexcept {nullptr};
};
static_assert(sizeof(PoolOps) == 32);

// Manages contiguous dense component storage with O(1) sparse indexing.
// Non-owning 32B descriptor (2 pools per 64B cache line) over arena slabs carved by World::Init().
// Invariant: For all i in [0, m_size), m_sparse[m_denseEntities[i].index] == i.
template <Component T>
class alignas(32) ComponentPool final {
public:
    void BindArena(
        std::span<u16> sparse,
        std::span<EntityId> denseEntities,
        std::span<T> denseComponents) noexcept {
        m_sparse          = sparse.data();
        m_denseEntities   = denseEntities.data();
        m_denseComponents = denseComponents.data();
        m_capacity        = static_cast<u16>(denseComponents.size());
        m_size            = 0;
        std::fill(sparse.begin(), sparse.end(), kInvalidDenseIndex);
    }

    constexpr void Clear() noexcept {
        for (u16 i = 0; i < m_size; ++i) {
            m_sparse[m_denseEntities[i].index] = kInvalidDenseIndex;
        }
        m_size = 0;
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

    template <typename Self>
    [[nodiscard]] constexpr auto& ComponentAtDense(this Self&& self, usize denseIdx) noexcept {
        return self.m_denseComponents[denseIdx];
    }

    template <typename... Args>
    [[nodiscard]] constexpr Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
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
        m_denseEntities[denseIdx]   = id;
        m_denseComponents[denseIdx] = T{std::forward<Args>(args)...};
        return &m_denseComponents[denseIdx];
    }

    // Alignment-safe raw component insertion for the 8B-aligned deferred command tape.
    [[nodiscard]] Result<void, WorldErrc> EmplaceRaw(EntityId id, const void* rawComponent) noexcept {
        const u16 existing = m_sparse[id.index];
        if (existing < m_size && m_denseEntities[existing] == id) {
            std::memcpy(&m_denseComponents[existing], rawComponent, sizeof(T));
            return {};
        }
        if (m_size >= m_capacity) [[unlikely]] {
            return Err(WorldErrc::ComponentPoolExhausted);
        }
        const u16 denseIdx = m_size++;
        m_sparse[id.index] = denseIdx;
        m_denseEntities[denseIdx] = id;
        std::memcpy(&m_denseComponents[denseIdx], rawComponent, sizeof(T));
        return {};
    }

    constexpr bool Remove(EntityId id) noexcept {
        const u16 denseIdx = m_sparse[id.index];
        if (denseIdx >= m_size || m_denseEntities[denseIdx] != id) [[unlikely]] {
            return false;
        }
        RemoveAtDense(denseIdx, id.index);
        return true;
    }

    constexpr void RemoveUnchecked(EntityId id) noexcept {
        const u16 denseIdx = m_sparse[id.index];
        if (denseIdx < m_size) [[likely]] {
            RemoveAtDense(denseIdx, id.index);
        }
    }

    [[nodiscard]] constexpr u16 Size() const noexcept { return m_size; }
    [[nodiscard]] constexpr u16 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr u16 DenseIndexOf(u16 entityIndex) const noexcept { return m_sparse[entityIndex]; }

    [[nodiscard]] constexpr std::span<const EntityId> Entities() const noexcept {
        return {m_denseEntities, m_size};
    }

    template <typename Self>
    [[nodiscard]] constexpr auto Components(this Self&& self) noexcept {
        using ElemT = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const T, T>;
        return std::span<ElemT>{self.m_denseComponents, self.m_size};
    }

private:
    constexpr void RemoveAtDense(u16 targetIdx, u16 entityIndex) noexcept {
        const u16 lastIdx = --m_size;
        if (targetIdx != lastIdx) {
            const EntityId movedEnt      = m_denseEntities[lastIdx];
            m_denseEntities[targetIdx]   = movedEnt;
            m_denseComponents[targetIdx] = m_denseComponents[lastIdx];
            m_sparse[movedEnt.index]     = targetIdx;
        }
        m_sparse[entityIndex] = kInvalidDenseIndex;
    }

    u16* m_sparse{nullptr};
    EntityId* m_denseEntities{nullptr};
    T* m_denseComponents{nullptr};
    u16 m_size{0};
    u16 m_capacity{0};
    u32 m_reserved{0};
};
static_assert(sizeof(ComponentPool<Velocity2D>) == 32);

} // namespace en::world
