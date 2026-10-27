#pragma once

#include "renderer/RendererTypes.h"

namespace en {

// 2D quad command buffer, frustum culler, in-place sorter, and geometry batcher.
// Self-contained value type with fixed L2-resident inline storage (282 KB).
// Invariant: Compile() orders m_keys monotonically and coalesces state-identical runs into DrawBatches.
class alignas(64) RenderList {
public:
    static constexpr u32 kMaxQuads = 4096;
    static constexpr u32 kMaxBatches = 256;
    static constexpr u32 kChunkQuads = 512;
    static constexpr u8 kMaxPasses = 4;
    static constexpr u8 kMaxViews = 8;
    static constexpr u8 kFirstClipViewSlot = 4;
    static constexpr u8 kMaxClipViewSlot = 6;
    static constexpr u8 kHudViewSlot = 7;

    constexpr RenderList() noexcept { Reset({1280, 720}); }

    constexpr void Reset(ViewportExtent viewportSize = {1280, 720}) noexcept {
        if (viewportSize.IsValid()) m_viewport = viewportSize;
        m_prevKey = 0ULL;
        m_stats = RenderStats{};
        m_clipViewStack.fill(kHudViewSlot);
        m_clipStackDepth = 0U;
        m_nextClipViewSlot = kFirstClipViewSlot;
        m_activeClipView = kHudViewSlot;
        m_enabledPassMask = 0x0FU;
        m_clearPassMask = 0x01U;
        m_isMonotonic = true;
        m_compiled = false;
        m_pixelSnap = false;

        for (u8 i = 0U; i < kMaxPasses; ++i) {
            m_passes[i] = RenderPassDesc{.clear = (i == 0U)};
        }
        for (u8 i = 0U; i < kMaxViews; ++i) {
            m_views[i] = Camera2D{.screenSpace = (i >= kFirstClipViewSlot)};
        }
    }

    constexpr void SetPass(u8 passId, const RenderPassDesc& desc) noexcept {
        if (passId >= kMaxPasses) return;
        m_passes[passId] = desc;
        const u8 bit = static_cast<u8>(1U << passId);
        if (desc.enabled) m_enabledPassMask |= bit;
        else m_enabledPassMask &= static_cast<u8>(~bit);

        if (desc.enabled && desc.clear) m_clearPassMask |= bit;
        else m_clearPassMask &= static_cast<u8>(~bit);
    }

    constexpr void SetView(u8 viewSlot, const Camera2D& cam) noexcept {
        if (viewSlot < kMaxViews) m_views[viewSlot] = cam;
    }

    constexpr void SetScissor(u8 viewSlot, Rect normalizedScissor) noexcept {
        if (viewSlot < kMaxViews) m_views[viewSlot].scissor = normalizedScissor;
    }

    constexpr void SetPixelScissor(u8 viewSlot, Rect pixelScissor) noexcept {
        if (viewSlot >= kMaxViews || !m_viewport.IsValid()) return;
        const Camera2D& cam = m_views[viewSlot];
        const f32 vpX = cam.viewport.x * static_cast<f32>(m_viewport.w);
        const f32 vpY = cam.viewport.y * static_cast<f32>(m_viewport.h);
        const f32 vpW = std::max(1.0f, cam.viewport.w * static_cast<f32>(m_viewport.w));
        const f32 vpH = std::max(1.0f, cam.viewport.h * static_cast<f32>(m_viewport.h));
        m_views[viewSlot].scissor = Rect{
            std::clamp((pixelScissor.x - vpX) / vpW, 0.0f, 1.0f),
            std::clamp((pixelScissor.y - vpY) / vpH, 0.0f, 1.0f),
            std::clamp(pixelScissor.w / vpW, 0.0f, 1.0f),
            std::clamp(pixelScissor.h / vpH, 0.0f, 1.0f),
        };
    }

    constexpr u8 PushClipRect(Rect pixelClip, u8 baseViewSlot = kHudViewSlot) noexcept {
        if (m_clipStackDepth >= m_clipViewStack.size() || m_nextClipViewSlot > kMaxClipViewSlot) {
            return m_activeClipView;
        }
        const u8 parentSlot = (m_clipStackDepth > 0U) ? m_activeClipView : (baseViewSlot & 0x07U);
        m_clipViewStack[m_clipStackDepth++] = m_activeClipView;

        const u8 slot = m_nextClipViewSlot++;
        m_views[slot] = m_views[parentSlot];
        SetPixelScissor(slot, pixelClip);
        if (m_clipStackDepth > 1U) {
            m_views[slot].scissor = m_views[parentSlot].scissor.Intersection(m_views[slot].scissor);
        }
        m_activeClipView = slot;
        return slot;
    }

