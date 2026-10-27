#include "renderer/RenderList.h"

#include <algorithm>

namespace en {

namespace {

consteval std::array<u16, RenderList::kChunkQuads * 6U> BuildSharedQuadIndexTable() noexcept {
    std::array<u16, RenderList::kChunkQuads * 6U> table{};
    for (u32 q = 0U; q < RenderList::kChunkQuads; ++q) {
        const u16 base = static_cast<u16>(q * 4U);
        const u32 idx = q * 6U;
        table[idx + 0U] = static_cast<u16>(base + 0U);
        table[idx + 1U] = static_cast<u16>(base + 1U);
        table[idx + 2U] = static_cast<u16>(base + 2U);
        table[idx + 3U] = static_cast<u16>(base + 0U);
        table[idx + 4U] = static_cast<u16>(base + 2U);
        table[idx + 5U] = static_cast<u16>(base + 3U);
    }
    return table;
}

inline constexpr auto kSharedQuadIndices = BuildSharedQuadIndexTable();

} // namespace

void RenderList::PushQuad(const QuadCmd& cmd) noexcept {
    const u8 passId = cmd.passId & 0x03U;
    if ((m_enabledPassMask & (1U << passId)) == 0U ||
        cmd.size.x <= 0.0f || cmd.size.y <= 0.0f || cmd.color.a <= 0.0f) {
        ++m_stats.culledQuads;
        return;
    }

    if (m_stats.submittedQuads >= kMaxQuads) {
        ++m_stats.droppedQuads;
        return;
    }

    const Camera2D& cam = m_views[cmd.viewSlot & 0x07U];
    const f32 vpW = cam.viewport.w * static_cast<f32>(m_viewport.w);
    const f32 vpH = cam.viewport.h * static_cast<f32>(m_viewport.h);
    if (vpW <= 0.0f || vpH <= 0.0f) {
        ++m_stats.culledQuads;
        return;
    }

    const f32 scale = cam.screenSpace ? cam.zoom : cam.EffectiveScale();
    const f32 ySign = (!cam.screenSpace && cam.yUp) ? -1.0f : 1.0f;
    f32 cx = cam.screenSpace
                 ? (cmd.center.x * scale)
                 : (vpW * 0.5f + (cmd.center.x - cam.worldCenterX) * scale);
    f32 cy = cam.screenSpace
                 ? (cmd.center.y * scale)
                 : (vpH * 0.5f + (cmd.center.y - cam.worldCenterY) * scale * ySign);

    const f32 hw = cmd.size.x * scale * 0.5f;
    const f32 hh = cmd.size.y * scale * 0.5f;
    const f32 effSin = cmd.sinRot * ySign;
    const f32 hxX = hw * cmd.cosRot;
    const f32 hxY = hw * effSin;
    const f32 hyX = -hh * effSin;
    const f32 hyY = hh * cmd.cosRot;

    if (cmd.pivot.x != 0.5f || cmd.pivot.y != 0.5f) {
        const f32 px = 1.0f - 2.0f * cmd.pivot.x;
        const f32 py = (1.0f - 2.0f * cmd.pivot.y) * ySign;
        cx += px * hxX + py * hyX;
        cy += px * hxY + py * hyY;
    }

    if (cmd.pixelSnap || m_pixelSnap) {
        cx = std::floor(cx + 0.5f);
        cy = std::floor(cy + 0.5f);
    }

    const f32 d0x = hxX + hyX;
    const f32 d0y = hxY + hyY;
    const f32 d1x = hxX - hyX;
    const f32 d1y = hxY - hyY;

    const f32 aabbHalfX = std::max(std::abs(d0x), std::abs(d1x));
    const f32 aabbHalfY = std::max(std::abs(d0y), std::abs(d1y));
    const f32 clipMinX = std::max(0.0f, cam.scissor.x * vpW);
    const f32 clipMinY = std::max(0.0f, cam.scissor.y * vpH);
    const f32 clipMaxX = std::min(vpW, cam.scissor.Right() * vpW);
    const f32 clipMaxY = std::min(vpH, cam.scissor.Bottom() * vpH);

    if (cx + aabbHalfX < clipMinX || cx - aabbHalfX > clipMaxX ||
        cy + aabbHalfY < clipMinY || cy - aabbHalfY > clipMaxY) {
        ++m_stats.culledQuads;
        return;
    }

    const u16 qIdx = static_cast<u16>(m_stats.submittedQuads++);
    const TextureHandle tex = cmd.texture.IsValid() ? cmd.texture : kWhiteTexture;
    const u64 key = SortKey::Pack(cmd, tex.index, qIdx);
    if (qIdx > 0U && key < m_prevKey) {
        m_isMonotonic = false;
    }
    m_prevKey = key;
    m_keys[qIdx] = key;

    m_quads[qIdx] = PreparedQuad{
        .color = cmd.color,
        .cx = cx,
        .cy = cy,
        .d0x = d0x,
        .d0y = d0y,
        .d1x = d1x,
        .d1y = d1y,
        .u0 = cmd.flipX ? cmd.uv.Right() : cmd.uv.x,
        .v0 = cmd.flipY ? cmd.uv.Bottom() : cmd.uv.y,
        .u1 = cmd.flipX ? cmd.uv.x : cmd.uv.Right(),
        .v1 = cmd.flipY ? cmd.uv.y : cmd.uv.Bottom(),
        .texture = tex,
    };
}

void RenderList::PushRect(
    Rect rect, Color color, u8 passId, u8 layer, u16 depth, u8 viewSlot, BlendMode blend) noexcept {
    PushQuad(QuadCmd{
        .center = rect.Center(),
        .size = {rect.w, rect.h},
        .color = color,
        .depth = depth,
        .passId = passId,
        .layer = layer,
        .viewSlot = viewSlot,
        .blend = blend,
    });
}

void RenderList::PushTiledRect(
    Rect rect, TextureHandle texture, Vec2 tileSize, Rect uvRect,
    Color color, u8 passId, u8 layer, u16 depth, u8 viewSlot) noexcept {
    if (rect.IsEmpty() || tileSize.x <= 0.01f || tileSize.y <= 0.01f) return;

    QuadCmd cmd{
        .color = color,
        .texture = texture.IsValid() ? texture : kWhiteTexture,
        .depth = depth,
        .passId = passId,
        .layer = layer,
        .viewSlot = viewSlot,
    };

    for (f32 y = rect.y; y < rect.Bottom(); y += tileSize.y) {
        const f32 h = std::min(tileSize.y, rect.Bottom() - y);
        const f32 vRatio = h / tileSize.y;
        for (f32 x = rect.x; x < rect.Right(); x += tileSize.x) {
            const f32 w = std::min(tileSize.x, rect.Right() - x);
            const f32 uRatio = w / tileSize.x;
            cmd.center = {x + w * 0.5f, y + h * 0.5f};
            cmd.size = {w, h};
            cmd.uv = {uvRect.x, uvRect.y, uvRect.w * uRatio, uvRect.h * vRatio};
            PushQuad(cmd);
        }
    }
}

void RenderList::PushRectOutline(
    Rect rect, f32 thickness, Color color, u8 passId, u8 layer, u16 depth, u8 viewSlot) noexcept {
    if (rect.IsEmpty() || thickness <= 0.0f) return;
    const f32 t = std::min({thickness, rect.w * 0.5f, rect.h * 0.5f});
    PushRect({rect.x, rect.y, rect.w, t}, color, passId, layer, depth, viewSlot);
    PushRect({rect.x, rect.Bottom() - t, rect.w, t}, color, passId, layer, depth, viewSlot);
    if (rect.h > 2.0f * t) {
        PushRect({rect.x, rect.y + t, t, rect.h - 2.0f * t}, color, passId, layer, depth, viewSlot);
        PushRect({rect.Right() - t, rect.y + t, t, rect.h - 2.0f * t}, color, passId, layer, depth, viewSlot);
    }
}

void RenderList::PushLine(
    Vec2 a, Vec2 b, f32 thickness, Color color, u8 passId, u8 layer, u16 depth, u8 viewSlot) noexcept {
    const f32 dx = b.x - a.x;
    const f32 dy = b.y - a.y;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq <= 1e-10f || thickness <= 0.0f) return;
    const f32 len = std::sqrt(lenSq);
    const f32 invLen = 1.0f / len;

