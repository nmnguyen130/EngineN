#pragma once

#include "renderer/RendererTypes.h"
#include "renderer/Texture.h"
#include "renderer/RenderBackend.h"

#include <cstring>
#include <memory>

namespace EngineN {

struct RenderQueueConfig {
    usize maxVertices = 262'144; // ~5.2 MB (20 bytes per vertex)
    usize maxIndices  = 393'216; // ~1.5 MB (4 bytes per index)
    usize maxItems    = 65'536;  // ~1.0 MB (draw commands)
    usize maxRuns     = 4'096;   // ~128 KB (merged draw batches)
    usize maxScissors = 256;     // Unique scissor clip rectangles
};

struct DrawItem {
    u64       sortKey = 0;
    u16       textureSlot = 0;
    BlendMode blend = BlendMode::Premultiplied;
    u8        material = 0;
    u16       scissorId = 0;
    u32       vertexOffset = 0;
    u32       vertexCount = 0;
    u32       firstIndex = 0;
    u32       indexCount = 0;
};

struct SortEntry {
    u64 key = 0;
    u32 itemIndex = 0;
};

struct VertexReservation {
    Vertex* vertices = nullptr;
    u32*    indices = nullptr;
    u32     vertexOffset = 0;
    u32     firstIndex = 0;
    bool    isValid = false;
};

/// High-throughput zero-allocation batch queue.
/// Manages pre-allocated CPU arenas, stable 64-bit LSD Radix Sort, and run merging.
class RenderQueue {
public:
    RenderQueue() = default;
    ~RenderQueue() { Shutdown(); }

    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;
    RenderQueue(RenderQueue&& other) noexcept;
    RenderQueue& operator=(RenderQueue&& other) noexcept;

    [[nodiscard]] Result<void> Init(const RenderQueueConfig& cfg = {});
    void Shutdown();

    void BeginFrame() noexcept {
        m_vertexCount = 0;
        m_indexCount = 0;
        m_itemCount = 0;
        m_runCount = 0;
        m_scissorCount = 1;
        if (m_scissors) {
            m_scissors[0] = {0, 0, 0, 0}; // 0 = Disabled
        }
        m_stats = {};
    }

    [[nodiscard]] VertexReservation Reserve(u32 vCount, u32 iCount) noexcept {
        if (m_vertexCount + vCount > m_cfg.maxVertices ||
            m_indexCount + iCount > m_cfg.maxIndices ||
            m_itemCount >= m_cfg.maxItems)
        {
            m_stats.droppedDraws++;
            return {
                .vertices = nullptr,
                .indices = nullptr,
                .vertexOffset = 0,
                .firstIndex = 0,
                .isValid = false
            };
        }

        return {
            .vertices = &m_vertices[m_vertexCount],
            .indices = &m_indices[m_indexCount],
            .vertexOffset = m_vertexCount,
            .firstIndex = m_indexCount,
            .isValid = true
        };
    }

    void Commit(
        const VertexReservation& res,
        u64 sortKey,
        u16 scissorId,
        u32 vCount,
        u32 iCount) noexcept
    {
        if (!res.isValid) return;

        for (u32 i = 0; i < iCount; ++i) {
            m_indices[res.firstIndex + i] += res.vertexOffset;
        }

        m_vertexCount += vCount;
        m_indexCount += iCount;

        DrawItem& item = m_drawItems[m_itemCount];
        item.sortKey = sortKey;
        item.textureSlot = SortKey::UnpackTextureSlot(sortKey);
        item.blend = SortKey::UnpackBlend(sortKey);
        item.material = SortKey::UnpackMaterial(sortKey);
        item.scissorId = scissorId;
        item.vertexOffset = res.vertexOffset;
        item.vertexCount = vCount;
        item.firstIndex = res.firstIndex;
        item.indexCount = iCount;

        m_sortEntries[m_itemCount] = { .key = sortKey, .itemIndex = m_itemCount };
        m_itemCount++;

        m_stats.drawCount++;
        m_stats.vertexCount = m_vertexCount;
        m_stats.indexCount = m_indexCount;
    }

    [[nodiscard]] u16 InternScissor(const Rect& scissor) noexcept {
        if (scissor.IsEmpty()) return 0;

        for (u16 i = 1; i < static_cast<u16>(m_scissorCount); ++i) {
            if (m_scissors[i] == scissor) {
                return i;
            }
        }

        if (m_scissorCount < m_cfg.maxScissors) {
            const u16 newId = static_cast<u16>(m_scissorCount++);
            m_scissors[newId] = scissor;
            return newId;
        }

        return 0;
    }

    [[nodiscard]] BatchView Finalize() noexcept;

    [[nodiscard]] const FrameStats& GetStats() const noexcept { return m_stats; }
    [[nodiscard]] const RenderQueueConfig& GetConfig() const noexcept { return m_cfg; }
    [[nodiscard]] std::span<const SortEntry> GetSortedEntries() const noexcept { return {m_sortEntries, m_itemCount}; }

private:
    void SortEntries() noexcept;
    void MergeRuns() noexcept;

    RenderQueueConfig m_cfg{};

    std::unique_ptr<u8[]> m_memoryBlock;
    usize m_totalAllocatedBytes = 0;

    Vertex*    m_vertices = nullptr;
    u32*       m_indices = nullptr;
    u32*       m_sortedIndices = nullptr;
    DrawItem*  m_drawItems = nullptr;
    SortEntry* m_sortEntries = nullptr;
    SortEntry* m_sortScratch = nullptr;
    DrawRun*   m_runs = nullptr;
    Rect*      m_scissors = nullptr;

    u32 m_vertexCount = 0;
    u32 m_indexCount = 0;
    u32 m_itemCount = 0;
    u32 m_runCount = 0;
    u32 m_scissorCount = 1;

    FrameStats m_stats{};
};

} // namespace EngineN
