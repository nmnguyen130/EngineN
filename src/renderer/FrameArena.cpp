#include "renderer/FrameArena.h"
#include "core/Log.h"
#include <cstring>

namespace EngineN {

FrameArenaSlot::FrameArenaSlot()
    : m_instances(std::make_unique<QuadInstance[]>(kMaxInstances)),
      m_sortEntries(std::make_unique<SortEntry[]>(kMaxSortEntries)),
      m_batches(std::make_unique<DrawBatch[]>(kMaxBatches)),
      m_viewSlots(std::make_unique<ViewSlotDesc[]>(kMaxViewSlots)),
      m_passes(std::make_unique<RenderPassDesc[]>(kMaxPasses)) {
}

FrameArenaSlot::FrameArenaSlot(FrameArenaSlot&&) noexcept = default;
FrameArenaSlot& FrameArenaSlot::operator=(FrameArenaSlot&&) noexcept = default;

void FrameArenaSlot::Reset() noexcept {
    m_instanceCount = 0;
    m_sortEntryCount = 0;
    m_batchCount = 0;
    m_viewSlotCount = 0;
    m_passCount = 0;
    m_droppedInstances = 0;
}

u32 FrameArenaSlot::PushQuads(const QuadInstance* quads, u32 count) noexcept {
    if (m_instanceCount + count > kMaxInstances) {
        m_droppedInstances += count;
        ENGINE_LOG(Warn, Render, "FrameArenaSlot instance capacity exceeded! Dropping {} quads", count);
        return ~0u;
    }

    const u32 offset = m_instanceCount;
    std::memcpy(&m_instances[offset], quads, count * sizeof(QuadInstance));
    m_instanceCount += count;
    return offset;
}

bool FrameArenaSlot::PushQuad(const QuadInstance& quad, const SortKey& key, u16 viewSlot) noexcept {
    if (m_instanceCount >= kMaxInstances || m_sortEntryCount >= kMaxSortEntries) {
        m_droppedInstances++;
        return false;
    }
    const u32 idx = m_instanceCount++;
    m_instances[idx] = quad;
    m_sortEntries[m_sortEntryCount++] = SortEntry{
        .key = key.value,
        .instanceIndex = idx,
        .instanceCount = 1,
        .viewSlot = viewSlot
    };
    return true;
}

bool FrameArenaSlot::PushSortEntry(const SortEntry& entry) noexcept {
    if (m_sortEntryCount >= kMaxSortEntries) {
        ENGINE_LOG(Warn, Render, "FrameArenaSlot SortEntry capacity exceeded!");
        return false;
    }
    m_sortEntries[m_sortEntryCount++] = entry;
    return true;
}

u16 FrameArenaSlot::PushViewSlot(const ViewSlotDesc& slot) noexcept {
    if (m_viewSlotCount >= kMaxViewSlots) {
        ENGINE_LOG(Warn, Render, "FrameArenaSlot ViewSlot capacity exceeded, clamping to slot 0");
        return 0;
    }
    const u16 idx = static_cast<u16>(m_viewSlotCount++);
    m_viewSlots[idx] = slot;
    return idx;
}

bool FrameArenaSlot::PushPass(const RenderPassDesc& pass) noexcept {
    if (m_passCount >= kMaxPasses) {
        return false;
    }
    m_passes[m_passCount++] = pass;
    return true;
}

} // namespace EngineN
