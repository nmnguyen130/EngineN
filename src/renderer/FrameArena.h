#pragma once

#include "core/Types.h"
#include "renderer/RendererTypes.h"
#include "renderer/QuadInstance.h"
#include "renderer/SortKey.h"
#include "renderer/FramePacket.h"
#include <memory>
#include <span>

namespace EngineN {

/**
 * FrameArenaSlot: Single-slot linear storage pre-allocated at boot.
 * Guaranteed 0 allocations per frame. Excess draws are dropped with logged telemetry.
 */
class FrameArenaSlot {
public:
    static constexpr u32 kMaxInstances   = 131'072; // ~6.0 MB
    static constexpr u32 kMaxSortEntries = 65'536;  // ~1.0 MB
    static constexpr u32 kMaxBatches     = 4'096;   // ~64 KB
    static constexpr u32 kMaxViewSlots   = 32;
    static constexpr u32 kMaxPasses      = 16;

    FrameArenaSlot();
    ~FrameArenaSlot() = default;

    FrameArenaSlot(const FrameArenaSlot&) = delete;
    FrameArenaSlot& operator=(const FrameArenaSlot&) = delete;
    FrameArenaSlot(FrameArenaSlot&&) noexcept;
    FrameArenaSlot& operator=(FrameArenaSlot&&) noexcept;

    void Reset() noexcept;

    // Allocation & append methods
    [[nodiscard]] u32 PushQuads(const QuadInstance* quads, u32 count) noexcept;
    [[nodiscard]] bool PushQuad(const QuadInstance& quad, const SortKey& key, u16 viewSlot = 0) noexcept;
    [[nodiscard]] bool PushSortEntry(const SortEntry& entry) noexcept;
    [[nodiscard]] u16 PushViewSlot(const ViewSlotDesc& slot) noexcept;
    [[nodiscard]] bool PushPass(const RenderPassDesc& pass) noexcept;

    // Direct mutable access for Batcher
    [[nodiscard]] std::span<QuadInstance> MutableInstances() noexcept {
        return {m_instances.get(), m_instanceCount};
    }
    [[nodiscard]] std::span<SortEntry> MutableSortEntries() noexcept {
        return {m_sortEntries.get(), m_sortEntryCount};
    }
    [[nodiscard]] std::span<DrawBatch> MutableBatches() noexcept {
        return {m_batches.get(), kMaxBatches};
    }

    // Const spans for FramePacket construction
    [[nodiscard]] std::span<const QuadInstance> Instances() const noexcept {
        return {m_instances.get(), m_instanceCount};
    }
    [[nodiscard]] std::span<const SortEntry> SortEntries() const noexcept {
        return {m_sortEntries.get(), m_sortEntryCount};
    }
    [[nodiscard]] std::span<const DrawBatch> Batches() const noexcept {
        return {m_batches.get(), m_batchCount};
    }
    [[nodiscard]] std::span<const ViewSlotDesc> ViewSlots() const noexcept {
        return {m_viewSlots.get(), m_viewSlotCount};
    }
    [[nodiscard]] std::span<const RenderPassDesc> Passes() const noexcept {
        return {m_passes.get(), m_passCount};
    }

    [[nodiscard]] u32 InstanceCount() const noexcept { return m_instanceCount; }
    [[nodiscard]] u32 SortEntryCount() const noexcept { return m_sortEntryCount; }
    [[nodiscard]] u32 DroppedInstances() const noexcept { return m_droppedInstances; }

    void SetBatchCount(u32 count) noexcept { m_batchCount = count; }

private:
    std::unique_ptr<QuadInstance[]>   m_instances;
    std::unique_ptr<SortEntry[]>      m_sortEntries;
    std::unique_ptr<DrawBatch[]>      m_batches;
    std::unique_ptr<ViewSlotDesc[]>   m_viewSlots;
    std::unique_ptr<RenderPassDesc[]> m_passes;

    u32 m_instanceCount{0};
    u32 m_sortEntryCount{0};
    u32 m_batchCount{0};
    u32 m_viewSlotCount{0};
    u32 m_passCount{0};
    u32 m_droppedInstances{0};
};

/**
 * Double-buffered FrameArena managing 2 frames in flight.
 */
class FrameArena {
public:
    FrameArena() = default;
    ~FrameArena() = default;

    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    [[nodiscard]] FrameArenaSlot& CurrentSlot() noexcept { return m_slots[m_currentSlot]; }
    [[nodiscard]] const FrameArenaSlot& CurrentSlot() const noexcept { return m_slots[m_currentSlot]; }

    void AdvanceSlot() noexcept {
        m_currentSlot = (m_currentSlot + 1) % 2;
    }

    [[nodiscard]] u32 SlotIndex() const noexcept { return m_currentSlot; }

private:
    FrameArenaSlot m_slots[2];
    u32 m_currentSlot{0};
};

} // namespace EngineN
