#include "renderer/RenderList.h"
#include <algorithm>
#include <cmath>

namespace en {

bool RenderList::Init(usize arenaBytes) noexcept {
    Shutdown();
    if (!m_arena.Init(arenaBytes)) return false;
    m_keys = m_arena.Carve<u64>(kMaxQuads);
    m_scratchKeys = m_arena.Carve<u64>(kMaxQuads);
    m_quads = m_arena.Carve<PreparedQuad>(kMaxQuads);
    m_batches = m_arena.Carve<DrawBatch>(kMaxBatches);
    if (m_keys.empty() || m_scratchKeys.empty() || m_quads.empty() || m_batches.empty()) {
        Shutdown();
        return false;
    }
    Reset({1280, 720});
    return true;
}

void RenderList::Shutdown() noexcept {
    m_arena.Shutdown();
}

void RenderList::Reset(ViewportExtent viewportSize) noexcept {
    if (viewportSize.IsValid()) m_viewport = viewportSize;
    m_prevKey = 0;
    m_varyingBits = 0;
    m_stats = RenderStats{};
    m_isMonotonic = true;
    m_compiled = false;
    m_occluderCount = 0;
    for (u8 i = 0; i < kMaxViews; ++i) {
        m_views[i] = Camera2D{};
        m_views[i].screenSpace = (i == 15U);
    }
}

bool RenderList::IsOccluded2D(Rect worldBounds) const noexcept {
    for (u32 i = 0; i < m_occluderCount; ++i) {
        const Rect& occ = m_occluders[i];
        if (worldBounds.x >= occ.x && worldBounds.y >= occ.y &&
            worldBounds.Right() <= occ.Right() && worldBounds.Bottom() <= occ.Bottom()) {
            return true;
        }
    }
    return false;
}

void RenderList::PushQuad(const QuadCmd& cmd) noexcept {
    if (m_quads.empty()) return;
    if (m_stats.submittedQuads >= kMaxQuads) {
        ++m_stats.droppedQuads;
        return;
    }

    const Camera2D& cam = m_views[cmd.viewSlot & 0x0FU];
    f32 cx = cmd.center.x;
    f32 cy = cmd.center.y;
    f32 w = cmd.size.x;
    f32 h = cmd.size.y;

    if (!cam.screenSpace) {
        const f32 scale = cam.pixelsPerMeter * std::max(0.05f, cam.zoom);
        const f32 halfVpW = static_cast<f32>(m_viewport.w) * 0.5f;
        const f32 halfVpH = static_cast<f32>(m_viewport.h) * 0.5f;
        cx = halfVpW + (cmd.center.x - cam.worldCenterX) * scale;
        cy = halfVpH - (cmd.center.y - cam.worldCenterY) * scale;
        w = cmd.size.x * scale;
        h = cmd.size.y * scale;

        const Rect worldRect = Rect::FromCenterHalf(cmd.center, cmd.size * 0.5f);
        if (cmd.layer < 18 && IsOccluded2D(worldRect)) {
            ++m_stats.culledQuads;
            return;
        }
        const f32 rad = 0.5f * (std::abs(w) + std::abs(h));
        if (cx + rad < -64.0f || cx - rad > static_cast<f32>(m_viewport.w) + 64.0f ||
            cy + rad < -64.0f || cy - rad > static_cast<f32>(m_viewport.h) + 64.0f) {
            ++m_stats.culledQuads;
            return;
        }
    }

    const u16 qIdx = static_cast<u16>(m_stats.submittedQuads++);
    const u64 key = SortKey::Pack(cmd, qIdx);
    if (qIdx > 0) {
        if (key < m_prevKey) m_isMonotonic = false;
        m_varyingBits |= (key ^ m_prevKey);
    }
    m_prevKey = key;
    m_keys[qIdx] = key;

    const f32 hw = w * 0.5f;
    const f32 hh = h * 0.5f;
    const f32 hxX = hw * cmd.cosRot;
    const f32 hxY = hw * cmd.sinRot;
    const f32 hyX = -hh * cmd.sinRot;
    const f32 hyY = hh * cmd.cosRot;

    const ShaderParamBlock& mat = m_materials[cmd.materialId % kMaxMaterials];
    PreparedQuad& pq = m_quads[qIdx];
    pq.color = Color{
        std::clamp(cmd.color.r + mat.emissiveBoost, 0.0f, 1.0f),
        std::clamp(cmd.color.g + mat.emissiveBoost, 0.0f, 1.0f),
        std::clamp(cmd.color.b + mat.emissiveBoost, 0.0f, 1.0f),
        cmd.color.a};
    pq.cx = cx;
    pq.cy = cy;
    pq.d0x = hxX + hyX;
    pq.d0y = hxY + hyY;
    pq.d1x = hxX - hyX;
    pq.d1y = hxY - hyY;
    pq.u0 = cmd.flipX ? cmd.uv.Right() : cmd.uv.x;
    pq.u1 = cmd.flipX ? cmd.uv.x : cmd.uv.Right();
    pq.v0 = cmd.flipY ? cmd.uv.Bottom() : cmd.uv.y;
    pq.v1 = cmd.flipY ? cmd.uv.y : cmd.uv.Bottom();
    pq.texture = cmd.texture;
}

void RenderList::PushRect(
    Rect rect, Color color, u8 passId, u8 layer, u16 depth, u8 viewSlot, BlendMode blend) noexcept {
    QuadCmd cmd{};
    cmd.center = rect.Center();
    cmd.size = {rect.w, rect.h};
    cmd.color = color;
    cmd.passId = passId;
    cmd.layer = layer;
    cmd.depth = depth;
    cmd.viewSlot = viewSlot;
    cmd.blend = blend;
    PushQuad(cmd);
}

void RenderList::PushLine(
    Vec2 a, Vec2 b, f32 thickness, Color color, u8 passId, u8 layer, u16 depth, u8 viewSlot) noexcept {
    const Vec2 d = b - a;
    const f32 len = d.Length();
    if (len < 1e-5f) return;
    QuadCmd cmd{};
    cmd.center = (a + b) * 0.5f;
    cmd.size = {len, thickness};
    cmd.cosRot = d.x / len;
    cmd.sinRot = d.y / len;
    cmd.color = color;
    cmd.passId = passId;
    cmd.layer = layer;
    cmd.depth = depth;
    cmd.viewSlot = viewSlot;
    PushQuad(cmd);
}

void RenderList::PushCircleOutline(
    Vec2 center, f32 radius, u32 segments, f32 thickness, Color color,
    u8 passId, u8 layer, u8 viewSlot) noexcept {
    const u32 segs = std::clamp(segments, 6U, 64U);
    const f32 step = kTwoPi / static_cast<f32>(segs);
    Vec2 prev{center.x + radius, center.y};
    for (u32 i = 1; i <= segs; ++i) {
        f32 c = 1.0f, s = 0.0f;
        FastSinCos(static_cast<f32>(i) * step, c, s);
        const Vec2 cur{center.x + c * radius, center.y + s * radius};
        PushLine(prev, cur, thickness, color, passId, layer, 32768, viewSlot);
        prev = cur;
    }
}

void RenderList::PushSlashArc(
    Vec2 center, f32 radius, f32 startAngleRad, f32 sweepAngleRad,
    f32 thickness, Color color, u8 passId, u8 layer, u8 viewSlot) noexcept {
    constexpr u32 kArcSegs = 8;
    const f32 step = sweepAngleRad / static_cast<f32>(kArcSegs);
    f32 c0 = 1.0f, s0 = 0.0f;
    FastSinCos(startAngleRad, c0, s0);
    Vec2 prev{center.x + c0 * radius, center.y + s0 * radius};
    for (u32 i = 1; i <= kArcSegs; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kArcSegs);
        f32 c = 1.0f, s = 0.0f;
        FastSinCos(startAngleRad + static_cast<f32>(i) * step, c, s);
        const Vec2 cur{center.x + c * radius, center.y + s * radius};
        Color segCol = color;
        segCol.a *= (0.3f + 0.7f * t);
        PushLine(prev, cur, thickness * (0.4f + 0.6f * std::sin(t * kPi)), segCol, passId, layer, 32768, viewSlot);
        prev = cur;
    }
}