    PushQuad(QuadCmd{
        .center = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f},
        .size = {len, thickness},
        .cosRot = dx * invLen,
        .sinRot = dy * invLen,
        .color = color,
        .depth = depth,
        .passId = passId,
        .layer = layer,
        .viewSlot = viewSlot,
    });
}

void RenderList::PushCircleOutline(
    Vec2 center, f32 radius, u32 segments, f32 thickness, Color color,
    u8 passId, u8 layer, u8 viewSlot) noexcept {
    if (radius <= 0.0f || thickness <= 0.0f) return;
    const u32 segs = std::clamp(segments, 6U, 64U);
    const f32 step = kTwoPi / static_cast<f32>(segs);
    Vec2 prev{center.x + radius, center.y};
    for (u32 i = 1U; i <= segs; ++i) {
        f32 c = 1.0f, s = 0.0f;
        FastSinCos(static_cast<f32>(i) * step, c, s);
        const Vec2 cur{center.x + c * radius, center.y + s * radius};
        PushLine(prev, cur, thickness, color, passId, layer, 32768U, viewSlot);
        prev = cur;
    }
}

void RenderList::PushNineSlice(
    const NineSliceDesc& desc, u8 passId, u8 layer, u8 viewSlot) noexcept {
    const Rect& r = desc.outerRect;
    const Rect& uv = desc.uv;
    if (r.IsEmpty() || uv.IsEmpty()) return;

    const f32 bx = std::clamp(desc.border, 0.0f, r.w * 0.5f);
    const f32 by = std::clamp(desc.border, 0.0f, r.h * 0.5f);
    const f32 xCols[4]{r.x, r.x + bx, r.Right() - bx, r.Right()};
    const f32 yRows[4]{r.y, r.y + by, r.Bottom() - by, r.Bottom()};

    const f32 uvInset = std::clamp(desc.uvBorder, 0.0f, 0.5f);
    const f32 uIn = uv.w * uvInset;
    const f32 vIn = uv.h * uvInset;
    const f32 uCols[4]{uv.x, uv.x + uIn, uv.Right() - uIn, uv.Right()};
    const f32 vRows[4]{uv.y, uv.y + vIn, uv.Bottom() - vIn, uv.Bottom()};

    QuadCmd patch{
        .color = desc.color,
        .texture = desc.texture.IsValid() ? desc.texture : kWhiteTexture,
        .depth = desc.depth,
        .passId = passId,
        .layer = layer,
        .viewSlot = viewSlot,
        .blend = desc.blend,
    };

    for (u32 row = 0U; row < 3U; ++row) {
        const f32 py = yRows[row];
        const f32 ph = yRows[row + 1U] - py;
        if (ph <= 1e-4f) continue;

        const f32 vy = vRows[row];
        const f32 vh = vRows[row + 1U] - vy;

        for (u32 col = 0U; col < 3U; ++col) {
            if (row == 1U && col == 1U && !desc.drawCenter) continue;
            const f32 px = xCols[col];
            const f32 pw = xCols[col + 1U] - px;
            if (pw <= 1e-4f) continue;

            patch.center = {px + pw * 0.5f, py + ph * 0.5f};
            patch.size = {pw, ph};
            patch.uv = {uCols[col], vy, uCols[col + 1U] - uCols[col], vh};
            PushQuad(patch);
        }
    }
}

