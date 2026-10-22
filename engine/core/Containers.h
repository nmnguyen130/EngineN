#pragma once

#include "core/Memory.h"

namespace en {

// Generational dense-sparse slot map carved over an AlignedArena in a 32-byte header.
// Non-owning view over arena memory; O(1) handle lookup, O(1) swap-and-pop removal, contiguous dense iteration.
// Invariant: for every d in [0, m_activeCount), m_slots[m_denseToSparse[d]].denseOrFreeNext == d.
template <typename T, typename Tag = T>
class alignas(8) SlotMap {
public:
    static constexpr u16 kNullSlot = 0xFFFFU;

    struct alignas(4) SparseSlot {
        u16 denseOrFreeNext{kNullSlot};
        u16 generation{0};
    };
    static_assert(sizeof(SparseSlot) == 4);

    [[nodiscard]] bool Init(AlignedArena& arena, u16 capacity) noexcept {
        if (capacity == 0U || capacity == kNullSlot) return false;
        const u32 marker = arena.SaveMarker();
        auto values = arena.Carve<T>(capacity);
        auto slots = arena.Carve<SparseSlot>(capacity);
        auto denseToSparse = arena.Carve<u16>(capacity);
        if (values.empty() || slots.empty() || denseToSparse.empty()) {
            arena.RollbackToMarker(marker);
            m_values = nullptr;
            m_slots = nullptr;
            m_denseToSparse = nullptr;
            m_capacity = 0U;
            m_activeCount = 0U;
            m_freeHead = kNullSlot;
            return false;
        }
        m_values = values.data();
        m_slots = slots.data();
        m_denseToSparse = denseToSparse.data();
        m_capacity = capacity;
        m_activeCount = 0U;
        Clear();
        return true;
    }

    constexpr void Clear() noexcept {
        for (u16 d = 0; d < m_activeCount; ++d) {
            const u16 sparseIdx = m_denseToSparse[d];
            m_values[d] = T{};
            m_slots[sparseIdx].generation = NextGeneration(m_slots[sparseIdx].generation);
        }
        m_activeCount = 0U;
        m_freeHead = (m_capacity > 0U) ? 0U : kNullSlot;
        for (u16 i = 0; i < m_capacity; ++i) {
            m_slots[i].denseOrFreeNext = (i + 1U < m_capacity) ? static_cast<u16>(i + 1U) : kNullSlot;
        }
    }

    [[nodiscard]] constexpr Handle<Tag> Allocate(const T& initialValue = {}) noexcept {
        if (m_freeHead == kNullSlot || m_activeCount >= m_capacity) return {};
        const u16 sparseIdx = m_freeHead;
        SparseSlot& slot = m_slots[sparseIdx];
        m_freeHead = slot.denseOrFreeNext;

        if (slot.generation == 0U) slot.generation = 1U;
        const u16 denseIdx = m_activeCount++;
        slot.denseOrFreeNext = denseIdx;
        m_denseToSparse[denseIdx] = sparseIdx;
        m_values[denseIdx] = initialValue;
        return Handle<Tag>{sparseIdx, slot.generation};
    }

    constexpr bool Free(Handle<Tag> h) noexcept {
        if (!IsValid(h)) return false;
        SparseSlot& removedSlot = m_slots[h.index];
        const u16 denseIdx = removedSlot.denseOrFreeNext;
        const u16 lastDenseIdx = static_cast<u16>(m_activeCount - 1U);

        if (denseIdx != lastDenseIdx) {
            const u16 movedSparseIdx = m_denseToSparse[lastDenseIdx];
            m_values[denseIdx] = std::move(m_values[lastDenseIdx]);
            m_denseToSparse[denseIdx] = movedSparseIdx;
            m_slots[movedSparseIdx].denseOrFreeNext = denseIdx;
        }
        m_values[lastDenseIdx] = T{};
        --m_activeCount;

        removedSlot.generation = NextGeneration(removedSlot.generation);
        removedSlot.denseOrFreeNext = m_freeHead;
        m_freeHead = h.index;
        return true;
    }

    [[nodiscard]] constexpr bool IsValid(Handle<Tag> h) const noexcept {
        if (!h.IsValid() || h.index >= m_capacity) return false;
        const SparseSlot& slot = m_slots[h.index];
        return slot.generation == h.generation &&
               slot.denseOrFreeNext < m_activeCount &&
               m_denseToSparse[slot.denseOrFreeNext] == h.index;
    }

    [[nodiscard]] constexpr T* TryGet(Handle<Tag> h) noexcept {
        return IsValid(h) ? &m_values[m_slots[h.index].denseOrFreeNext] : nullptr;
    }

    [[nodiscard]] constexpr const T* TryGet(Handle<Tag> h) const noexcept {
        return IsValid(h) ? &m_values[m_slots[h.index].denseOrFreeNext] : nullptr;
    }

    [[nodiscard]] constexpr std::span<T> Values() noexcept {
        return {m_values, m_activeCount};
    }

    [[nodiscard]] constexpr std::span<const T> Values() const noexcept {
        return {m_values, m_activeCount};
    }

