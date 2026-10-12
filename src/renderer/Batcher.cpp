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
 * GatherInstances: Returns a span of draw-ordered instances.
 * Bypasses scratch copying if submissions were already monotonic and contiguous.
 * Eliminates redundant write-back copy into arena storage.
 */
std::span<const QuadInstance> GatherInstances(
    const FrameArenaSlot& slot, bool wasSorted, QuadInstance* scratch) noexcept {
    const auto entries = slot.SortEntries();
    const auto src     = slot.Instances();

    if (!wasSorted) {
        u32 expected = 0;
        bool contiguous = true;
        for (const auto& e : entries) {
            if (e.instanceIndex != expected) {
                contiguous = false;
                break;
            }
            expected += e.instanceCount;
        }
        if (contiguous) {
            return src.subspan(0, std::min<usize>(expected, src.size()));
        }
    }

    u32 out = 0;
    for (const auto& e : entries) {
        if (e.instanceIndex >= src.size()) continue;
        const u32 n = std::min({
            static_cast<u32>(e.instanceCount),
            static_cast<u32>(src.size()) - e.instanceIndex,
            FrameArenaSlot::kMaxInstances - out
        });
        std::memcpy(scratch + out, src.data() + e.instanceIndex, n * sizeof(QuadInstance));
        out += n;
    }
    return {scratch, out};
}

struct CoalesceResult {
    u32 batches = 0;
    u32 droppedBatches = 0;
};

/**
 * CoalesceBatches: Merges contiguous sorted entries sharing identical GPU pipeline state.
 * Once capacity is reached, halts further merging to prevent silent instance corruption.
 */
CoalesceResult CoalesceBatches(FrameArenaSlot& slot) noexcept {
    const auto entries = slot.SortEntries();
    auto batches = slot.MutableBatches();
    CoalesceResult r{};
    u32 currentOffset = 0;
    bool overflowed = false;

    for (const auto& entry : entries) {
        const u32 count = entry.instanceCount;
        if (count == 0) continue;

        const SortKey key{entry.key};
        const u8  passId      = key.GetPass();
        const u8  blendMode   = key.GetBlend();
        const u16 pipelineId  = key.GetPipeline();
        const u16 textureSlot = key.GetTexture();
        const u8  viewSlot    = static_cast<u8>(entry.viewSlot);

        if (!overflowed && r.batches > 0) {
            DrawBatch& prev = batches[r.batches - 1];
            if (prev.passId == passId &&
                prev.blendMode == blendMode &&
                prev.pipelineId == pipelineId &&
                prev.textureSlot == textureSlot &&
                prev.viewSlot == viewSlot) {
                prev.instanceCount += count;
                currentOffset += count;
                continue;
            }
        }

        if (r.batches < FrameArenaSlot::kMaxBatches) {
            batches[r.batches++] = DrawBatch{
                .instanceOffset = currentOffset,
                .instanceCount = count,
                .textureSlot = textureSlot,
                .pipelineId = pipelineId,
                .blendMode = blendMode,
                .passId = passId,
                .viewSlot = viewSlot,
                .reserved = 0
            };
        } else {
            overflowed = true;
            ++r.droppedBatches;
        }

        currentOffset += count;
    }

    slot.SetBatchCount(r.batches);
    slot.SetDroppedBatches(r.droppedBatches);
    return r;
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
            .droppedBatches = slot.DroppedBatches(),
            .droppedEntries = slot.DroppedEntries(),
            .wasSorted = false
        };
        return;
    }

    // 1. Sort entries by 64-bit key (radix sort with monotonic skip)
    const bool wasSorted = SortEntries(slot.MutableSortEntries(), m_sortScratch.get());

    // 2. Gather QuadInstances into draw order without redundant write-back
    const auto gathered = GatherInstances(slot, wasSorted, m_instanceScratch.get());

    // 3. Coalesce contiguous identical states into DrawBatches with overflow guard
    const auto coalesceRes = CoalesceBatches(slot);

    // 4. Fill immutable FramePacket
    outPacket.batches = slot.Batches();
    outPacket.instances = gathered;
    outPacket.viewSlots = slot.ViewSlots();
    outPacket.passes = slot.Passes();
    outPacket.stats = FrameStats{
        .instanceCount = static_cast<u32>(gathered.size()),
        .batchCount = coalesceRes.batches,
        .sortEntryCount = sortCount,
        .viewSlotCount = static_cast<u32>(slot.ViewSlots().size()),
        .droppedInstances = slot.DroppedInstances(),
        .droppedBatches = coalesceRes.droppedBatches,
        .droppedEntries = slot.DroppedEntries(),
        .wasSorted = wasSorted
    };
}

} // namespace EngineN
