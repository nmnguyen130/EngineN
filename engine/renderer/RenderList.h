#pragma once

#include "renderer/RendererTypes.h"

namespace en {

// Pure C++23 command buffer, rotated-AABB frustum culler, single-pass active-byte radix sorter, and geometry builder.
// Value type with L2/L3-resident fixed storage (sizeof == 1,328,256 B, 20,754 cache lines exact) and Cache Line 0 hot state.
// Invariant: Compile() orders m_keys monotonically and coalesces state-identical runs into minimal DrawBatches.
class alignas(64) RenderList {
public:
    static constexpr u32 kMaxQuads = 16384;
    static constexpr u32 kMaxBatches = 1024;
    static constexpr u32 kChunkQuads = 2048;
    static constexpr u32 kMaxPasses = 16;
    static constexpr u32 kMaxViews = 16;
    static constexpr u8 kFirstClipViewSlot = 8;
    static constexpr u8 kMaxClipViewSlot = 14;
    static constexpr u8 kHudViewSlot = 15;

    constexpr RenderList() noexcept { Reset({1280, 720}); }

    constexpr void Reset(ViewportExtent viewportSize = {1280, 720}) noexcept {
        if (viewportSize.IsValid()) {
            m_viewport = viewportSize;
        }
        m_prevKey = 0ULL;
        m_varyingBits = 0ULL;
        m_stats = RenderStats{};
        m_enabledPassMask = 0xFFFFU;
        m_clearPassMask = 0x0001U;
        m_activePassMask = 0U;
        m_clipViewStack.fill(kHudViewSlot);
        m_clipStackDepth = 0U;
        m_nextClipViewSlot = kFirstClipViewSlot;
        m_activeClipView = kHudViewSlot;
        m_isMonotonic = true;
        m_compiled = false;
        m_pixelSnap = false;

        for (u8 i = 0U; i < kMaxPasses; ++i) {
            m_passes[i] = RenderPassDesc{};
            m_passes[i].clear = (i == 0U);
        }
        for (u8 i = 0U; i < kMaxViews; ++i) {
            m_views[i] = Camera2D{};
            m_views[i].screenSpace = (i >= kFirstClipViewSlot);
        }
    }

    constexpr void SetPass(u8 passId, const RenderPassDesc& desc) noexcept {
        if (passId >= kMaxPasses) return;
        m_passes[passId] = desc;
        const u16 bit = static_cast<u16>(1U << passId);
        if (desc.enabled) {
            m_enabledPassMask |= bit;
        } else {
            m_enabledPassMask &= static_cast<u16>(~bit);
        }
        if (desc.enabled && desc.clear) {
            m_clearPassMask |= bit;
        } else {
            m_clearPassMask &= static_cast<u16>(~bit);
        }
    }

    constexpr void SetView(u8 viewSlot, const Camera2D& cam) noexcept {
        if (viewSlot < kMaxViews) {
            m_views[viewSlot] = cam;
        }
    }

    constexpr void SetScissor(u8 viewSlot, Rect normalizedScissor) noexcept {
        if (viewSlot < kMaxViews) {
            m_views[viewSlot].scissor = normalizedScissor;
        }
    }

    constexpr void SetPixelScissor(u8 viewSlot, Rect pixelScissor) noexcept {
        if (viewSlot >= kMaxViews || !m_viewport.IsValid()) return;
        const Camera2D& cam = m_views[viewSlot];
        const f32 vpW = std::max(1.0f, cam.viewport.w * static_cast<f32>(m_viewport.w));
        const f32 vpH = std::max(1.0f, cam.viewport.h * static_cast<f32>(m_viewport.h));
        m_views[viewSlot].scissor = Rect{
            std::clamp(pixelScissor.x / vpW, 0.0f, 1.0f),
            std::clamp(pixelScissor.y / vpH, 0.0f, 1.0f),
            std::clamp(pixelScissor.w / vpW, 0.0f, 1.0f),
            std::clamp(pixelScissor.h / vpH, 0.0f, 1.0f),
        };
    }

    // Pushes a hierarchical pixel-space scissor clip rect (intersecting with any active parent clip)
    // and returns the assigned viewSlot for subsequent UI draw calls.
    constexpr u8 PushClipRect(Rect pixelClip, u8 baseViewSlot = kHudViewSlot) noexcept {
        if (m_clipStackDepth >= m_clipViewStack.size() || m_nextClipViewSlot > kMaxClipViewSlot) {
            return m_activeClipView;
        }
        const u8 parentSlot = (m_clipStackDepth > 0U) ? m_activeClipView : (baseViewSlot & 0x0FU);
        m_clipViewStack[m_clipStackDepth++] = m_activeClipView;

        const u8 slot = m_nextClipViewSlot++;
        m_views[slot] = m_views[parentSlot];
        SetPixelScissor(slot, pixelClip);
        if (m_clipStackDepth > 1U) {
            m_views[slot].scissor = m_views[parentSlot].scissor.Intersection(m_views[slot].scissor);
        }
        m_activeClipView = slot;
        ++m_stats.activeClipViews;
        return slot;
    }