    [[nodiscard]] constexpr u16 ActiveCount() const noexcept { return m_activeCount; }
    [[nodiscard]] constexpr u16 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_activeCount == 0U; }
    [[nodiscard]] constexpr bool Full() const noexcept { return m_activeCount >= m_capacity; }

private:
    [[nodiscard]] static constexpr u16 NextGeneration(u16 gen) noexcept {
        const u16 next = static_cast<u16>(gen + 1U);
        return (next == 0U) ? 1U : next;
    }

    T* m_values{nullptr};
    SparseSlot* m_slots{nullptr};
    u16* m_denseToSparse{nullptr};
    u16 m_capacity{0};
    u16 m_activeCount{0};
    u16 m_freeHead{kNullSlot};
};
static_assert(sizeof(SlotMap<u32>) == 32);

// Open-addressed power-of-two Robin Hood hash map packed into 16 bytes with single-pass upsert and backward-shift deletion.
// Non-owning view over AlignedArena slabs; zero tombstones and zero heap allocation after Init().
// Invariant: m_capacity is a power of two, key == 0 is reserved as null, and dist == 0 marks an empty slot.
template <typename ValueT>
class alignas(8) RobinHoodMap {
public:
    struct alignas(8) Slot {
        u64 key{0};
        ValueT value{};
        u32 dist{0};
    };

    [[nodiscard]] bool Init(AlignedArena& arena, u32 minCapacity) noexcept {
        if (minCapacity == 0U || minCapacity > 0x40000000U) return false;
        const u32 p2 = NextPow2(minCapacity);
        const u32 cap = (p2 < 16U) ? 16U : p2;
        auto slots = arena.Carve<Slot>(cap);
        if (slots.empty()) {
            m_slots = nullptr;
            m_capacity = 0U;
            m_count = 0U;
            return false;
        }
        m_slots = slots.data();
        m_capacity = cap;
        m_count = 0U;
        return true;
    }

    constexpr void Clear() noexcept {
        for (u32 i = 0; i < m_capacity; ++i) {
            m_slots[i] = Slot{};
        }
        m_count = 0U;
    }

    // Single-pass Robin Hood upsert: updates existing keys even at MaxLoad() and rejects new keys when full.
    [[nodiscard]] constexpr bool InsertOrAssign(u64 key, const ValueT& val) noexcept {
        if (key == 0ULL || m_slots == nullptr) return false;
        const u32 mask = m_capacity - 1U;
        const u32 maxLoad = MaxLoad();
        u32 idx = static_cast<u32>(Mix64(key)) & mask;
        u64 curKey = key;
        ValueT curVal = val;
        u32 curDist = 1U;
        bool displacing = false;

        for (u32 step = 0; step < m_capacity; ++step) {
            Slot& slot = m_slots[idx];
            if (slot.dist == 0U) {
                if (!displacing && m_count >= maxLoad) return false;
                slot.key = curKey;
                slot.value = std::move(curVal);
                slot.dist = curDist;
                ++m_count;
                return true;
            }
            if (!displacing && slot.key == curKey) {
                slot.value = std::move(curVal);
                return true;
            }
            if (slot.dist < curDist) {
                if (!displacing && m_count >= maxLoad) return false;
                displacing = true;
                std::swap(slot.key, curKey);
                std::swap(slot.value, curVal);
                std::swap(slot.dist, curDist);
            }
            idx = (idx + 1U) & mask;
            ++curDist;
        }
        return false;
    }

    [[nodiscard]] constexpr ValueT* Find(u64 key) noexcept {
        const u32 idx = FindSlotIndex(key);
        return (idx != kInvalidIndex) ? &m_slots[idx].value : nullptr;
    }

    [[nodiscard]] constexpr const ValueT* Find(u64 key) const noexcept {
        const u32 idx = FindSlotIndex(key);
        return (idx != kInvalidIndex) ? &m_slots[idx].value : nullptr;
    }

    [[nodiscard]] constexpr bool Contains(u64 key) const noexcept {
        return FindSlotIndex(key) != kInvalidIndex;
    }

    constexpr bool Erase(u64 key) noexcept {
        u32 idx = FindSlotIndex(key);
        if (idx == kInvalidIndex) return false;
        const u32 mask = m_capacity - 1U;
        for (u32 shift = 1U; shift < m_capacity; ++shift) {
            const u32 nextIdx = (idx + 1U) & mask;
            if (m_slots[nextIdx].dist <= 1U) break;
            m_slots[idx] = std::move(m_slots[nextIdx]);
            --m_slots[idx].dist;
            idx = nextIdx;
        }
        m_slots[idx] = Slot{};
        --m_count;
        return true;
    }

    [[nodiscard]] constexpr u32 Size() const noexcept { return m_count; }
    [[nodiscard]] constexpr u32 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr u32 MaxLoad() const noexcept { return m_capacity - (m_capacity >> 3U); }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_count == 0U; }
    [[nodiscard]] constexpr std::span<const Slot> Slots() const noexcept { return {m_slots, m_capacity}; }

