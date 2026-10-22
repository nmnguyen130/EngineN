#pragma once

#include "core/Memory.h"
#include "world/WorldTypes.h"

namespace en {

// 32-byte (2 per 64B cache line, zero padding) non-template Sparse-Set component pool base.
// Executes Clear, RemoveUnchecked, EmplaceRaw, CloneFromEntityIndex, and GetRawByIndex directly without function-pointer tables or indirect calls.
// Invariant: for every d in [0, m_size), m_sparse[m_denseEntities[d].index] == d.
class alignas(32) RawComponentPool {
public:
    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_denseBytes != nullptr; }

    constexpr void Clear() noexcept {
        for (u16 i = 0U; i < m_size; ++i) {
            m_sparse[m_denseEntities[i].index] = kInvalidDenseIndex;
        }
        m_size = 0U;
    }

    [[nodiscard]] constexpr bool Contains(EntityId id) const noexcept {
        if (id.index >= m_maxEntities) return false;
        const u16 d = m_sparse[id.index];
        return d < m_size && m_denseEntities[d] == id;
    }

    [[nodiscard]] constexpr u16 DenseIndexOf(u16 entityIndex) const noexcept {
        return (entityIndex < m_maxEntities) ? m_sparse[entityIndex] : kInvalidDenseIndex;
    }

    [[nodiscard]] bool EmplaceRaw(EntityId id, const void* rawComponent) noexcept {
        if (!rawComponent || id.index >= m_maxEntities || m_denseBytes == nullptr) return false;
        const u16 existing = m_sparse[id.index];
        if (existing < m_size && m_denseEntities[existing] == id) {
            std::memcpy(
                m_denseBytes + static_cast<usize>(existing) * m_elemBytes,
                rawComponent,
                m_elemBytes);
            return true;
        }
        if (m_size >= m_capacity) return false;
        const u16 d = m_size++;
        m_sparse[id.index] = d;
        m_denseEntities[d] = id;
        std::memcpy(
            m_denseBytes + static_cast<usize>(d) * m_elemBytes,
            rawComponent,
            m_elemBytes);
        return true;
    }

    [[nodiscard]] bool CloneFromEntityIndex(EntityId dstId, u16 srcEntityIndex) noexcept {
        if (srcEntityIndex >= m_maxEntities || m_denseBytes == nullptr) return false;
        const u16 srcDense = m_sparse[srcEntityIndex];
        if (srcDense >= m_size) return false;
        return EmplaceRaw(dstId, m_denseBytes + static_cast<usize>(srcDense) * m_elemBytes);
    }

    void RemoveUnchecked(EntityId id) noexcept {
        if (id.index >= m_maxEntities || m_denseBytes == nullptr) return;
        const u16 d = m_sparse[id.index];
        if (d >= m_size) return;
        const u16 lastIdx = static_cast<u16>(m_size - 1U);
        if (d != lastIdx) {
            const EntityId movedEntity = m_denseEntities[lastIdx];
            m_denseEntities[d] = movedEntity;
            std::memcpy(
                m_denseBytes + static_cast<usize>(d) * m_elemBytes,
                m_denseBytes + static_cast<usize>(lastIdx) * m_elemBytes,
                m_elemBytes);
            m_sparse[movedEntity.index] = d;
        }
        m_sparse[id.index] = kInvalidDenseIndex;
        --m_size;
    }

    [[nodiscard]] constexpr const void* GetRawByEntityIndex(
        u16 entityIndex,
        u16& outElemBytes) const noexcept {
        outElemBytes = m_elemBytes;
        const u16 d = m_sparse[entityIndex];
        return m_denseBytes + static_cast<usize>(d) * m_elemBytes;
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

// Zero-overhead typed Sparse-Set view over RawComponentPool (32 bytes exact, zero additional members).
template <Component T>
class alignas(32) ComponentPool : public RawComponentPool {
public:
    [[nodiscard]] bool Init(AlignedArena& arena, u16 maxEntities, u16 capacity) noexcept {
        if (maxEntities == 0U || capacity == 0U) return false;
        const u32 marker = arena.SaveMarker();
        auto sparseSpan = arena.Carve<u16>(maxEntities);
        auto entitySpan = arena.Carve<EntityId>(capacity);
        auto compSpan = arena.Carve<T>(capacity);
        if (sparseSpan.empty() || entitySpan.empty() || compSpan.empty()) {
            arena.RollbackToMarker(marker);
            return false;
        }

        m_sparse = sparseSpan.data();
        m_denseEntities = entitySpan.data();
        m_denseBytes = reinterpret_cast<u8*>(compSpan.data());
        m_maxEntities = maxEntities;
        m_capacity = capacity;
        m_size = 0U;
        m_elemBytes = static_cast<u16>(sizeof(T));

        for (u16 i = 0U; i < maxEntities; ++i) {
            m_sparse[i] = kInvalidDenseIndex;
        }
        return true;
    }

    [[nodiscard]] constexpr T* TryGet(EntityId id) noexcept {
        if (id.index >= m_maxEntities) return nullptr;
        const u16 d = m_sparse[id.index];
        return (d < m_size && m_denseEntities[d] == id) ? &DenseTyped()[d] : nullptr;
    }

    [[nodiscard]] constexpr const T* TryGet(EntityId id) const noexcept {
        if (id.index >= m_maxEntities) return nullptr;
        const u16 d = m_sparse[id.index];
        return (d < m_size && m_denseEntities[d] == id) ? &DenseTyped()[d] : nullptr;
    }

    [[nodiscard]] constexpr T& DirectAtEntityIndex(u16 entityIndex) noexcept {
        return DenseTyped()[m_sparse[entityIndex]];
    }

    [[nodiscard]] constexpr const T& DirectAtEntityIndex(u16 entityIndex) const noexcept {
        return DenseTyped()[m_sparse[entityIndex]];
    }

    [[nodiscard]] constexpr T& ComponentAtDense(usize denseIdx) noexcept {
        return DenseTyped()[denseIdx];
    }

    [[nodiscard]] constexpr const T& ComponentAtDense(usize denseIdx) const noexcept {
        return DenseTyped()[denseIdx];
    }

    template <typename... Args>
    [[nodiscard]] constexpr Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
        if (id.index >= m_maxEntities) return Err(WorldErrc::DeadEntity);
        T* dense = DenseTyped();
        const u16 existing = m_sparse[id.index];
        if (existing < m_size && m_denseEntities[existing] == id) {
            dense[existing] = T{std::forward<Args>(args)...};
            return &dense[existing];
        }
        if (m_size >= m_capacity) return Err(WorldErrc::PoolCapacityReached);
        const u16 d = m_size++;
        m_sparse[id.index] = d;
        m_denseEntities[d] = id;
        dense[d] = T{std::forward<Args>(args)...};
        return &dense[d];
    }

    constexpr bool Remove(EntityId id) noexcept {
        if (!Contains(id)) return false;
        RemoveUnchecked(id);
        return true;
    }

    [[nodiscard]] constexpr std::span<T> Components() noexcept {
        return {DenseTyped(), m_size};
    }

    [[nodiscard]] constexpr std::span<const T> Components() const noexcept {
        return {DenseTyped(), m_size};
    }

private:
    [[nodiscard]] constexpr T* DenseTyped() noexcept {
        return reinterpret_cast<T*>(m_denseBytes);
    }
    [[nodiscard]] constexpr const T* DenseTyped() const noexcept {
        return reinterpret_cast<const T*>(m_denseBytes);
    }
};
static_assert(sizeof(ComponentPool<Velocity2D>) == 32);

} // namespace en
