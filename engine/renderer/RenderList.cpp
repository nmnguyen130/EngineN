#include "renderer/RenderList.h"

#include <algorithm>

namespace en {
namespace {

consteval std::array<u16, RenderList::kChunkQuads * 6U> MakeIndices() noexcept {
    std::array<u16, RenderList::kChunkQuads * 6U> t{};
    for (u32 q = 0U; q < RenderList::kChunkQuads; ++q) {
        const u16 b = static_cast<u16>(q * 4U);
        const u32 i = q * 6U;
        t[i + 0U] = b + 0U; t[i + 1U] = b + 1U; t[i + 2U] = b + 2U;
        t[i + 3U] = b + 0U; t[i + 4U] = b + 2U; t[i + 5U] = b + 3U;
    }
    return t;
}
inline constexpr auto kQuadIndices = MakeIndices();

} // namespace

void RenderList::EmitPostProcess(const PostProcessConfig& fx) noexcept {
    if (!m_viewport.Valid()) return;
    const f32 vw = static_cast<f32>(m_viewport.w);
    const f32 vh = static_cast<f32>(m_viewport.h);

    // 1. Ambient darkness multiply pass (layer 195, before additive PointLight2D layer 200).
    if (fx.ambientColor.a > 1e-4f) {
        PushRect({0.0f, 0.0f, vw, vh}, fx.ambientColor, 0U, 195U, 32768U, kHudViewSlot, BlendMode::Multiply);
    }

    // 2. Screen-border trauma / low-health vignette bands (layer 240).
    if (fx.vignetteColor.a > 1e-4f && fx.vignetteThickness > 1e-4f) {
        const f32 t  = std::clamp(fx.vignetteThickness, 0.01f, 0.45f);
        const f32 bx = vw * t;
        const f32 by = vh * t;
        PushRect({0.0f, 0.0f, vw, by}, fx.vignetteColor, 0U, 240U, 32768U, kHudViewSlot, BlendMode::Alpha);
        PushRect({0.0f, vh - by, vw, by}, fx.vignetteColor, 0U, 240U, 32768U, kHudViewSlot, BlendMode::Alpha);
        PushRect({0.0f, by, bx, vh - 2.0f * by}, fx.vignetteColor, 0U, 240U, 32768U, kHudViewSlot, BlendMode::Alpha);
        PushRect({vw - bx, by, bx, vh - 2.0f * by}, fx.vignetteColor, 0U, 240U, 32768U, kHudViewSlot, BlendMode::Alpha);
    }

    // 3. Full-screen additive hit-flash / lightning overlay (layer 245).
    if (fx.flashColor.a > 1e-4f) {
        PushRect({0.0f, 0.0f, vw, vh}, fx.flashColor, 0U, 245U, 32768U, kHudViewSlot, BlendMode::Additive);
    }
}

void RenderList::Push(const QuadCmd& cmd) noexcept {
    const u8 pass = cmd.pass & 0x03U;
    if ((m_enabledPassMask & (1U << pass)) == 0U || cmd.size.x <= 0.0f || cmd.size.y <= 0.0f ||
        cmd.color.a <= 0.0f) {
        ++m_stats.culled;
        return;
    }
    if (m_stats.submitted >= kMaxQuads) {
        ++m_stats.dropped;
        return;
    }

    const Camera2D& cam = m_views[cmd.view & 0x07U];
    const f32 vw = cam.viewport.w * static_cast<f32>(m_viewport.w);
    const f32 vh = cam.viewport.h * static_cast<f32>(m_viewport.h);
    if (vw <= 0.0f || vh <= 0.0f || cam.scissor.IsEmpty()) {
        ++m_stats.culled;
        return;
    }

    const f32 scale = cam.screenSpace ? cam.zoom : cam.Scale();
    const f32 ySign = (!cam.screenSpace && cam.yUp) ? -1.0f : 1.0f;

    f32 cx = 0.0f, cy = 0.0f;
    if (cam.screenSpace) {
        cx = cmd.center.x * scale;
        cy = cmd.center.y * scale;
    } else {
        cx = vw * 0.5f + (cmd.center.x - cam.cx) * scale;
        cy = vh * 0.5f + (cmd.center.y - cam.cy) * scale * ySign;
    }

    f32 hw = cmd.size.x * scale * 0.5f;
    f32 hh = cmd.size.y * scale * 0.5f;
    const f32 s = cmd.sinRot * ySign;
    f32 hxX = hw * cmd.cosRot, hxY = hw * s;
    f32 hyX = -hh * s,         hyY = hh * cmd.cosRot;

    if (cmd.pivot.x != 0.5f || cmd.pivot.y != 0.5f) {
        const f32 px = 1.0f - 2.0f * cmd.pivot.x;
        const f32 py = (1.0f - 2.0f * cmd.pivot.y) * ySign;
        cx += px * hxX + py * hyX;
        cy += px * hxY + py * hyY;
    }

    if (cmd.pixelSnap || m_pixelSnap) {
        cx = std::floor(cx + 0.5f);
        cy = std::floor(cy + 0.5f);
        if (cmd.sinRot == 0.0f && cmd.cosRot == 1.0f) {
            hw  = std::floor(cmd.size.x * scale + 0.5f) * 0.5f;
            hh  = std::floor(cmd.size.y * scale + 0.5f) * 0.5f;
            hxX = hw;   hxY = 0.0f;
            hyX = 0.0f; hyY = hh * ySign;
        }
    }

    const f32 d0x = hxX + hyX, d0y = hxY + hyY;
    const f32 d1x = hxX - hyX, d1y = hxY - hyY;

    const f32 ax = std::max(std::abs(d0x), std::abs(d1x));
    const f32 ay = std::max(std::abs(d0y), std::abs(d1y));
    const f32 x0 = std::max(0.0f, cam.scissor.x * vw);
    const f32 y0 = std::max(0.0f, cam.scissor.y * vh);
    const f32 x1 = std::min(vw, cam.scissor.Right() * vw);
    const f32 y1 = std::min(vh, cam.scissor.Bottom() * vh);
    if (cx + ax < x0 || cx - ax > x1 || cy + ay < y0 || cy - ay > y1) {
        ++m_stats.culled;
        return;
    }

    const u16 idx = static_cast<u16>(m_stats.submitted++);
    const TextureHandle tex = cmd.texture.IsValid() ? cmd.texture : kWhiteTexture;
    const u64 key = SortKey::Pack(cmd, tex.index, idx);
    if (idx > 0U && key < m_lastKey) m_monotonic = false;
    m_lastKey  = key;
    m_keys[idx] = key;

    m_quads[idx] = PreparedQuad{
        .r  = cmd.color.r, .g = cmd.color.g, .b = cmd.color.b, .a = cmd.color.a,
        .cx = cx, .cy = cy,
        .d0x = d0x, .d0y = d0y,
        .d1x = d1x, .d1y = d1y,
        .u0 = cmd.flipX ? cmd.uv.Right()  : cmd.uv.x,
        .v0 = cmd.flipY ? cmd.uv.Bottom() : cmd.uv.y,
        .u1 = cmd.flipX ? cmd.uv.x        : cmd.uv.Right(),
        .v1 = cmd.flipY ? cmd.uv.y        : cmd.uv.Bottom(),
        .texture = tex,
    };
    m_compiled = false;
}

void RenderList::PushRect(Rect r, Color c, u8 pass, u8 layer, u16 depth, u8 view,
                          BlendMode blend) noexcept {
    Push(QuadCmd{
        .center = r.Center(), .size = {r.w, r.h}, .color = c,
        .depth = depth, .pass = pass, .layer = layer, .view = view, .blend = blend,
    });
}

void RenderList::PushTile(Rect r, TextureHandle tex, Vec2 tile, Rect uv, Color c,
                          u8 pass, u8 layer, u16 depth, u8 view) noexcept {
    if (r.IsEmpty() || tile.x <= 0.01f || tile.y <= 0.01f) return;
    QuadCmd cmd{
        .color = c, .texture = tex.IsValid() ? tex : kWhiteTexture,
        .depth = depth, .pass = pass, .layer = layer, .view = view,
    };
    for (f32 y = r.y; y < r.Bottom(); y += tile.y) {
        const f32 h  = std::min(tile.y, r.Bottom() - y);
        const f32 vr = h / tile.y;
        for (f32 x = r.x; x < r.Right(); x += tile.x) {
            const f32 w = std::min(tile.x, r.Right() - x);
            cmd.center = {x + w * 0.5f, y + h * 0.5f};
            cmd.size   = {w, h};
            cmd.uv     = {uv.x, uv.y, uv.w * (w / tile.x), uv.h * vr};
            Push(cmd);
        }
    }
}

void RenderList::PushOutline(Rect r, f32 thick, Color c, u8 pass, u8 layer, u16 depth,
                             u8 view) noexcept {
    if (r.IsEmpty() || thick <= 0.0f) return;
    const f32 t = std::min({thick, r.w * 0.5f, r.h * 0.5f});
    PushRect({r.x, r.y, r.w, t}, c, pass, layer, depth, view);
    PushRect({r.x, r.Bottom() - t, r.w, t}, c, pass, layer, depth, view);
    if (r.h > 2.0f * t) {
        PushRect({r.x, r.y + t, t, r.h - 2.0f * t}, c, pass, layer, depth, view);
        PushRect({r.Right() - t, r.y + t, t, r.h - 2.0f * t}, c, pass, layer, depth, view);
    }
}

void RenderList::PushLine(Vec2 a, Vec2 b, f32 thick, Color c, u8 pass, u8 layer, u16 depth,
                          u8 view) noexcept {
    const f32 dx = b.x - a.x, dy = b.y - a.y;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq <= 1e-10f || thick <= 0.0f) return;
    const f32 len = std::sqrt(lenSq);
    const f32 inv = 1.0f / len;
    Push(QuadCmd{
        .center = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f},
        .size   = {len, thick},
        .cosRot = dx * inv, .sinRot = dy * inv,
        .color  = c, .depth = depth, .pass = pass, .layer = layer, .view = view,
    });
}