private:
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFU;

    [[nodiscard]] constexpr u32 FindSlotIndex(u64 key) const noexcept {
        if (key == 0ULL || m_slots == nullptr) return kInvalidIndex;
        const u32 mask = m_capacity - 1U;
        u32 idx = static_cast<u32>(Mix64(key)) & mask;
        u32 dist = 1U;
        for (u32 step = 0; step < m_capacity; ++step) {
            const Slot& slot = m_slots[idx];
            if (slot.dist == 0U || slot.dist < dist) return kInvalidIndex;
            if (slot.key == key) return idx;
            idx = (idx + 1U) & mask;
            ++dist;
        }
        return kInvalidIndex;
    }

    Slot* m_slots{nullptr};
    u32 m_capacity{0};
    u32 m_count{0};
};
static_assert(sizeof(RobinHoodMap<u32>) == 16);
static_assert(sizeof(RobinHoodMap<u32>::Slot) == 16);

// Fixed-capacity inline contiguous vector avoiding heap allocation for bounded collections.
// Value type owning inline array storage.
// Invariant: m_size <= MaxCapacity.
template <typename T, usize MaxCapacity>
    requires (MaxCapacity > 0 && MaxCapacity <= 0xFFFFFFFFU)
class SmallVector {
public:
    constexpr void Clear() noexcept { m_size = 0U; }

    constexpr bool PushBack(const T& val) noexcept {
        if (m_size >= MaxCapacity) return false;
        m_data[m_size++] = val;
        return true;
    }

    constexpr bool PopBack() noexcept {
        if (m_size == 0U) return false;
        --m_size;
        return true;
    }

    constexpr bool EraseSwapBack(usize idx) noexcept {
        if (idx >= m_size) return false;
        if (idx + 1U < m_size) {
            m_data[idx] = std::move(m_data[m_size - 1U]);
        }
        --m_size;
        return true;
    }

    [[nodiscard]] constexpr T& operator[](usize idx) noexcept { return m_data[idx]; }
    [[nodiscard]] constexpr const T& operator[](usize idx) const noexcept { return m_data[idx]; }

    [[nodiscard]] constexpr std::span<T> Span() noexcept { return {m_data.data(), m_size}; }
    [[nodiscard]] constexpr std::span<const T> Span() const noexcept { return {m_data.data(), m_size}; }

    [[nodiscard]] constexpr T* begin() noexcept { return m_data.data(); }
    [[nodiscard]] constexpr const T* begin() const noexcept { return m_data.data(); }
    [[nodiscard]] constexpr T* end() noexcept { return m_data.data() + m_size; }
    [[nodiscard]] constexpr const T* end() const noexcept { return m_data.data() + m_size; }

    [[nodiscard]] constexpr u32 Size() const noexcept { return m_size; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_size == 0U; }
    [[nodiscard]] constexpr bool Full() const noexcept { return m_size >= MaxCapacity; }
    [[nodiscard]] static constexpr usize Capacity() noexcept { return MaxCapacity; }

private:
    std::array<T, MaxCapacity> m_data{};
    u32 m_size{0};
};

// Fixed-capacity power-of-two FIFO ring buffer with wrapping 32-bit unsigned indices.
// Value type owning inline storage; zero dynamic memory allocation.
// Invariant: (m_tail - m_head) <= CapacityPow2.
template <typename T, usize CapacityPow2>
    requires (std::has_single_bit(CapacityPow2) && CapacityPow2 <= 0x80000000U)
class RingBuffer {
public:
    constexpr void Clear() noexcept {
        m_head = 0U;
        m_tail = 0U;
    }

    [[nodiscard]] constexpr bool Push(const T& item) noexcept {
        if (Full()) return false;
        m_storage[m_tail & kMask] = item;
        ++m_tail;
        return true;
    }

    [[nodiscard]] constexpr bool Pop(T& outItem) noexcept {
        if (Empty()) return false;
        outItem = std::move(m_storage[m_head & kMask]);
        ++m_head;
        return true;
    }

    [[nodiscard]] constexpr T* PeekFront() noexcept {
        return Empty() ? nullptr : &m_storage[m_head & kMask];
    }

    [[nodiscard]] constexpr const T* PeekFront() const noexcept {
        return Empty() ? nullptr : &m_storage[m_head & kMask];
    }

    [[nodiscard]] constexpr T* PeekBack() noexcept {
        return Empty() ? nullptr : &m_storage[(m_tail - 1U) & kMask];
    }

    [[nodiscard]] constexpr const T* PeekBack() const noexcept {
        return Empty() ? nullptr : &m_storage[(m_tail - 1U) & kMask];
    }

    [[nodiscard]] constexpr u32 Size() const noexcept { return m_tail - m_head; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_head == m_tail; }
    [[nodiscard]] constexpr bool Full() const noexcept { return Size() >= CapacityPow2; }
    [[nodiscard]] static constexpr usize Capacity() noexcept { return CapacityPow2; }

private:
    static constexpr u32 kMask = static_cast<u32>(CapacityPow2 - 1U);
    std::array<T, CapacityPow2> m_storage{};
    u32 m_head{0};
    u32 m_tail{0};
};

} // namespace en
