#include "renderer/RenderQueue.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace EngineN {

RenderQueue::RenderQueue(RenderQueue&& other) noexcept {
    *this = std::move(other);
}

RenderQueue& RenderQueue::operator=(RenderQueue&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_cfg = other.m_cfg;
        m_memoryBlock = std::move(other.m_memoryBlock);
        m_totalAllocatedBytes = other.m_totalAllocatedBytes;
        m_vertices = other.m_vertices;
        m_indices = other.m_indices;
        m_sortedIndices = other.m_sortedIndices;
        m_drawItems = other.m_drawItems;
        m_sortEntries = other.m_sortEntries;
        m_sortScratch = other.m_sortScratch;
        m_runs = other.m_runs;
        m_scissors = other.m_scissors;
        m_vertexCount = other.m_vertexCount;
        m_indexCount = other.m_indexCount;
        m_itemCount = other.m_itemCount;
        m_runCount = other.m_runCount;
        m_scissorCount = other.m_scissorCount;
        m_stats = other.m_stats;

        other.m_totalAllocatedBytes = 0;
        other.m_vertices = nullptr;
        other.m_indices = nullptr;
        other.m_sortedIndices = nullptr;
        other.m_drawItems = nullptr;
        other.m_sortEntries = nullptr;
        other.m_sortScratch = nullptr;
        other.m_runs = nullptr;
        other.m_scissors = nullptr;
    }
    return *this;
}

Result<void> RenderQueue::Init(const RenderQueueConfig& cfg) {
    Shutdown();
    m_cfg = cfg;

    // Calculate memory requirements for all frame arenas
    const usize vertexBytes  = m_cfg.maxVertices * sizeof(Vertex);
    const usize indexBytes   = m_cfg.maxIndices * sizeof(u32);
    const usize sortedBytes  = m_cfg.maxIndices * sizeof(u32);
    const usize itemBytes    = m_cfg.maxItems * sizeof(DrawItem);
    const usize sortBytes    = m_cfg.maxItems * sizeof(SortEntry);
    const usize scratchBytes = m_cfg.maxItems * sizeof(SortEntry);
    const usize runBytes     = m_cfg.maxRuns * sizeof(DrawRun);
    const usize scissorBytes = m_cfg.maxScissors * sizeof(Rect);

    // Pre-allocate a single contiguous arena to guarantee zero heap allocation during gameplay
    m_totalAllocatedBytes = vertexBytes + indexBytes + sortedBytes + itemBytes +
                            sortBytes + scratchBytes + runBytes + scissorBytes + 512;

    m_memoryBlock = std::make_unique<u8[]>(m_totalAllocatedBytes);
    if (!m_memoryBlock) {
        return Unexpected(MakeRenderError(
            RenderErrc::QueueCapacityExceeded,
            "Failed to allocate memory arena for RenderQueue"));
    }

    u8* ptr = m_memoryBlock.get();

    // 64-byte alignment helper for SIMD/cacheline efficiency
    auto align64 = [](u8* p) -> u8* {
        const uintptr_t raw = reinterpret_cast<uintptr_t>(p);
        const uintptr_t aligned = (raw + 63) & ~uintptr_t(63);
        return reinterpret_cast<u8*>(aligned);
    };

    // Sub-divide the arena into dedicated typed sub-buffers
    ptr = align64(ptr);
    m_vertices = reinterpret_cast<Vertex*>(ptr);
    ptr += vertexBytes;

    ptr = align64(ptr);
    m_indices = reinterpret_cast<u32*>(ptr);
    ptr += indexBytes;

    ptr = align64(ptr);
    m_sortedIndices = reinterpret_cast<u32*>(ptr);
    ptr += sortedBytes;

    ptr = align64(ptr);
    m_drawItems = reinterpret_cast<DrawItem*>(ptr);
    ptr += itemBytes;

    ptr = align64(ptr);
    m_sortEntries = reinterpret_cast<SortEntry*>(ptr);
    ptr += sortBytes;

    ptr = align64(ptr);
    m_sortScratch = reinterpret_cast<SortEntry*>(ptr);
    ptr += scratchBytes;

    ptr = align64(ptr);
    m_runs = reinterpret_cast<DrawRun*>(ptr);
    ptr += runBytes;

    ptr = align64(ptr);
    m_scissors = reinterpret_cast<Rect*>(ptr);
    ptr += scissorBytes;

    BeginFrame();
    return Ok;
}

void RenderQueue::Shutdown() {
    m_memoryBlock.reset();
    m_totalAllocatedBytes = 0;
    m_vertices = nullptr;
    m_indices = nullptr;
    m_sortedIndices = nullptr;
    m_drawItems = nullptr;
    m_sortEntries = nullptr;
    m_sortScratch = nullptr;
    m_runs = nullptr;
    m_scissors = nullptr;
    m_vertexCount = 0;
    m_indexCount = 0;
    m_itemCount = 0;
    m_runCount = 0;
    m_scissorCount = 1;
}