void RenderList::PushCircle(Vec2 center, f32 radius, u32 segs, f32 thick, Color c,
                            u8 pass, u8 layer, u8 view) noexcept {
    if (radius <= 0.0f || thick <= 0.0f) return;
    const u32 n    = std::clamp(segs, 6U, 64U);
    const f32 step = kTwoPi / static_cast<f32>(n);
    Vec2 prev{center.x + radius, center.y};
    for (u32 i = 1U; i <= n; ++i) {
        f32 co = 1.0f, si = 0.0f;
        FastSinCos(static_cast<f32>(i) * step, co, si);
        const Vec2 cur{center.x + co * radius, center.y + si * radius};
        PushLine(prev, cur, thick, c, pass, layer, 32768U, view);
        prev = cur;
    }
}

void RenderList::PushNine(const NineSlice& n, u8 pass, u8 layer, u8 view) noexcept {
    const Rect& r  = n.rect;
    const Rect& uv = n.uv;
    if (r.IsEmpty() || uv.IsEmpty()) return;

    const f32 bx = std::clamp(n.border, 0.0f, r.w * 0.5f);
    const f32 by = std::clamp(n.border, 0.0f, r.h * 0.5f);
    const f32 xc[4]{r.x, r.x + bx, r.Right() - bx, r.Right()};
    const f32 yr[4]{r.y, r.y + by, r.Bottom() - by, r.Bottom()};

    const f32 uIn = uv.w * std::clamp(n.uvBorder, 0.0f, 0.5f);
    const f32 vIn = uv.h * std::clamp(n.uvBorder, 0.0f, 0.5f);
    const f32 uc[4]{uv.x, uv.x + uIn, uv.Right() - uIn, uv.Right()};
    const f32 vr[4]{uv.y, uv.y + vIn, uv.Bottom() - vIn, uv.Bottom()};

    QuadCmd q{
        .color = n.color, .texture = n.texture.IsValid() ? n.texture : kWhiteTexture,
        .depth = n.depth, .pass = pass, .layer = layer, .view = view, .blend = n.blend,
    };
    for (u32 row = 0U; row < 3U; ++row) {
        const f32 py = yr[row], ph = yr[row + 1U] - py;
        if (ph <= 1e-4f) continue;
        const f32 vy = vr[row], vh = vr[row + 1U] - vy;
        for (u32 col = 0U; col < 3U; ++col) {
            if (row == 1U && col == 1U && !n.center) continue;
            const f32 px = xc[col], pw = xc[col + 1U] - px;
            if (pw <= 1e-4f) continue;
            q.center = {px + pw * 0.5f, py + ph * 0.5f};
            q.size   = {pw, ph};
            q.uv     = {uc[col], vy, uc[col + 1U] - uc[col], vh};
            Push(q);
        }
    }
}