void RenderList::PushBeam(
    Vec2 start, Vec2 end, f32 width, Color coreColor, Color glowColor,
    u8 passId, u8 layer, u8 viewSlot) noexcept {
    PushLine(start, end, width * 2.2f, glowColor, passId, layer, 32767, viewSlot);
    PushLine(start, end, width, coreColor, passId, layer, 32768, viewSlot);
}

void RenderList::PushRing(
    Vec2 center, f32 innerRadius, f32 outerRadius, Color color,
    u8 passId, u8 layer, u8 viewSlot) noexcept {
    const f32 midRadius = 0.5f * (innerRadius + outerRadius);
    const f32 thick = std::max(0.02f, outerRadius - innerRadius);
    PushCircleOutline(center, midRadius, 16, thick, color, passId, layer, viewSlot);
}

void RenderList::PushNineSlice(const NineSliceDesc& desc, u8 passId, u8 layer, u8 viewSlot) noexcept {
    PushRect(desc.outerRect, desc.color, passId, layer, 32768, viewSlot);
    const Rect r = desc.outerRect;
    const Color borderCol = Color::FromRGBA8(220, 230, 255, 180);
    PushRect({r.x, r.y, r.w, desc.borderTop * 0.25f}, borderCol, passId, layer + 1U, 32768, viewSlot);
    PushRect({r.x, r.Bottom() - desc.borderBottom * 0.25f, r.w, desc.borderBottom * 0.25f}, borderCol, passId, layer + 1U, 32768, viewSlot);
}

