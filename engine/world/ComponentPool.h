#pragma once

#include "core/Memory.h"
#include "world/WorldTypes.h"

namespace en {

// Sparse-set pool over arena memory. Swap-and-pop remove.
// Invariant: sparse[dense[d].index] == d for all live d.
class alignas(32) RawComponentPool {
public:
    [[nodiscard]] constexpr bool IsInitialized() const noexcept {
        return m_denseBytes != nullptr;
    }

    constexpr void Clear() noexcept {
        for (u16 i = 0U; i < m_size; ++i) {
            m_sparse[m_denseEntities[i].index] = kInvalidDenseIndex;
        }
        m_size = 0U;
    }

    [[nodiscard]] constexpr bool Contains(EntityId id) const noexcept {
        if (id.index >= m_maxEntities) {
            return false;
        }
        const u16 d = m_sparse[id.index];
        return d < m_size && m_denseEntities[d] == id;
    }

    [[nodiscard]] constexpr u16 DenseIndexOf(u16 entityIndex) const noexcept {
        return (entityIndex < m_maxEntities) ? m_sparse[entityIndex] : kInvalidDenseIndex;
    }

    [[nodiscard]] bool EmplaceRaw(EntityId id, const void* raw) noexcept {
        if (!raw || id.index >= m_maxEntities || !m_denseBytes) {
            return false;
        }
        u16 d = m_sparse[id.index];
        if (d >= m_size || m_denseEntities[d] != id) {
            if (m_size >= m_capacity) {
                return false;
            }
            d = m_size++;
            m_sparse[id.index] = d;
            m_denseEntities[d] = id;
        }
        std::memcpy(m_denseBytes + static_cast<usize>(d) * m_elemBytes, raw, m_elemBytes);
        return true;
    }

    [[nodiscard]] bool CloneFromEntityIndex(EntityId dst, u16 srcIndex) noexcept {
        if (srcIndex >= m_maxEntities || !m_denseBytes) {
            return false;
        }
        const u16 srcDense = m_sparse[srcIndex];
        return srcDense < m_size &&
               EmplaceRaw(dst, m_denseBytes + static_cast<usize>(srcDense) * m_elemBytes);
    }

    void RemoveUnchecked(EntityId id) noexcept {
        if (id.index >= m_maxEntities || !m_denseBytes) {
            return;
        }
        const u16 d = m_sparse[id.index];
        if (d >= m_size) {
            return;
        }
        const u16 last = static_cast<u16>(--m_size);
        if (d != last) {
            const EntityId moved = m_denseEntities[last];
            m_denseEntities[d] = moved;
            std::memcpy(
                m_denseBytes + static_cast<usize>(d) * m_elemBytes,
                m_denseBytes + static_cast<usize>(last) * m_elemBytes,
                m_elemBytes);
            m_sparse[moved.index] = d;
        }
        m_sparse[id.index] = kInvalidDenseIndex;
    }

    [[nodiscard]] constexpr const u8* RawAtEntityIndex(u16 entityIndex) const noexcept {
        return m_denseBytes + static_cast<usize>(m_sparse[entityIndex]) * m_elemBytes;
    }

    [[nodiscard]] constexpr u16 Size() const noexcept { return m_size; }
    [[nodiscard]] constexpr u16 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr u16 ElemBytes() const noexcept { return m_elemBytes; }

    [[nodiscard]] constexpr std::span<const EntityId> Entities() const noexcept {
        return {m_denseEntities, m_size};
    }

protected:
    u16* m_sparse{nullptr};
    EntityId* m_denseEntities{nullptr};
    u8* m_denseBytes{nullptr};
    u16 m_maxEntities{0};
    u16 m_capacity{0};
    u16 m_size{0};
    u16 m_elemBytes{0};
};
static_assert(sizeof(RawComponentPool) == 32);

template <Component T>
class alignas(32) ComponentPool : public RawComponentPool {
public:
    [[nodiscard]] bool Init(AlignedArena& arena, u16 maxEntities, u16 capacity) noexcept {
        if (maxEntities == 0U || capacity == 0U) {
            return false;
        }
        const u32 marker = arena.SaveMarker();
        auto sparse = arena.Carve<u16>(maxEntities);
        auto entities = arena.Carve<EntityId>(capacity);
        auto comps = arena.Carve<T>(capacity);
        if (sparse.empty() || entities.empty() || comps.empty()) {
            arena.RollbackToMarker(marker);
            return false;
        }

        m_sparse = sparse.data();
        m_denseEntities = entities.data();
        m_denseBytes = reinterpret_cast<u8*>(comps.data());
        m_maxEntities = maxEntities;
        m_capacity = capacity;
        m_size = 0U;
        m_elemBytes = static_cast<u16>(sizeof(T));
        std::fill_n(m_sparse, maxEntities, kInvalidDenseIndex);
        return true;
    }

    [[nodiscard]] constexpr T* TryGet(EntityId id) noexcept {
        return Contains(id) ? &Dense()[m_sparse[id.index]] : nullptr;
    }

    [[nodiscard]] constexpr const T* TryGet(EntityId id) const noexcept {
        return Contains(id) ? &Dense()[m_sparse[id.index]] : nullptr;
    }

    [[nodiscard]] constexpr T& DirectAtEntityIndex(u16 entityIndex) noexcept {
        return Dense()[m_sparse[entityIndex]];
    }

    [[nodiscard]] constexpr const T& DirectAtEntityIndex(u16 entityIndex) const noexcept {
        return Dense()[m_sparse[entityIndex]];
    }

    [[nodiscard]] constexpr T& ComponentAtDense(usize denseIdx) noexcept {
        return Dense()[denseIdx];
    }

    [[nodiscard]] constexpr const T& ComponentAtDense(usize denseIdx) const noexcept {
        return Dense()[denseIdx];
    }

    template <typename... Args>
    [[nodiscard]] constexpr Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
        if (id.index >= m_maxEntities) {
            return Err(WorldErrc::DeadEntity);
        }
        u16 d = m_sparse[id.index];
        if (d >= m_size || m_denseEntities[d] != id) {
            if (m_size >= m_capacity) {
                return Err(WorldErrc::PoolCapacityReached);
            }
            d = m_size++;
            m_sparse[id.index] = d;
            m_denseEntities[d] = id;
        }
        T& slot = Dense()[d];
        slot = T{std::forward<Args>(args)...};
        return &slot;
    }

    constexpr bool Remove(EntityId id) noexcept {
        if (!Contains(id)) {
            return false;
        }
        RemoveUnchecked(id);
        return true;
    }

    [[nodiscard]] constexpr std::span<T> Components() noexcept {
        return {Dense(), m_size};
    }
    [[nodiscard]] constexpr std::span<const T> Components() const noexcept {
        return {Dense(), m_size};
    }

private:
    [[nodiscard]] constexpr T* Dense() noexcept {
        return reinterpret_cast<T*>(m_denseBytes);
    }
    [[nodiscard]] constexpr const T* Dense() const noexcept {
        return reinterpret_cast<const T*>(m_denseBytes);
    }
};
static_assert(sizeof(ComponentPool<Velocity2D>) == 32);

} // namespace en
