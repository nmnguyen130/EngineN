#pragma once

#include "platform/PlatformTypes.h"
#include "renderer/RendererTypes.h"

#include <array>
#include <span>

namespace en::renderer {

// Pure C++ 2D command recorder, view-space frustum culler, 6-byte radix sorter, and vertex expander.
// Owned by Renderer or instantiated standalone in headless unit tests; main-thread use only.
// Invariant: Zero heap allocation after construction; excess submissions increment droppedQuads.
class RenderList {
public:
    static constexpr u32 kMaxQuads   = 65536;
    static constexpr u32 kMaxBatches = 4096;
    static constexpr u32 kMaxViews   = 16;
    static constexpr u32 kMaxPasses  = 16;
    static constexpr u32 kChunkQuads = 4096;

    // Pre-transformed view-space quad ready for branchless 4-vertex expansion (48 bytes).
    struct alignas(16) PreparedQuad {
        f32 cx{0.0f};
        f32 cy{0.0f};
        f32 hxX{0.0f};
        f32 hxY{0.0f};
        f32 hyX{0.0f};
        f32 hyY{0.0f};
        f32 u0{0.0f};
        f32 v0{0.0f};
        f32 u1{1.0f};
        f32 v1{1.0f};
        Color color{};
        u16 textureGen{1};
        u16 pad{0};
    };
    static_assert(sizeof(PreparedQuad) == 64);

    void Reset(platform::Extent2i defaultViewport) noexcept;

    void SetPass(u8 passId, const RenderPassDesc& desc) noexcept;
    void SetView(u8 viewSlot, const Camera2D& cam) noexcept;

    void PushQuad(const QuadCmd& cmd) noexcept;
    void PushRect(FRect rect, Color color, u8 passId = 0, u8 layer = 0, u16 depth = 32768, u8 viewSlot = 0) noexcept;
    void PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Color color, u8 passId = 0, u8 layer = 0, u16 depth = 32768, u8 viewSlot = 0) noexcept;

    // Compiles recorded keys via 6-byte LSD radix sort and coalesces state-identical runs into batches.
    void Compile() noexcept;

    // Expands a contiguous slice of sorted quads [firstQuad, firstQuad + quadCount) into 4*quadCount vertices.
    void ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept;

    // Computes a deterministic 64-bit FNV-1a hash of compiled batches and sorted quads for headless CI.
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    // Returns a compile-time/static u16 index buffer (0,1,2, 2,3,0...) for up to kChunkQuads quads.
    [[nodiscard]] static std::span<const u16> SharedQuadIndices(u32 quadCount) noexcept;

    [[nodiscard]] constexpr std::span<const DrawBatch> Batches() const noexcept {
        return {m_batches.data(), m_batchCount};
    }
    [[nodiscard]] constexpr std::span<const u64> SortedKeys() const noexcept {
        return {m_keys.data(), m_quadCount};
    }
    [[nodiscard]] constexpr const RenderPassDesc& Pass(u8 id) const noexcept { return m_passes[id & 0x0Fu]; }
    [[nodiscard]] constexpr const Camera2D& View(u8 slot) const noexcept { return m_views[slot & 0x0Fu]; }
    [[nodiscard]] constexpr const RenderStats& Stats() const noexcept { return m_stats; }

private:
    alignas(64) std::array<PreparedQuad, kMaxQuads> m_quads{};
    alignas(64) std::array<u64, kMaxQuads> m_keys{};
    alignas(64) std::array<u64, kMaxQuads> m_scratchKeys{};
    std::array<std::array<u32, 256>, 6> m_histograms{};

    std::array<DrawBatch, kMaxBatches> m_batches{};
    std::array<Camera2D, kMaxViews> m_views{};
    std::array<RenderPassDesc, kMaxPasses> m_passes{};

    u32 m_quadCount{0};
    u32 m_batchCount{0};
    u64 m_prevKey{0};
    bool m_isMonotonic{true};
    bool m_compiled{false};
    RenderStats m_stats{};
};

} // namespace en::renderer