void RenderList::PushProgressBar(
    Rect rect, f32 ratio, Color fillColor, Color backColor, Color borderColor,
    u8 passId, u8 layer, u8 viewSlot) noexcept {
    PushRect(rect, backColor, passId, layer, 32768, viewSlot);
    const f32 clamped = std::clamp(ratio, 0.0f, 1.0f);
    if (clamped > 0.0f) {
        PushRect({rect.x, rect.y, rect.w * clamped, rect.h}, fillColor, passId, layer + 1U, 32768, viewSlot);
    }
    PushRect({rect.x, rect.y, rect.w, 1.5f}, borderColor, passId, layer + 2U, 32768, viewSlot);
}

void RenderList::PushText(
    Vec2 pos, std::string_view text, Color color, f32 scale,
    u8 passId, u8 layer, u8 viewSlot, GpuTextureHandle fontTex) noexcept {
    const f32 quadSize = 16.0f * scale;
    const f32 advanceX = 12.0f * scale;
    f32 cursorX = pos.x;
    const GpuTextureHandle activeFont = (fontTex == kWhiteGpuTexture) ? kBuiltinFontTexture : fontTex;
    for (char c : text) {
        if (c == '\n') {
            cursorX = pos.x;
            pos.y += quadSize * 1.15f;
            continue;
        }
        if (c > 32 && c <= 126) {
            const u8 glyph = static_cast<u8>(c - 32);
            const f32 u0 = static_cast<f32>(glyph & 15U) * (1.0f / 16.0f);
            const f32 v0 = static_cast<f32>(glyph >> 4U) * (1.0f / 6.0f);
            QuadCmd cmd{};
            cmd.center = {cursorX + quadSize * 0.5f, pos.y + quadSize * 0.5f};
            cmd.size = {quadSize, quadSize};
            cmd.uv = {u0, v0, 7.0f / 128.0f, 7.2f / 48.0f};
            cmd.color = color;
            cmd.texture = activeFont;
            cmd.passId = passId;
            cmd.layer = layer;
            cmd.viewSlot = viewSlot;
            PushQuad(cmd);
        }
        cursorX += advanceX;
    }
}

