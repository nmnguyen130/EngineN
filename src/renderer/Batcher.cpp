#include "renderer/Batcher.h"
#include <algorithm>
#include <cstring>

namespace EngineN {

namespace {

/**
 * SortEntries: 8-bit LSD Radix Sort across varying key bytes only.
 * Returns true if entries were reordered, false if fast-path bypassed.
 */
bool SortEntries(std::span<SortEntry> entries, SortEntry* scratch) noexcept {
    const u32 count = static_cast<u32>(entries.size());
    if (count <= 1) {
        return false;
    }

    // Monotonic fast-path: check if entries are already non-decreasing
    bool isMonotonic = true;
    for (u32 i = 1; i < count; ++i) {
        if (entries[i].key < entries[i - 1].key) {
            isMonotonic = false;
            break;
        }
    }
    if (isMonotonic) {
        return false;
    }

    // Find varying bytes across 64-bit keys to skip uniform passes
    u64 orMask = 0;
    u64 andMask = ~0ULL;
    for (u32 i = 0; i < count; ++i) {
        orMask |= entries[i].key;
        andMask &= entries[i].key;
    }
    const u64 varying = orMask ^ andMask;

    SortEntry* src = entries.data();
    SortEntry* dst = scratch;

    for (int byteIdx = 0; byteIdx < 8; ++byteIdx) {
        if (((varying >> (byteIdx * 8)) & 0xFF) == 0) {
            continue;
        }

        u32 buckets[256] = {};
        const u32 shift = byteIdx * 8;

        for (u32 i = 0; i < count; ++i) {
            const u8 b = static_cast<u8>((src[i].key >> shift) & 0xFF);
            buckets[b]++;
        }

        u32 prefix[256];
        prefix[0] = 0;
        for (int b = 1; b < 256; ++b) {
            prefix[b] = prefix[b - 1] + buckets[b - 1];
        }

        for (u32 i = 0; i < count; ++i) {
            const u8 b = static_cast<u8>((src[i].key >> shift) & 0xFF);
            dst[prefix[b]++] = src[i];
        }

        std::swap(src, dst);
    }

    // Final output must reside in the original slot buffer
    if (src != entries.data()) {
        std::memcpy(entries.data(), scratch, count * sizeof(SortEntry));
    }
    return true;
}

/**
 * LinearizeInstances: Orders QuadInstances into contiguous memory matching sorted entries.
 * Bypasses 12 MB scratch copy if submissions are already monotonic and contiguous.
 */
u32 LinearizeInstances(FrameArenaSlot& slot, bool wasSorted, QuadInstance* scratch) noexcept {
    const auto entries = slot.SortEntries();
    const auto sourceQuads = slot.Instances();
    const u32 sortCount = static_cast<u32>(entries.size());

    // Check if quads already reside in contiguous sorted layout
    bool isContiguous = true;
    u32 expectedOffset = 0;
    for (u32 i = 0; i < sortCount; ++i) {
        if (entries[i].instanceIndex != expectedOffset) {
            isContiguous = false;
            break;
        }
        expectedOffset += entries[i].instanceCount;
    }

    if (!wasSorted && isContiguous) {
        return std::min(expectedOffset, static_cast<u32>(sourceQuads.size()));
    }

    // Reorder quads into scratch buffer with boundary clamping
    u32 totalOutput = 0;
    for (u32 i = 0; i < sortCount; ++i) {
        const auto& entry = entries[i];
        const u32 srcIdx = entry.instanceIndex;
        const u16 count = entry.instanceCount;

        if (count > 0 && srcIdx < sourceQuads.size()) {
            const u32 availableSrc = static_cast<u32>(sourceQuads.size() - srcIdx);
            const u32 copyCount = std::min<u32>(count, availableSrc);
            const u32 availableDst = (FrameArenaSlot::kMaxInstances > totalOutput)
                                         ? (FrameArenaSlot::kMaxInstances - totalOutput)
                                         : 0;
            const u32 toCopy = std::min(copyCount, availableDst);
            if (toCopy > 0) {
                std::memcpy(&scratch[totalOutput], &sourceQuads[srcIdx], toCopy * sizeof(QuadInstance));
                totalOutput += toCopy;
            }
        }
    }

    // Write back sorted contiguous instances into arena storage
    auto destQuads = slot.MutableInstances();
    if (totalOutput > 0) {
        const u32 writeCount = std::min(totalOutput, static_cast<u32>(destQuads.size()));
        std::memcpy(destQuads.data(), scratch, writeCount * sizeof(QuadInstance));
    }

    return totalOutput;
}

/**
 * CoalesceBatches: Merges contiguous sorted entries sharing identical GPU pipeline state.
 */
u32 CoalesceBatches(FrameArenaSlot& slot) noexcept {
    const auto entries = slot.SortEntries();
    auto batches = slot.MutableBatches();
    const u32 sortCount = static_cast<u32>(entries.size());

    u32 batchCount = 0;
    u32 currentOffset = 0;

    for (u32 i = 0; i < sortCount; ++i) {
        const auto& entry = entries[i];
        const u32 count = entry.instanceCount;
        if (count == 0) {
            continue;
        }

        const SortKey key{entry.key};
        const u8  passId      = key.GetPass();
        const u8  blendMode   = key.GetBlend();
        const u16 pipelineId  = key.GetPipeline();
        const u16 textureSlot = key.GetTexture();
        const u16 viewSlot    = entry.viewSlot;

        bool merged = false;
        if (batchCount > 0) {
            auto& prev = batches[batchCount - 1];
            if (prev.passId == passId &&
                prev.blendMode == blendMode &&
                prev.pipelineId == pipelineId &&
                prev.textureSlot == textureSlot &&
                prev.viewSlot == viewSlot) {
                prev.instanceCount += count;
                merged = true;
            }
        }

        if (!merged && batchCount < FrameArenaSlot::kMaxBatches) {
            batches[batchCount++] = DrawBatch{
                .instanceOffset = currentOffset,
                .instanceCount = count,
                .textureSlot = textureSlot,
                .pipelineId = pipelineId,
                .blendMode = blendMode,
                .passId = passId,
                .viewSlot = static_cast<u8>(viewSlot),
                .reserved = 0
            };
        }

        currentOffset += count;
    }

    slot.SetBatchCount(batchCount);
    return batchCount;
}

} // namespace

Batcher::Batcher()
    : m_sortScratch(std::make_unique<SortEntry[]>(FrameArenaSlot::kMaxSortEntries)),
      m_instanceScratch(std::make_unique<QuadInstance[]>(FrameArenaSlot::kMaxInstances)) {
}

void Batcher::Finalize(FrameArenaSlot& slot, FramePacket& outPacket) noexcept {
    const u32 sortCount = slot.SortEntryCount();
    if (sortCount == 0) {
        slot.SetBatchCount(0);
        outPacket.batches = {};
        outPacket.instances = {};
        outPacket.viewSlots = slot.ViewSlots();
        outPacket.passes = slot.Passes();
        outPacket.stats = FrameStats{
            .instanceCount = 0,
            .batchCount = 0,
            .sortEntryCount = 0,
            .viewSlotCount = static_cast<u32>(slot.ViewSlots().size()),
            .droppedInstances = slot.DroppedInstances(),
            .wasSorted = false
        };
        return;
    }

    // 1. Sort entries by 64-bit key (radix sort with monotonic skip)
    const bool wasSorted = SortEntries(slot.MutableSortEntries(), m_sortScratch.get());

    // 2. Linearize QuadInstances into sorted order
    const u32 totalOutputQuads = LinearizeInstances(slot, wasSorted, m_instanceScratch.get());

    // 3. Coalesce contiguous identical states into DrawBatches
    const u32 batchCount = CoalesceBatches(slot);

    // 4. Fill immutable FramePacket
    outPacket.batches = slot.Batches();
    outPacket.instances = slot.Instances().subspan(0, totalOutputQuads);
    outPacket.viewSlots = slot.ViewSlots();
    outPacket.passes = slot.Passes();
    outPacket.stats = FrameStats{
        .instanceCount = totalOutputQuads,
        .batchCount = batchCount,
        .sortEntryCount = sortCount,
        .viewSlotCount = static_cast<u32>(slot.ViewSlots().size()),
        .droppedInstances = slot.DroppedInstances(),
        .wasSorted = wasSorted
    };
}

} // namespace EngineN