void RenderList::PushText(
    Vec2 pos, std::string_view text, Color color, f32 scale,
    u8 passId, u8 layer, u8 viewSlot, TextureHandle fontTex) noexcept {
    if (text.empty() || scale <= 0.0f || !fontTex.IsValid() || fontTex == kWhiteTexture) return;

    const f32 quadSize = 16.0f * scale;
    const f32 advanceX = 12.0f * scale;
    f32 cursorX = pos.x;
    f32 cursorY = pos.y;

    QuadCmd cmd{
        .size = {quadSize, quadSize},
        .color = color,
        .texture = fontTex,
        .passId = passId,
        .layer = layer,
        .viewSlot = viewSlot,
    };

    for (const char c : text) {
        if (c == '\n') {
            cursorX = pos.x;
            cursorY += quadSize * 1.15f;
            continue;
        }
        const u8 b = static_cast<u8>(c);
        if ((b & 0xC0U) == 0x80U) continue;

        const u8 ch = (b >= 128U) ? static_cast<u8>('?') : b;
        if (ch > 32U && ch <= 126U) {
            const u8 glyph = static_cast<u8>(ch - 32U);
            const f32 u0 = static_cast<f32>(glyph & 15U) * (1.0f / 16.0f);
            const f32 v0 = static_cast<f32>(glyph >> 4U) * (1.0f / 6.0f);
            cmd.center = {cursorX + quadSize * 0.5f, cursorY + quadSize * 0.5f};
            cmd.uv = {u0, v0, 1.0f / 16.0f, 1.0f / 6.0f};
            PushQuad(cmd);
        }
        cursorX += advanceX;
    }
}

