#pragma once

#include "renderer/RendererTypes.h"

namespace en {

// 2D quad command list with frustum culling, 64-bit sort-key ordering, and state batching.
// Value type with fixed inline storage and zero runtime allocations.
class RenderList {
public:
    static constexpr u32 kMaxQuads = 4096;
    static constexpr u32 kMaxBatches = 256;
    static constexpr u32 kChunkQuads = 512;
    static constexpr u8  kMaxPasses = 4;
    static constexpr u8  kMaxViews = 8;
    static constexpr u8  kHudViewSlot = 7;

    constexpr RenderList() noexcept { Reset({1280, 720}); }

    constexpr void Reset(Viewport vp = {1280, 720}) noexcept {
        if (vp.Valid()) m_viewport = vp;
        m_lastKey = 0ULL;
        m_stats = {};
        m_enabledPassMask = 0x0FU;
        m_clearPassMask = 0x01U;
        m_monotonic = true;
        m_compiled = false;
        m_pixelSnap = false;
        for (u8 i = 0U; i < kMaxPasses; ++i) {
            m_passes[i] = PassDesc{.clear = (i == 0U)};
        }
        for (u8 i = 0U; i < kMaxViews; ++i) {
            m_views[i] = Camera2D{.screenSpace = (i == kHudViewSlot)};
        }
    }

    constexpr void SetPass(u8 id, const PassDesc& d) noexcept {
        if (id >= kMaxPasses) return;
        m_passes[id] = d;
        const u8 bit = static_cast<u8>(1U << id);
        m_enabledPassMask = d.enabled
            ? static_cast<u8>(m_enabledPassMask | bit)
            : static_cast<u8>(m_enabledPassMask & ~bit);
        m_clearPassMask = (d.enabled && d.clear)
            ? static_cast<u8>(m_clearPassMask | bit)
            : static_cast<u8>(m_clearPassMask & ~bit);
    }

    constexpr void SetView(u8 slot, const Camera2D& cam) noexcept {
        if (slot < kMaxViews) m_views[slot] = cam;
    }

    constexpr void SetScissor(u8 slot, Rect norm) noexcept {
        if (slot < kMaxViews) m_views[slot].scissor = norm;
    }

    constexpr void SetPixelSnap(bool on) noexcept { m_pixelSnap = on; }

    void Push(const QuadCmd& cmd) noexcept;

    void PushRect(Rect r, Color c, u8 pass = 0, u8 layer = 10, u16 depth = 32768,
                  u8 view = 0, BlendMode blend = BlendMode::Alpha) noexcept;

    void PushTile(Rect r, TextureHandle tex, Vec2 tile,
                  Rect uv = {0, 0, 1, 1}, Color c = {1, 1, 1, 1},
                  u8 pass = 0, u8 layer = 5, u16 depth = 32768, u8 view = 0) noexcept;

    void PushOutline(Rect r, f32 thick, Color c, u8 pass = 0, u8 layer = 10,
                     u16 depth = 32768, u8 view = 0) noexcept;

    void PushLine(Vec2 a, Vec2 b, f32 thick, Color c, u8 pass = 0, u8 layer = 10,
                  u16 depth = 32768, u8 view = 0) noexcept;

    void PushCircle(Vec2 center, f32 radius, u32 segs, f32 thick, Color c,
                    u8 pass = 0, u8 layer = 10, u8 view = 0) noexcept;

    void PushNine(const NineSlice& n, u8 pass = 0, u8 layer = 20, u8 view = 0) noexcept;

    void PushText(Vec2 pos, std::string_view text, Color c, f32 scale = 1.0f,
                  u8 pass = 0, u8 layer = 30, u8 view = 0, TextureHandle font = {}) noexcept;

    [[nodiscard]] static constexpr Vec2 MeasureText(std::string_view text, f32 scale = 1.0f) noexcept {
        if (text.empty()) return {};
        const f32 qs = 16.0f * scale;
        const f32 adv = 12.0f * scale;
        const f32 line = qs * 1.15f;
        f32 maxW = 0.0f, cur = 0.0f;
        u32 lines = 1U;
        for (const char ch : text) {
            if (ch == '\n') {
                maxW = std::max(maxW, cur);
                cur = 0.0f;
                ++lines;
            } else if ((static_cast<u8>(ch) & 0xC0U) != 0x80U) {
                cur += adv;
            }
        }
        return {std::max(maxW, cur), static_cast<f32>(lines - 1U) * line + qs};
    }

    void Compile() noexcept;

    void Expand(u32 first, u32 count, std::span<Vertex2D> out) const noexcept;

    [[nodiscard]] static std::span<const u16> Indices(u32 count) noexcept;

    [[nodiscard]] constexpr Viewport GetViewport() const noexcept { return m_viewport; }
    [[nodiscard]] constexpr const RenderStats& Stats() const noexcept { return m_stats; }
    [[nodiscard]] constexpr u8 ClearPassMask() const noexcept { return m_clearPassMask; }

    [[nodiscard]] constexpr std::span<const DrawBatch> Batches() const noexcept {
        return {m_batches.data(), m_stats.batches};
    }
    [[nodiscard]] constexpr const PassDesc& Pass(u8 id) const noexcept {
        return m_passes[id & 0x03U];
    }
    [[nodiscard]] constexpr const Camera2D& View(u8 slot) const noexcept {
        return m_views[slot & 0x07U];
    }

private:
    u64         m_lastKey{0};
    Viewport    m_viewport{1280, 720};
    RenderStats m_stats{};
    u8          m_enabledPassMask{0x0F};
    u8          m_clearPassMask{0x01};
    bool        m_monotonic{true};
    bool        m_compiled{false};
    bool        m_pixelSnap{false};

    std::array<PassDesc, kMaxPasses>               m_passes{};
    std::array<Camera2D, kMaxViews>                m_views{};
    std::array<DrawBatch, kMaxBatches>             m_batches{};
    std::array<u64, kMaxQuads>          m_keys{};
    std::array<PreparedQuad, kMaxQuads> m_quads{};
};

} // namespace en
