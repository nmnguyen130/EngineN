#pragma once

#include "core/Memory.h"
#include "renderer/RendererTypes.h"

namespace en {

// Pure C++23 command buffer, frustum culler, active-byte radix sorter, and procedural geometry builder.
// Move-only RAII owner of a single 64B-aligned arena slab; 100% deterministic in headless CI.
// Invariant: Compile() orders m_keys monotonically and coalesces state-identical runs into DrawBatches.
class alignas(64) RenderList {
public:
    static constexpr u32 kMaxQuads = 16384;
    static constexpr u32 kMaxBatches = 1024;
    static constexpr u32 kMaxPasses = 16;
    static constexpr u32 kMaxViews = 16;

    RenderList() noexcept = default;
    ~RenderList() noexcept { Shutdown(); }

    RenderList(const RenderList&) = delete;
    RenderList& operator=(const RenderList&) = delete;

    [[nodiscard]] bool Init(usize arenaBytes = 2 * 1024 * 1024) noexcept;
    void Shutdown() noexcept;

    void Reset(ViewportExtent viewportSize = {1280, 720}) noexcept;

    constexpr void SetPass(u8 passId, const RenderPassDesc& desc) noexcept {
        if (passId < kMaxPasses) m_passes[passId] = desc;
    }
    static constexpr u32 kMaxMaterials = 32;
    static constexpr u32 kMaxOccluders = 32;

    constexpr void SetView(u8 viewSlot, const Camera2D& cam) noexcept {
        if (viewSlot < kMaxViews) m_views[viewSlot] = cam;
    }
    constexpr void SetMaterial(u8 materialId, const ShaderParamBlock& block) noexcept {
        if (materialId < kMaxMaterials) m_materials[materialId] = block;
    }
    [[nodiscard]] constexpr const ShaderParamBlock& Material(u8 materialId) const noexcept {
        return m_materials[materialId % kMaxMaterials];
    }

    void ClearOccluders2D() noexcept { m_occluderCount = 0; }
    bool AddOccluder2D(Rect worldOccluder) noexcept {
        if (m_occluderCount >= kMaxOccluders) return false;
        m_occluders[m_occluderCount++] = worldOccluder;
        return true;
    }
    [[nodiscard]] bool IsOccluded2D(Rect worldBounds) const noexcept;

    void PushQuad(const QuadCmd& cmd) noexcept;
    void PushRect(Rect rect, Color color, u8 passId = 0, u8 layer = 10, u16 depth = 32768, u8 viewSlot = 0, BlendMode blend = BlendMode::Alpha) noexcept;
    void PushLine(Vec2 a, Vec2 b, f32 thickness, Color color, u8 passId = 0, u8 layer = 10, u16 depth = 32768, u8 viewSlot = 0) noexcept;
    void PushCircleOutline(Vec2 center, f32 radius, u32 segments, f32 thickness, Color color, u8 passId = 0, u8 layer = 10, u8 viewSlot = 0) noexcept;
    void PushSlashArc(Vec2 center, f32 radius, f32 startAngleRad, f32 sweepAngleRad, f32 thickness, Color color, u8 passId = 0, u8 layer = 15, u8 viewSlot = 0) noexcept;
    void PushBeam(Vec2 start, Vec2 end, f32 width, Color coreColor, Color glowColor, u8 passId = 0, u8 layer = 15, u8 viewSlot = 0) noexcept;
    void PushRing(Vec2 center, f32 innerRadius, f32 outerRadius, Color color, u8 passId = 0, u8 layer = 14, u8 viewSlot = 0) noexcept;
    void PushNineSlice(const NineSliceDesc& desc, u8 passId = 0, u8 layer = 20, u8 viewSlot = 0) noexcept;
    void PushProgressBar(Rect rect, f32 ratio, Color fillColor, Color backColor, Color borderColor, u8 passId = 0, u8 layer = 25, u8 viewSlot = 0) noexcept;
    void PushText(Vec2 pos, std::string_view text, Color color, f32 scale = 1.0f, u8 passId = 0, u8 layer = 30, u8 viewSlot = 0, GpuTextureHandle fontTex = kWhiteGpuTexture) noexcept;

    void Compile() noexcept;
    void ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept;

    [[nodiscard]] constexpr ViewportExtent Viewport() const noexcept { return m_viewport; }
    [[nodiscard]] constexpr const RenderStats& Stats() const noexcept { return m_stats; }
    [[nodiscard]] constexpr std::span<const DrawBatch> Batches() const noexcept {
        return m_batches.subspan(0, m_stats.drawBatches);
    }
    [[nodiscard]] constexpr const Camera2D& View(u8 slot) const noexcept { return m_views[slot & 0x0FU]; }
    [[nodiscard]] constexpr const RenderPassDesc& Pass(u8 passId) const noexcept { return m_passes[passId & 0x0FU]; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    u64 m_prevKey{0};
    u64 m_varyingBits{0};
    RenderStats m_stats{};
    ViewportExtent m_viewport{1280, 720};
    bool m_isMonotonic{true};
    bool m_compiled{false};

    std::span<u64> m_keys{};
    std::span<u64> m_scratchKeys{};
    std::span<PreparedQuad> m_quads{};
    std::span<DrawBatch> m_batches{};
    std::array<RenderPassDesc, kMaxPasses> m_passes{};
    std::array<Camera2D, kMaxViews> m_views{};
    std::array<ShaderParamBlock, kMaxMaterials> m_materials{};
    std::array<Rect, kMaxOccluders> m_occluders{};
    u32 m_occluderCount{0};
    AlignedArena m_arena{};
};

} // namespace en