void RenderList::PushText(Vec2 pos, std::string_view text, Color c, f32 scale,
                          u8 pass, u8 layer, u8 view, TextureHandle font) noexcept {
    if (text.empty() || scale <= 0.0f || !font.IsValid() || font == kWhiteTexture) return;
    const f32 qs  = 16.0f * scale;
    const f32 adv = 12.0f * scale;
    f32 x = pos.x, y = pos.y;
    QuadCmd q{
        .size = {qs, qs}, .color = c, .texture = font,
        .pass = pass, .layer = layer, .view = view,
    };
    for (const char ch : text) {
        if (ch == '\n') {
            x = pos.x;
            y += qs * 1.15f;
            continue;
        }
        const u8 b = static_cast<u8>(ch);
        if ((b & 0xC0U) == 0x80U) continue;
        const u8 g = (b >= 128U) ? static_cast<u8>('?') : b;
        if (g > 32U && g <= 126U) {
            const u8 glyph = static_cast<u8>(g - 32U);
            q.center = {x + qs * 0.5f, y + qs * 0.5f};
            q.uv = {
                static_cast<f32>(glyph & 15U) * (1.0f / 16.0f),
                static_cast<f32>(glyph >> 4U) * (1.0f / 6.0f),
                1.0f / 16.0f, 1.0f / 6.0f,
            };
            Push(q);
        }
        x += adv;
    }
}