    constexpr void PopClipRect() noexcept {
        if (m_clipStackDepth > 0U) m_activeClipView = m_clipViewStack[--m_clipStackDepth];
    }

    [[nodiscard]] constexpr u8 ActiveClipViewSlot() const noexcept { return m_activeClipView; }

    constexpr void SetPixelSnap(bool enabled) noexcept { m_pixelSnap = enabled; }
    [[nodiscard]] constexpr bool PixelSnap() const noexcept { return m_pixelSnap; }

    void PushQuad(const QuadCmd& cmd) noexcept;

    void PushRect(
        Rect rect, Color color, u8 passId = 0, u8 layer = 10,
        u16 depth = 32768, u8 viewSlot = 0, BlendMode blend = BlendMode::Alpha) noexcept;

    void PushTiledRect(
        Rect rect, TextureHandle texture, Vec2 tileSize,
        Rect uvRect = {0.0f, 0.0f, 1.0f, 1.0f}, Color color = {1.0f, 1.0f, 1.0f, 1.0f},
        u8 passId = 0, u8 layer = 5, u16 depth = 32768, u8 viewSlot = 0) noexcept;

    void PushRectOutline(
        Rect rect, f32 thickness, Color color,
        u8 passId = 0, u8 layer = 10, u16 depth = 32768, u8 viewSlot = 0) noexcept;

    void PushLine(
        Vec2 a, Vec2 b, f32 thickness, Color color,
        u8 passId = 0, u8 layer = 10, u16 depth = 32768, u8 viewSlot = 0) noexcept;

    void PushCircleOutline(
        Vec2 center, f32 radius, u32 segments, f32 thickness, Color color,
        u8 passId = 0, u8 layer = 10, u8 viewSlot = 0) noexcept;

    void PushNineSlice(
        const NineSliceDesc& desc, u8 passId = 0, u8 layer = 20, u8 viewSlot = 0) noexcept;

    void PushText(
        Vec2 pos, std::string_view text, Color color, f32 scale = 1.0f,
        u8 passId = 0, u8 layer = 30, u8 viewSlot = 0, TextureHandle fontTex = {}) noexcept;

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
            } else if ((static_cast<u8>(c) & 0xC0U) != 0x80U) {
                curW += advanceX;
            }
        }
        return {std::max(maxW, curW), static_cast<f32>(lines - 1U) * lineStep + quadSize};
    }

    void Compile() noexcept;

    void ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept;

    [[nodiscard]] static std::span<const u16> SharedQuadIndices(u32 quadCount) noexcept;

    [[nodiscard]] constexpr ViewportExtent Viewport() const noexcept { return m_viewport; }
    [[nodiscard]] constexpr const RenderStats& Stats() const noexcept { return m_stats; }
    [[nodiscard]] constexpr u8 ClearPassMask() const noexcept { return m_clearPassMask; }

    [[nodiscard]] constexpr std::span<const DrawBatch> Batches() const noexcept {
        return {m_batches.data(), m_stats.drawBatches};
    }

    [[nodiscard]] constexpr const RenderPassDesc& Pass(u8 passId) const noexcept {
        return m_passes[passId & 0x03U];
    }

    [[nodiscard]] constexpr const Camera2D& View(u8 viewSlot) const noexcept {
        return m_views[viewSlot & 0x07U];
    }

private:
    u64 m_prevKey{0};
    ViewportExtent m_viewport{1280, 720};
    RenderStats m_stats{};
    std::array<u8, 4> m_clipViewStack{};
    u8 m_clipStackDepth{0};
    u8 m_nextClipViewSlot{kFirstClipViewSlot};
    u8 m_activeClipView{kHudViewSlot};
    u8 m_enabledPassMask{0x0FU};
    u8 m_clearPassMask{0x01U};
    bool m_isMonotonic{true};
    bool m_compiled{false};
    bool m_pixelSnap{false};

    std::array<RenderPassDesc, kMaxPasses> m_passes{};
    std::array<Camera2D, kMaxViews> m_views{};
    std::array<DrawBatch, kMaxBatches> m_batches{};
    alignas(64) std::array<u64, kMaxQuads> m_keys{};
    alignas(64) std::array<PreparedQuad, kMaxQuads> m_quads{};
};
static_assert(sizeof(RenderList) == 282176);

} // namespace en
