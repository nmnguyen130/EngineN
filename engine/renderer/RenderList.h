#pragma once

#include "renderer/RendererTypes.h"

#include <array>
#include <span>

namespace en::renderer {

// Pure C++23 2D command recorder, view-space frustum culler, 6-byte radix sorter, and vertex expander.
// Owned by Renderer or instantiated standalone in headless unit tests; main-thread use only.
// Invariant: Zero heap allocation after construction; hot control state resides in Cache Line 0.
class alignas(64) RenderList {
public:
    // Sized to the 16-bit GPU index limit (16384 quads = 65536 vertices, ~1.26 MB total L2/L3 footprint).
    static constexpr u32 kMaxQuads   = 16384;
    static constexpr u32 kMaxBatches = 1024;
    static constexpr u32 kMaxViews   = 16;
    static constexpr u32 kMaxPasses  = 16;
    static constexpr u32 kChunkQuads = 2048;

    // Pre-transformed view-space quad occupying one 64-byte cache line with zero implicit padding.
    // Stores pre-summed corner offset vectors (d0 = hx + hy, d1 = hx - hy) for 1-op vertex expansion.
    struct alignas(64) PreparedQuad {
        f32 cx{0.0f};
        f32 cy{0.0f};
        f32 d0X{0.0f};
        f32 d0Y{0.0f};
        f32 d1X{0.0f};
        f32 d1Y{0.0f};
        f32 u0{0.0f};
        f32 v0{0.0f};
        f32 u1{1.0f};
        f32 v1{1.0f};
        Color color{};
        u16 textureGen{1};
        u16 reserved0{0};
        u32 reserved1{0};
    };
    static_assert(sizeof(PreparedQuad) == 64 && alignof(PreparedQuad) == 64);

    void Reset(ViewportExtent size) noexcept;

    constexpr void SetPass(u8 passId, const RenderPassDesc& desc) noexcept {
        const u8 idx  = passId & 0x0Fu;
        const u16 bit = static_cast<u16>(1u << idx);
        m_passes[idx] = desc;
        m_enabledPassMask = desc.enabled
            ? static_cast<u16>(m_enabledPassMask | bit)
            : static_cast<u16>(m_enabledPassMask & ~bit);
        m_clearPassMask = (desc.enabled && desc.clear)
            ? static_cast<u16>(m_clearPassMask | bit)
            : static_cast<u16>(m_clearPassMask & ~bit);
    }

    constexpr void SetView(u8 viewSlot, const Camera2D& cam) noexcept {
        m_views[viewSlot & 0x0Fu] = cam;
    }

    void PushQuad(const QuadCmd& cmd) noexcept;

    void PushRect(
        FRect rect,
        Color color,
        u8 passId = 0,
        u8 layer = 0,
        u16 depth = 32768,
        u8 viewSlot = 0) noexcept {
        PushQuad(QuadCmd{
            .x        = rect.x + rect.w * 0.5f,
            .y        = rect.y + rect.h * 0.5f,
            .w        = rect.w,
            .h        = rect.h,
            .color    = color,
            .texture  = kWhiteTexture,
            .depth    = depth,
            .passId   = passId,
            .layer    = layer,
            .viewSlot = viewSlot
        });
    }

    void PushLine(
        f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Color color,
        u8 passId = 0, u8 layer = 0, u16 depth = 32768, u8 viewSlot = 0) noexcept;

    void Compile() noexcept;
    void ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;
    [[nodiscard]] static std::span<const u16> SharedQuadIndices(u32 quadCount) noexcept;

    [[nodiscard]] constexpr std::span<const DrawBatch> Batches() const noexcept {
        return {m_batches.data(), m_stats.drawBatches};
    }
    [[nodiscard]] constexpr std::span<const u64> SortedKeys() const noexcept {
        return {m_keys.data(), m_stats.submittedQuads};
    }
    [[nodiscard]] constexpr u16 ClearPassMask() const noexcept { return m_clearPassMask; }
    [[nodiscard]] constexpr const RenderPassDesc& Pass(u8 id) const noexcept { return m_passes[id & 0x0Fu]; }
    [[nodiscard]] constexpr const Camera2D& View(u8 slot) const noexcept { return m_views[slot & 0x0Fu]; }
    [[nodiscard]] constexpr const RenderStats& Stats() const noexcept { return m_stats; }

private:
    // Cache Line 0 (offset 0..63): Hot per-quad recording counters, key variance tracker, and pass bitmasks.
    u64 m_prevKey{0};
    u64 m_varyingBits{0};
    RenderStats m_stats{};
    u16 m_enabledPassMask{0x0001u};
    u16 m_clearPassMask{0x0001u};
    bool m_isMonotonic{true};
    bool m_compiled{false};

    alignas(64) std::array<Camera2D, kMaxViews> m_views{};
    std::array<RenderPassDesc, kMaxPasses> m_passes{};

    alignas(64) std::array<u64, kMaxQuads> m_keys{};
    alignas(64) std::array<u64, kMaxQuads> m_scratchKeys{};
    alignas(64) std::array<DrawBatch, kMaxBatches> m_batches{};
    alignas(64) std::array<PreparedQuad, kMaxQuads> m_quads{};
};

} // namespace en::renderer