void RenderList::Compile() noexcept {
    if (m_compiled) return;
    m_compiled = true;
    const u32 n = m_stats.submitted;
    if (n == 0U) return;

    if (!m_monotonic && n > 1U) {
        std::sort(m_keys.begin(), m_keys.begin() + n);
    }

    u32 bi = 0U;
    const auto emit = [&](u32 start, u32 end, u64 state) noexcept {
        if (bi >= kMaxBatches) {
            m_stats.dropped += (end - start);
            return;
        }
        const PreparedQuad& q = m_quads[SortKey::Idx(m_keys[start])];
        m_batches[bi++] = DrawBatch{
            .texture = q.texture,
            .first   = static_cast<u16>(start),
            .count   = static_cast<u16>(end - start),
            .pass    = SortKey::Pass(state),
            .view    = SortKey::View(state),
            .blend   = SortKey::Blend(state),
        };
    };

    u64 state = m_keys[0] & SortKey::kBatchStateMask;
    u32 start = 0U;
    for (u32 i = 1U; i < n; ++i) {
        const u64 s = m_keys[i] & SortKey::kBatchStateMask;
        if (s != state) {
            emit(start, i, state);
            state = s;
            start = i;
        }
    }
    emit(start, n, state);
    m_stats.batches = bi;
}

void RenderList::Expand(u32 first, u32 count, std::span<Vertex2D> out) const noexcept {
    const u32 end = std::min(first + count, m_stats.submitted);
    usize v = 0U;
    for (u32 i = first; i < end && v + 4U <= out.size(); ++i) {
        const PreparedQuad& q = m_quads[SortKey::Idx(m_keys[i])];
        out[v + 0U] = {q.cx - q.d0x, q.cy - q.d0y, q.r, q.g, q.b, q.a, q.u0, q.v0};
        out[v + 1U] = {q.cx + q.d1x, q.cy + q.d1y, q.r, q.g, q.b, q.a, q.u1, q.v0};
        out[v + 2U] = {q.cx + q.d0x, q.cy + q.d0y, q.r, q.g, q.b, q.a, q.u1, q.v1};
        out[v + 3U] = {q.cx - q.d1x, q.cy - q.d1y, q.r, q.g, q.b, q.a, q.u0, q.v1};
        v += 4U;
    }
}

std::span<const u16> RenderList::Indices(u32 count) noexcept {
    const u32 n = std::min(count, kChunkQuads);
    return {kQuadIndices.data(), static_cast<usize>(n) * 6U};
}

} // namespace en