    constexpr void PopClipRect() noexcept {
        if (m_clipStackDepth > 0U) {
            m_activeClipView = m_clipViewStack[--m_clipStackDepth];
        }
    }

    [[nodiscard]] constexpr u8 ActiveClipViewSlot() const noexcept { return m_activeClipView; }

    constexpr void SetPixelSnap(bool enabled) noexcept { m_pixelSnap = enabled; }
    [[nodiscard]] constexpr bool PixelSnap() const noexcept { return m_pixelSnap; }

    void PushQuad(const QuadCmd& cmd) noexcept;

    // Emits a sprite quad plus an optional additive hit-flash overlay at depthBias + 1 when flashRatio > 0.
    void PushHitFlashQuad(
        const QuadCmd& baseCmd,
        f32 flashRatio,
        Color flashTint = Color{1.0f, 1.0f, 1.0f, 1.0f}) noexcept;

    void PushRect(
        Rect rect,
        Color color,
        u8 passId = 0,
        u8 layer = 10,
        u16 depth = 32768,
        u8 viewSlot = 0,
        BlendMode blend = BlendMode::Alpha) noexcept;

    // Emits a single-quad 2-color Gouraud gradient rectangle (vertical top->bottom or horizontal left->right).
    void PushGradientRect(
        Rect rect,
        Color startColor,
        Color endColor,
        bool vertical = true,
        u8 passId = 0,
        u8 layer = 10,
        u16 depth = 32768,
        u8 viewSlot = 0,
        BlendMode blend = BlendMode::Alpha) noexcept;

    // Subdivides rect into repeating tileSize quads for parallax backgrounds, chains, or hazard stripes.
    void PushTiledRect(
        Rect rect,
        TextureHandle texture,
        Vec2 tileSize,
        Rect uvRect = {0.0f, 0.0f, 1.0f, 1.0f},
        Color color = {1.0f, 1.0f, 1.0f, 1.0f},
        u8 passId = 0,
        u8 layer = 5,
        u16 depth = 32768,
        u8 viewSlot = 0) noexcept;

    void PushRectOutline(
        Rect rect,
        f32 thickness,
        Color color,
        u8 passId = 0,
        u8 layer = 10,
        u16 depth = 32768,
        u8 viewSlot = 0) noexcept;

    void PushLine(
        f32 x0,
        f32 y0,
        f32 x1,
        f32 y1,
        f32 thickness,
        Color color,
        u8 passId = 0,
        u8 layer = 10,
        u16 depth = 32768,
        u8 viewSlot = 0) noexcept;

    void PushLine(
        Vec2 a,
        Vec2 b,
        f32 thickness,
        Color color,
        u8 passId = 0,
        u8 layer = 10,
        u16 depth = 32768,
        u8 viewSlot = 0) noexcept {
        PushLine(a.x, a.y, b.x, b.y, thickness, color, passId, layer, depth, viewSlot);
    }

    void PushPolyline(
        std::span<const Vec2> points,
        f32 thickness,
        Color color,
        bool closed = false,
        u8 passId = 0,
        u8 layer = 10,
        u16 depth = 32768,
        u8 viewSlot = 0) noexcept;

    void PushCircleOutline(
        Vec2 center,
        f32 radius,
        u32 segments,
        f32 thickness,
        Color color,
        u8 passId = 0,
        u8 layer = 10,
        u8 viewSlot = 0) noexcept;

    void PushSlashArc(
        Vec2 center,
        f32 radius,
        f32 startAngleRad,
        f32 sweepAngleRad,
        f32 thickness,
        Color color,
        u8 passId = 0,
        u8 layer = 15,
        u8 viewSlot = 0) noexcept;

    void PushTaperedArc(
        Vec2 center,
        f32 radius,
        f32 startAngleRad,
        f32 sweepAngleRad,
        f32 thickness,
        Color color,
        u8 passId = 0,
        u8 layer = 15,
        u8 viewSlot = 0) noexcept {
        PushSlashArc(center, radius, startAngleRad, sweepAngleRad, thickness, color, passId, layer, viewSlot);
    }

    void PushBeam(
        Vec2 start,
        Vec2 end,
        f32 width,
        Color coreColor,
        Color glowColor,
        u8 passId = 0,
        u8 layer = 15,
        u8 viewSlot = 0) noexcept;

    void PushRing(
        Vec2 center,
        f32 innerRadius,
        f32 outerRadius,
        Color color,
        u8 passId = 0,
        u8 layer = 14,
        u8 viewSlot = 0) noexcept;