void RenderList::Compile() noexcept {
    if (m_compiled) return;
    m_compiled = true;

    const u32 count = m_stats.submittedQuads;
    if (count == 0U) return;

    if (!m_isMonotonic && count > 1U) {
        std::sort(m_keys.begin(), m_keys.begin() + count);
    }

    u32 batchIdx = 0U;
    const auto emitBatch = [&](u32 start, u32 end, u64 state) noexcept {
        if (batchIdx >= kMaxBatches) {
            m_stats.droppedQuads += (end - start);
            return;
        }
        const PreparedQuad& firstQ = m_quads[SortKey::QuadIndex(m_keys[start])];
        m_batches[batchIdx++] = DrawBatch{
            .texture = firstQ.texture,
            .firstQuad = static_cast<u16>(start),
            .quadCount = static_cast<u16>(end - start),
            .passId = SortKey::PassId(state),
            .viewSlot = SortKey::ViewSlot(state),
            .blend = SortKey::Blend(state),
        };
    };

    u64 curState = m_keys[0] & SortKey::kBatchStateMask;
    u32 batchStart = 0U;
    for (u32 i = 1U; i < count; ++i) {
        const u64 kState = m_keys[i] & SortKey::kBatchStateMask;
        if (kState != curState) {
            emitBatch(batchStart, i, curState);
            curState = kState;
            batchStart = i;
        }
    }
    emitBatch(batchStart, count, curState);
    m_stats.drawBatches = batchIdx;
}

void RenderList::ExpandVertices(
    u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept {
    const u32 limit = std::min(firstQuad + quadCount, m_stats.submittedQuads);
    usize vOut = 0U;
    for (u32 i = firstQuad; i < limit && vOut + 4U <= outVertices.size(); ++i) {
        const PreparedQuad& q = m_quads[SortKey::QuadIndex(m_keys[i])];
        outVertices[vOut + 0U] = Vertex2D{q.cx - q.d0x, q.cy - q.d0y, q.color, q.u0, q.v0};
        outVertices[vOut + 1U] = Vertex2D{q.cx + q.d1x, q.cy + q.d1y, q.color, q.u1, q.v0};
        outVertices[vOut + 2U] = Vertex2D{q.cx + q.d0x, q.cy + q.d0y, q.color, q.u1, q.v1};
        outVertices[vOut + 3U] = Vertex2D{q.cx - q.d1x, q.cy - q.d1y, q.color, q.u0, q.v1};
        vOut += 4U;
    }
}

std::span<const u16> RenderList::SharedQuadIndices(u32 quadCount) noexcept {
    const u32 clampedQuads = std::min(quadCount, kChunkQuads);
    return {kSharedQuadIndices.data(), static_cast<usize>(clampedQuads) * 6U};
}

} // namespace en