void RenderList::Compile() noexcept {
    if (m_compiled) return;
    m_compiled = true;
    const u32 count = m_stats.submittedQuads;
    if (count == 0) return;

    m_stats.monotonicFastPath = m_isMonotonic;
    if (!m_isMonotonic && count > 1) {
        const u64 varyingStateBits = m_varyingBits & SortKey::kBatchStateMask;
        u64* src = m_keys.data();
        u64* dst = m_scratchKeys.data();
        u8 passes = 0;

        for (u32 shift : {16U, 24U, 32U, 40U, 48U, 56U}) {
            if (((varyingStateBits >> shift) & 0xFFULL) == 0ULL) continue;
            std::array<u32, 256> hist{};
            for (u32 i = 0; i < count; ++i) {
                ++hist[(src[i] >> shift) & 0xFFU];
            }
            u32 offset = 0;
            for (u32 b = 0; b < 256U; ++b) {
                const u32 c = hist[b];
                hist[b] = offset;
                offset += c;
            }
            for (u32 i = 0; i < count; ++i) {
                const u64 k = src[i];
                dst[hist[(k >> shift) & 0xFFU]++] = k;
            }
            std::swap(src, dst);
            ++passes;
        }
        if (src != m_keys.data()) {
            std::memcpy(m_keys.data(), src, static_cast<usize>(count) * sizeof(u64));
        }
        m_stats.radixPassesRun = passes;
    }

    // Coalesce sorted quads into minimal state-identical DrawBatches.
    u32 batchIdx = 0;
    u64 curState = m_keys[0] & SortKey::kBatchStateMask;
    u32 batchStart = 0;

    for (u32 i = 1; i < count; ++i) {
        const u64 kState = m_keys[i] & SortKey::kBatchStateMask;
        if (kState != curState && batchIdx + 1U < kMaxBatches) {
            const PreparedQuad& firstQ = m_quads[SortKey::QuadIndex(m_keys[batchStart])];
            m_batches[batchIdx++] = DrawBatch{
                batchStart, i - batchStart, firstQ.texture,
                SortKey::PassId(curState), SortKey::ViewSlot(curState), SortKey::Blend(curState), 0};
            curState = kState;
            batchStart = i;
        }
    }
    if (batchIdx < kMaxBatches) {
        const PreparedQuad& firstQ = m_quads[SortKey::QuadIndex(m_keys[batchStart])];
        m_batches[batchIdx++] = DrawBatch{
            batchStart, count - batchStart, firstQ.texture,
            SortKey::PassId(curState), SortKey::ViewSlot(curState), SortKey::Blend(curState), 0};
    }
    m_stats.drawBatches = batchIdx;
}

void RenderList::ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept {
    const u32 limit = std::min(firstQuad + quadCount, m_stats.submittedQuads);
    usize vOut = 0;
    for (u32 i = firstQuad; i < limit && vOut + 4U <= outVertices.size(); ++i) {
        const u16 qIdx = SortKey::QuadIndex(m_keys[i]);
        const PreparedQuad& q = m_quads[qIdx];
        outVertices[vOut + 0] = Vertex2D{q.color, q.cx - q.d0x, q.cy - q.d0y, q.u0, q.v0};
        outVertices[vOut + 1] = Vertex2D{q.color, q.cx + q.d1x, q.cy + q.d1y, q.u1, q.v0};
        outVertices[vOut + 2] = Vertex2D{q.color, q.cx + q.d0x, q.cy + q.d0y, q.u1, q.v1};
        outVertices[vOut + 3] = Vertex2D{q.color, q.cx - q.d1x, q.cy - q.d1y, q.u0, q.v1};
        vOut += 4U;
    }
}

u64 RenderList::ComputeAuditHash() const noexcept {
    u64 h = Mix64(static_cast<u64>(m_stats.submittedQuads) |
                  (static_cast<u64>(m_stats.drawBatches) << 32U));
    for (u32 i = 0; i < m_stats.submittedQuads; ++i) {
        h = HashCombine64(h, m_keys[i]);
    }
    return h;
}

} // namespace en
