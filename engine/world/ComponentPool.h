#pragma once

#include "core/Memory.h"
#include "world/WorldTypes.h"

namespace en {

struct alignas(32) PoolOps {
    void* poolPtr{nullptr};
    void (*removeUncheckedFn)(void* pool, EntityId id) noexcept {nullptr};
    bool (*emplaceRawFn)(void* pool, EntityId id, const void* raw) noexcept {nullptr};
    void (*clearFn)(void* pool) noexcept {nullptr};
};
static_assert(sizeof(PoolOps) == 32);

// Arena-backed Sparse-Set component storage with O(1) lookup, insertion, and swap-and-pop removal.
template <Component T>
class alignas(32) ComponentPool {
public:
    [[nodiscard]] bool Init(AlignedArena& arena, u16 maxEntities, u16 capacity) noexcept {
        if (maxEntities == 0 || capacity == 0) return false;
        m_sparse = arena.Carve<u16>(maxEntities);
        m_denseEntities = arena.Carve<EntityId>(capacity);
        m_denseComponents = arena.Carve<T>(capacity);
        if (m_sparse.empty() || m_denseEntities.empty() || m_denseComponents.empty()) return false;
        for (u16& s : m_sparse) s = kInvalidDenseIndex;
        m_capacity = capacity;
        m_size = 0;
        return true;
    }

    constexpr void Clear() noexcept {
        for (u16 i = 0; i < m_size; ++i) {
            m_sparse[m_denseEntities[i].index] = kInvalidDenseIndex;
        }
        m_size = 0;
    }

    [[nodiscard]] constexpr bool Contains(EntityId id) const noexcept {
        if (id.index >= m_sparse.size()) return false;
        const u16 d = m_sparse[id.index];
        return d < m_size && m_denseEntities[d] == id;
    }

    [[nodiscard]] constexpr T* TryGet(EntityId id) noexcept {
        if (id.index >= m_sparse.size()) return nullptr;
        const u16 d = m_sparse[id.index];
        return (d < m_size && m_denseEntities[d] == id) ? &m_denseComponents[d] : nullptr;
    }
    [[nodiscard]] constexpr const T* TryGet(EntityId id) const noexcept {
        if (id.index >= m_sparse.size()) return nullptr;
        const u16 d = m_sparse[id.index];
        return (d < m_size && m_denseEntities[d] == id) ? &m_denseComponents[d] : nullptr;
    }

    [[nodiscard]] constexpr T& DirectAtEntityIndex(u16 entityIndex) noexcept {
        return m_denseComponents[m_sparse[entityIndex]];
    }
    [[nodiscard]] constexpr const T& DirectAtEntityIndex(u16 entityIndex) const noexcept {
        return m_denseComponents[m_sparse[entityIndex]];
    }

    [[nodiscard]] constexpr T& ComponentAtDense(usize denseIdx) noexcept {
        return m_denseComponents[denseIdx];
    }
    [[nodiscard]] constexpr const T& ComponentAtDense(usize denseIdx) const noexcept {
        return m_denseComponents[denseIdx];
    }

    template <typename... Args>
    [[nodiscard]] constexpr Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
        if (id.index >= m_sparse.size()) return Err(WorldErrc::DeadEntity);
        const u16 existing = m_sparse[id.index];
        if (existing < m_size && m_denseEntities[existing] == id) {
            m_denseComponents[existing] = T{std::forward<Args>(args)...};
            return &m_denseComponents[existing];
        }
        if (m_size >= m_capacity) return Err(WorldErrc::PoolCapacityReached);
        const u16 d = m_size++;
        m_sparse[id.index] = d;
        m_denseEntities[d] = id;
        m_denseComponents[d] = T{std::forward<Args>(args)...};
        return &m_denseComponents[d];
    }

    [[nodiscard]] bool EmplaceRaw(EntityId id, const void* rawComponent) noexcept {
        if (!rawComponent || id.index >= m_sparse.size()) return false;
        T temp{};
        std::memcpy(&temp, rawComponent, sizeof(T));
        return Emplace(id, temp).has_value();
    }

    constexpr bool Remove(EntityId id) noexcept {
        if (!Contains(id)) return false;
        RemoveUnchecked(id);
        return true;
    }

    constexpr void RemoveUnchecked(EntityId id) noexcept {
        const u16 d = m_sparse[id.index];
        if (d >= m_size) return;
        const u16 lastIdx = static_cast<u16>(m_size - 1U);
        if (d != lastIdx) {
            const EntityId movedEntity = m_denseEntities[lastIdx];
            m_denseEntities[d] = movedEntity;
            m_denseComponents[d] = m_denseComponents[lastIdx];
            m_sparse[movedEntity.index] = d;
        }
        m_sparse[id.index] = kInvalidDenseIndex;
        --m_size;
    }

    [[nodiscard]] constexpr u16 Size() const noexcept { return m_size; }
    [[nodiscard]] constexpr u16 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr std::span<const EntityId> Entities() const noexcept {
        return m_denseEntities.subspan(0, m_size);
    }
    [[nodiscard]] constexpr std::span<T> Components() noexcept {
        return std::span<T>(m_denseComponents.data(), m_size);
    }
    [[nodiscard]] constexpr std::span<const T> Components() const noexcept {
        return std::span<const T>(m_denseComponents.data(), m_size);
    }

private:
    std::span<u16> m_sparse{};
    std::span<EntityId> m_denseEntities{};
    std::span<T> m_denseComponents{};
    u16 m_capacity{0};
    u16 m_size{0};
};

} // namespace en