void RenderQueue::SortEntries() noexcept {
    if (m_itemCount <= 1) return;

    // Small arrays (<=32 items): use fast, cache-friendly insertion sort
    if (m_itemCount <= 32) {
        for (u32 i = 1; i < m_itemCount; ++i) {
            const SortEntry temp = m_sortEntries[i];
            u32 j = i;
            while (j > 0 && m_sortEntries[j - 1].key > temp.key) {
                m_sortEntries[j] = m_sortEntries[j - 1];
                --j;
            }
            m_sortEntries[j] = temp;
        }
        return;
    }

    // Large arrays: 8-bit Least Significant Digit (LSD) Radix Sort across bytes 1..7
    SortEntry* src = m_sortEntries;
    SortEntry* dst = m_sortScratch;

    for (u32 pass = 1; pass <= 7; ++pass) {
        const u32 shift = pass * 8;
        u32 counts[256] = {0};

        // Histogram step
        for (u32 i = 0; i < m_itemCount; ++i) {
            const u8 byteVal = static_cast<u8>((src[i].key >> shift) & 0xFF);
            counts[byteVal]++;
        }

        // Early-out if all keys share the exact same byte in this pass
        bool uniform = false;
        for (u32 b = 0; b < 256; ++b) {
            if (counts[b] == m_itemCount) {
                uniform = true;
                break;
            }
        }
        if (uniform) {
            continue;
        }

        // Prefix sum (scatter offsets)
        u32 offsets[256];
        offsets[0] = 0;
        for (u32 b = 1; b < 256; ++b) {
            offsets[b] = offsets[b - 1] + counts[b - 1];
        }

        // Scatter step
        for (u32 i = 0; i < m_itemCount; ++i) {
            const u8 byteVal = static_cast<u8>((src[i].key >> shift) & 0xFF);
            dst[offsets[byteVal]++] = src[i];
        }

        std::swap(src, dst);
    }

    // Ensure final sorted results reside in m_sortEntries
    if (src != m_sortEntries) {
        std::memcpy(m_sortEntries, src, m_itemCount * sizeof(SortEntry));
    }
}

void RenderQueue::MergeRuns() noexcept {
    m_runCount = 0;
    if (m_itemCount == 0) return;

    u32 currentSortedIndexOffset = 0;
    DrawRun* currentRun = nullptr;

    // Merge adjacent draw items with matching GPU pipeline state into single draw runs
    for (u32 i = 0; i < m_itemCount; ++i) {
        const u32 itemIdx = m_sortEntries[i].itemIndex;
        const DrawItem& item = m_drawItems[itemIdx];

        // Copy item indices into sorted indices arena
        std::memcpy(
            &m_sortedIndices[currentSortedIndexOffset],
            &m_indices[item.firstIndex],
            item.indexCount * sizeof(u32)
        );

        // Check compatibility for batch merging
        const bool canMerge = (currentRun != nullptr) &&
            (currentRun->textureSlot == item.textureSlot) &&
            (currentRun->blend == item.blend) &&
            (currentRun->material == item.material) &&
            (currentRun->scissorId == item.scissorId);

        if (canMerge) {
            currentRun->indexCount += item.indexCount;
            currentRun->vertexCount += item.vertexCount;
        } else {
            if (m_runCount >= m_cfg.maxRuns) {
                m_stats.droppedDraws++;
                continue;
            }
            // Start a new draw run
            currentRun = &m_runs[m_runCount++];
            currentRun->textureSlot = item.textureSlot;
            currentRun->blend = item.blend;
            currentRun->material = item.material;
            currentRun->scissorId = item.scissorId;
            currentRun->firstIndex = currentSortedIndexOffset;
            currentRun->indexCount = item.indexCount;
            currentRun->vertexOffset = item.vertexOffset;
            currentRun->vertexCount = item.vertexCount;
        }

        currentSortedIndexOffset += item.indexCount;
    }

    m_stats.runCount = m_runCount;
}

BatchView RenderQueue::Finalize() noexcept {
    SortEntries();
    MergeRuns();

    return BatchView{
        .vertices = std::span<const Vertex>(m_vertices, m_vertexCount),
        .indices  = std::span<const u32>(m_sortedIndices, m_indexCount),
        .runs     = std::span<const DrawRun>(m_runs, m_runCount),
        .scissors = std::span<const Rect>(m_scissors, m_scissorCount),
        .stats    = m_stats
    };
}

} // namespace EngineN