    void PushPointLight(
        Vec2 worldPos,
        f32 radius,
        Color color,
        f32 intensity = 1.0f,
        u8 passId = 0,
        u8 layer = 40,
        u8 viewSlot = 0) noexcept;

    // Emits a true 3x3 textured 9-slice patch grid with proportional border clamping and UV slicing.
    void PushNineSlice(
        const NineSliceDesc& desc,
        u8 passId = 0,
        u8 layer = 20,
        u8 viewSlot = 0) noexcept;

    void PushProgressBar(
        Rect rect,
        f32 ratio,
        Color fillColor,
        Color backColor,
        Color borderColor,
        u8 passId = 0,
        u8 layer = 25,
        u8 viewSlot = 0) noexcept;

    void PushText(
        Vec2 pos,
        std::string_view text,
        Color color,
        f32 scale = 1.0f,
        u8 passId = 0,
        u8 layer = 30,
        u8 viewSlot = 0,
        TextureHandle fontTex = kBuiltinFontTexture) noexcept;

    [[nodiscard]] static constexpr Vec2 MeasureText(std::string_view text, f32 scale = 1.0f) noexcept {
        if (text.empty()) return {0.0f, 0.0f};
        const f32 quadSize = 16.0f * scale;
        const f32 advanceX = 12.0f * scale;
        const f32 lineStep = quadSize * 1.15f;
        f32 maxW = 0.0f;
        f32 curW = 0.0f;
        u32 lines = 1U;
        for (const char c : text) {
            if (c == '\n') {
                maxW = std::max(maxW, curW);
                curW = 0.0f;
                ++lines;
            } else {
                curW += advanceX;
            }
        }
        maxW = std::max(maxW, curW);
        return {maxW, static_cast<f32>(lines - 1U) * lineStep + quadSize};
    }

    [[nodiscard]] static u32 ComputeShadowVisibilityPolygon(
        Vec2 lightPos,
        f32 lightRadius,
        std::span<const ShadowOccluder2D> occluders,
        std::span<Vec2> outPolygonVertices) noexcept;

    void Compile() noexcept;

    void ExpandVertices(
        u32 firstQuad,
        u32 quadCount,
        std::span<Vertex2D> outVertices) const noexcept;

    [[nodiscard]] static std::span<const u16> SharedQuadIndices(u32 quadCount) noexcept;

    [[nodiscard]] constexpr ViewportExtent Viewport() const noexcept { return m_viewport; }
    [[nodiscard]] constexpr const RenderStats& Stats() const noexcept { return m_stats; }
    [[nodiscard]] constexpr u16 EnabledPassMask() const noexcept { return m_enabledPassMask; }
    [[nodiscard]] constexpr u16 ClearPassMask() const noexcept { return m_clearPassMask; }
    [[nodiscard]] constexpr u16 ActivePassMask() const noexcept { return m_activePassMask; }

    [[nodiscard]] constexpr std::span<const DrawBatch> Batches() const noexcept {
        return std::span<const DrawBatch>(m_batches.data(), m_stats.drawBatches);
    }

    [[nodiscard]] constexpr std::span<const u64> SortedKeys() const noexcept {
        return std::span<const u64>(m_keys.data(), m_stats.submittedQuads);
    }

    [[nodiscard]] constexpr const RenderPassDesc& Pass(u8 passId) const noexcept {
        return m_passes[passId & 0x0FU];
    }

    [[nodiscard]] constexpr const Camera2D& View(u8 viewSlot) const noexcept {
        return m_views[viewSlot & 0x0FU];
    }

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    // Cache Line 0 (offset 0..63, 64B exact, 0B padding, 0 dead variables)
    u64 m_prevKey{0};
    u64 m_varyingBits{0};
    ViewportExtent m_viewport{1280, 720};
    RenderStats m_stats{};
    u16 m_enabledPassMask{0xFFFFU};
    u16 m_clearPassMask{0x0001U};
    u16 m_activePassMask{0};
    std::array<u8, 8> m_clipViewStack{};
    u8 m_clipStackDepth{0};
    u8 m_nextClipViewSlot{kFirstClipViewSlot};
    u8 m_activeClipView{kHudViewSlot};
    bool m_isMonotonic{true};
    bool m_compiled{false};
    bool m_pixelSnap{false};

    // Cache-line aligned tables (offset 64..1,328,255, 0B padding)
    alignas(64) std::array<RenderPassDesc, kMaxPasses> m_passes{};
    alignas(64) std::array<Camera2D, kMaxViews> m_views{};
    alignas(64) std::array<DrawBatch, kMaxBatches> m_batches{};
    alignas(64) std::array<u64, kMaxQuads> m_keys{};
    alignas(64) std::array<u64, kMaxQuads> m_scratchKeys{};
    alignas(64) std::array<PreparedQuad, kMaxQuads> m_quads{};
};
static_assert(sizeof(RenderList) == 1328256);

} // namespace en
