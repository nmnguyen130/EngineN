#include "renderer/RenderList.h"
#include <cstring>

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

[[nodiscard]] constexpr Color ScalePremultiplied(Color c, f32 factor) noexcept {
    const f32 f = std::clamp(factor, 0.0f, 1.0f);
    return Color{c.r * f, c.g * f, c.b * f, c.a * f};
}

[[nodiscard]] constexpr Color UnpackRGBA8(u32 packed) noexcept {
    constexpr f32 kInv = 1.0f / 255.0f;
    return Color{
        static_cast<f32>((packed >> 24U) & 0xFFU) * kInv,
        static_cast<f32>((packed >> 16U) & 0xFFU) * kInv,
        static_cast<f32>((packed >> 8U) & 0xFFU) * kInv,
        static_cast<f32>(packed & 0xFFU) * kInv,
    };
}

[[nodiscard]] f32 RaySegmentIntersectDist(Vec2 orig, Vec2 dir, Vec2 a, Vec2 b, f32 maxDist) noexcept {
    const Vec2 v1 = orig - a;
    const Vec2 v2 = b - a;
    const Vec2 v3{-dir.y, dir.x};
    const f32 dot = v2.Dot(v3);
    if (std::abs(dot) < 1e-6f) return maxDist;
    const f32 t1 = v2.Cross(v1) / dot;
    const f32 t2 = v1.Dot(v3) / dot;
    if (t1 >= 0.0f && t1 < maxDist && t2 >= 0.0f && t2 <= 1.0f) {
        return t1;
    }
    return maxDist;
}

} // namespace

void RenderList::PushQuad(const QuadCmd& cmd) noexcept {
    const u8 passId = cmd.passId & 0x0FU;
    if ((m_enabledPassMask & (1U << passId)) == 0U ||
        cmd.size.x == 0.0f || cmd.size.y == 0.0f || cmd.color.a <= 0.0f) {
        ++m_stats.culledQuads;
        return;
    }

    if (m_stats.submittedQuads >= kMaxQuads) {
        ++m_stats.droppedQuads;
        return;
    }

    const u8 viewSlot = cmd.viewSlot & 0x0FU;
    const Camera2D& cam = m_views[viewSlot];
    const f32 vpW = cam.viewport.w * static_cast<f32>(m_viewport.w);
    const f32 vpH = cam.viewport.h * static_cast<f32>(m_viewport.h);
    if (vpW <= 0.0f || vpH <= 0.0f) {
        ++m_stats.culledQuads;
        return;
    }

    f32 cx = cmd.center.x;
    f32 cy = cmd.center.y;
    f32 w = cmd.size.x;
    f32 h = cmd.size.y;

    const f32 ySign = (!cam.screenSpace && cam.yUp) ? -1.0f : 1.0f;
    if (cam.screenSpace) {
        w *= cam.zoom;
        h *= cam.zoom;
    } else {
        const f32 scale = cam.EffectiveScale();
        const f32 dx = (cmd.center.x - cam.worldCenterX) * scale;
        const f32 dy = (cmd.center.y - cam.worldCenterY) * scale;
        cx = vpW * 0.5f + dx;
        cy = vpH * 0.5f + dy * ySign;
        w *= scale;
        h *= scale;
    }

    const f32 hw = w * 0.5f;
    const f32 hh = h * 0.5f;
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

    // Tight rotated screen-space AABB frustum & scissor culling
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
    m_activePassMask |= static_cast<u16>(1U << passId);

    const u64 key = SortKey::Pack(cmd, qIdx);
    if (qIdx > 0U) {
        if (key < m_prevKey) m_isMonotonic = false;
        m_varyingBits |= (key ^ m_prevKey);
    }
    m_prevKey = key;
    m_keys[qIdx] = key;

    Color finalColor = cmd.color;
    if (cmd.flash8 > 0U) {
        const f32 f = static_cast<f32>(cmd.flash8) * (1.0f / 255.0f);
        const f32 targetWhite = finalColor.a;
        finalColor.r = finalColor.r + (targetWhite - finalColor.r) * f;
        finalColor.g = finalColor.g + (targetWhite - finalColor.g) * f;
        finalColor.b = finalColor.b + (targetWhite - finalColor.b) * f;
    }

    PreparedQuad& pq = m_quads[qIdx];
    pq.color = finalColor;
    pq.cx = cx;
    pq.cy = cy;
    pq.d0x = d0x;
    pq.d0y = d0y;
    pq.d1x = d1x;
    pq.d1y = d1y;
    pq.u0 = cmd.flipX ? cmd.uv.Right() : cmd.uv.x;
    pq.u1 = cmd.flipX ? cmd.uv.x : cmd.uv.Right();
    pq.v0 = cmd.flipY ? cmd.uv.Bottom() : cmd.uv.y;
    pq.v1 = cmd.flipY ? cmd.uv.y : cmd.uv.Bottom();
    pq.texture = cmd.texture.IsValid() ? cmd.texture : kWhiteTexture;
    pq.texture.index &= PreparedQuad::kTexIndexMask;
    pq.c1Rgba8 = 0U;
}

void RenderList::PushHitFlashQuad(
    const QuadCmd& baseCmd,
    f32 flashRatio,
    Color flashTint) noexcept {
    PushQuad(baseCmd);
    const f32 clamped = std::clamp(flashRatio, 0.0f, 1.0f);
    if (clamped <= 0.01f) return;

    QuadCmd flashCmd = baseCmd;
    flashCmd.color = ScalePremultiplied(flashTint, clamped * baseCmd.color.a);
    flashCmd.blend = BlendMode::Additive;
    flashCmd.depthBias = static_cast<i16>(
        std::clamp(static_cast<i32>(baseCmd.depthBias) + 1, -32768, 32767));
    PushQuad(flashCmd);
}

void RenderList::PushRect(
    Rect rect,
    Color color,
    u8 passId,
    u8 layer,
    u16 depth,
    u8 viewSlot,
    BlendMode blend) noexcept {
    if (rect.IsEmpty()) return;
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

void RenderList::PushGradientRect(
    Rect rect,
    Color startColor,
    Color endColor,
    bool vertical,
    u8 passId,
    u8 layer,
    u16 depth,
    u8 viewSlot,
    BlendMode blend) noexcept {
    if (rect.IsEmpty()) return;
    const u32 beforeCount = m_stats.submittedQuads;
    QuadCmd cmd{};
    cmd.center = rect.Center();
    cmd.size = {rect.w, rect.h};
    cmd.color = (startColor.a > 0.0f) ? startColor : Color{0.0f, 0.0f, 0.0f, 1e-4f};
    cmd.passId = passId;
    cmd.layer = layer;
    cmd.depth = depth;
    cmd.viewSlot = viewSlot;
    cmd.blend = blend;
    PushQuad(cmd);

    if (m_stats.submittedQuads > beforeCount) {
        PreparedQuad& pq = m_quads[beforeCount];
        pq.color = startColor;
        pq.texture.index |= PreparedQuad::kGradActiveBit |
                            (vertical ? 0U : PreparedQuad::kGradHorizontalBit);
        pq.c1Rgba8 = endColor.ToRGBA8();
    }
}

void RenderList::PushTiledRect(
    Rect rect,
    TextureHandle texture,
    Vec2 tileSize,
    Rect uvRect,
    Color color,
    u8 passId,
    u8 layer,
    u16 depth,
    u8 viewSlot) noexcept {
    if (rect.IsEmpty() || tileSize.x <= 0.01f || tileSize.y <= 0.01f) return;

    QuadCmd cmd{};
    cmd.color = color;
    cmd.texture = texture.IsValid() ? texture : kWhiteTexture;
    cmd.passId = passId;
    cmd.layer = layer;
    cmd.depth = depth;
    cmd.viewSlot = viewSlot;

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
    Rect rect,
    f32 thickness,
    Color color,
    u8 passId,
    u8 layer,
    u16 depth,
    u8 viewSlot) noexcept {
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
    f32 x0,
    f32 y0,
    f32 x1,
    f32 y1,
    f32 thickness,
    Color color,
    u8 passId,
    u8 layer,
    u16 depth,
    u8 viewSlot) noexcept {
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq <= 1e-10f || thickness <= 0.0f) return;
    const f32 len = std::sqrt(lenSq);
    const f32 invLen = 1.0f / len;

    QuadCmd cmd{};
    cmd.center = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f};
    cmd.size = {len, thickness};
    cmd.cosRot = dx * invLen;
    cmd.sinRot = dy * invLen;
    cmd.color = color;
    cmd.passId = passId;
    cmd.layer = layer;
    cmd.depth = depth;
    cmd.viewSlot = viewSlot;
    PushQuad(cmd);
}

void RenderList::PushPolyline(
    std::span<const Vec2> points,
    f32 thickness,
    Color color,
    bool closed,
    u8 passId,
    u8 layer,
    u16 depth,
    u8 viewSlot) noexcept {
    if (points.size() < 2U || thickness <= 0.0f) return;
    for (usize i = 0U; i + 1U < points.size(); ++i) {
        PushLine(points[i], points[i + 1U], thickness, color, passId, layer, depth, viewSlot);
    }
    if (closed && points.size() >= 3U) {
        PushLine(points.back(), points.front(), thickness, color, passId, layer, depth, viewSlot);
    }
}

void RenderList::PushCircleOutline(
    Vec2 center,
    f32 radius,
    u32 segments,
    f32 thickness,
    Color color,
    u8 passId,
    u8 layer,
    u8 viewSlot) noexcept {
    if (radius <= 0.0f || thickness <= 0.0f) return;
    const u32 segs = std::clamp(segments, 6U, 64U);
    const f32 step = kTwoPi / static_cast<f32>(segs);
    Vec2 prev{center.x + radius, center.y};
    for (u32 i = 1U; i <= segs; ++i) {
        f32 c = 1.0f;
        f32 s = 0.0f;
        FastSinCos(static_cast<f32>(i) * step, c, s);
        const Vec2 cur{center.x + c * radius, center.y + s * radius};
        PushLine(prev, cur, thickness, color, passId, layer, 32768U, viewSlot);
        prev = cur;
    }
}

void RenderList::PushSlashArc(
    Vec2 center,
    f32 radius,
    f32 startAngleRad,
    f32 sweepAngleRad,
    f32 thickness,
    Color color,
    u8 passId,
    u8 layer,
    u8 viewSlot) noexcept {
    if (radius <= 0.0f || thickness <= 0.0f || std::abs(sweepAngleRad) <= 1e-4f) return;
    constexpr u32 kArcSegs = 8U;
    const f32 step = sweepAngleRad / static_cast<f32>(kArcSegs);
    f32 c0 = 1.0f;
    f32 s0 = 0.0f;
    FastSinCos(startAngleRad, c0, s0);
    Vec2 prev{center.x + c0 * radius, center.y + s0 * radius};
    for (u32 i = 1U; i <= kArcSegs; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kArcSegs);
        f32 c = 1.0f;
        f32 s = 0.0f;
        FastSinCos(startAngleRad + static_cast<f32>(i) * step, c, s);
        const Vec2 cur{center.x + c * radius, center.y + s * radius};
        f32 tapSin = 0.0f;
        f32 tapCos = 1.0f;
        FastSinCos(t * kPi, tapCos, tapSin);
        const Color segCol = ScalePremultiplied(color, 0.3f + 0.7f * t);
        PushLine(prev, cur, thickness * (0.4f + 0.6f * tapSin), segCol, passId, layer, 32768U, viewSlot);
        prev = cur;
    }
}

void RenderList::PushBeam(
    Vec2 start,
    Vec2 end,
    f32 width,
    Color coreColor,
    Color glowColor,
    u8 passId,
    u8 layer,
    u8 viewSlot) noexcept {
    PushLine(start, end, width * 2.2f, glowColor, passId, layer, 32767U, viewSlot);
    PushLine(start, end, width, coreColor, passId, layer, 32768U, viewSlot);
}

void RenderList::PushRing(
    Vec2 center,
    f32 innerRadius,
    f32 outerRadius,
    Color color,
    u8 passId,
    u8 layer,
    u8 viewSlot) noexcept {
    if (outerRadius <= innerRadius || outerRadius <= 0.0f) return;
    const f32 midRadius = 0.5f * (innerRadius + outerRadius);
    const f32 thick = outerRadius - innerRadius;
    PushCircleOutline(center, midRadius, 20U, thick, color, passId, layer, viewSlot);
}

void RenderList::PushPointLight(
    Vec2 worldPos,
    f32 radius,
    Color color,
    f32 intensity,
    u8 passId,
    u8 layer,
    u8 viewSlot) noexcept {
    if (radius <= 0.01f || intensity <= 0.01f) return;
    const f32 outerGain = std::clamp(intensity * 0.35f, 0.0f, 1.0f);
    const f32 innerGain = std::clamp(intensity * 0.55f, 0.0f, 1.0f);

    QuadCmd outerCmd{};
    outerCmd.center = worldPos;
    outerCmd.size = {radius * 2.0f, radius * 2.0f};
    outerCmd.color = ScalePremultiplied(color, outerGain);
    outerCmd.texture = kBuiltinSoftLightTexture;
    outerCmd.passId = passId;
    outerCmd.layer = layer;
    outerCmd.viewSlot = viewSlot;
    outerCmd.blend = BlendMode::Additive;
    PushQuad(outerCmd);

    QuadCmd innerCmd = outerCmd;
    innerCmd.size = {radius * 0.9f, radius * 0.9f};
    innerCmd.color = ScalePremultiplied(color, innerGain);
    innerCmd.depthBias = 1;
    PushQuad(innerCmd);
}

void RenderList::PushNineSlice(
    const NineSliceDesc& desc,
    u8 passId,
    u8 layer,
    u8 viewSlot) noexcept {
    const Rect& r = desc.outerRect;
    if (r.IsEmpty()) return;

    const f32 cs = std::max(0.0f, desc.cornerScale);
    f32 bL = std::max(0.0f, desc.borderLeft * cs);
    f32 bR = std::max(0.0f, desc.borderRight * cs);
    f32 bT = std::max(0.0f, desc.borderTop * cs);
    f32 bB = std::max(0.0f, desc.borderBottom * cs);

    if (bL + bR > r.w && (bL + bR) > 0.0f) {
        const f32 scaleX = r.w / (bL + bR);
        bL *= scaleX;
        bR *= scaleX;
    }
    if (bT + bB > r.h && (bT + bB) > 0.0f) {
        const f32 scaleY = r.h / (bT + bB);
        bT *= scaleY;
        bB *= scaleY;
    }

    const f32 xCols[4]{r.x, r.x + bL, r.Right() - bR, r.Right()};
    const f32 yRows[4]{r.y, r.y + bT, r.Bottom() - bB, r.Bottom()};

    const f32 uvInset = std::clamp(desc.uvBorder, 0.0f, 0.5f);
    const f32 uCols[4]{0.0f, uvInset, 1.0f - uvInset, 1.0f};
    const f32 vRows[4]{0.0f, uvInset, 1.0f - uvInset, 1.0f};

    QuadCmd patch{};
    patch.color = desc.color;
    patch.texture = desc.texture.IsValid() ? desc.texture : kWhiteTexture;
    patch.depth = desc.depth;
    patch.passId = passId;
    patch.layer = layer;
    patch.viewSlot = viewSlot;
    patch.blend = desc.blend;

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

            const f32 ux = uCols[col];
            const f32 uw = uCols[col + 1U] - ux;

            patch.center = {px + pw * 0.5f, py + ph * 0.5f};
            patch.size = {pw, ph};
            patch.uv = {ux, vy, uw, vh};
            PushQuad(patch);
        }
    }
}

void RenderList::PushProgressBar(
    Rect rect,
    f32 ratio,
    Color fillColor,
    Color backColor,
    Color borderColor,
    u8 passId,
    u8 layer,
    u8 viewSlot) noexcept {
    if (rect.IsEmpty()) return;
    PushRect(rect, backColor, passId, layer, 32768U, viewSlot);
    const f32 clamped = std::clamp(ratio, 0.0f, 1.0f);
    if (clamped > 0.0f) {
        PushRect({rect.x, rect.y, rect.w * clamped, rect.h}, fillColor, passId, static_cast<u8>(layer + 1U), 32768U, viewSlot);
    }
    PushRectOutline(rect, 1.5f, borderColor, passId, static_cast<u8>(layer + 2U), 32768U, viewSlot);
}

void RenderList::PushText(
    Vec2 pos,
    std::string_view text,
    Color color,
    f32 scale,
    u8 passId,
    u8 layer,
    u8 viewSlot,
    TextureHandle fontTex) noexcept {
    if (text.empty() || scale <= 0.0f) return;
    const f32 quadSize = 16.0f * scale;
    const f32 advanceX = 12.0f * scale;
    f32 cursorX = pos.x;
    f32 cursorY = pos.y;
    const TextureHandle activeFont =
        (!fontTex.IsValid() || fontTex == kWhiteTexture) ? kBuiltinFontTexture : fontTex;

    QuadCmd cmd{};
    cmd.size = {quadSize, quadSize};
    cmd.color = color;
    cmd.texture = activeFont;
    cmd.passId = passId;
    cmd.layer = layer;
    cmd.viewSlot = viewSlot;

    for (const char c : text) {
        if (c == '\n') {
            cursorX = pos.x;
            cursorY += quadSize * 1.15f;
            continue;
        }
        const u8 ch = static_cast<u8>(c);
        if (ch > 32U && ch <= 126U) {
            const u8 glyph = static_cast<u8>(ch - 32U);
            const f32 u0 = static_cast<f32>(glyph & 15U) * (1.0f / 16.0f);
            const f32 v0 = static_cast<f32>(glyph >> 4U) * (1.0f / 6.0f);
            cmd.center = {cursorX + quadSize * 0.5f, cursorY + quadSize * 0.5f};
            cmd.uv = {u0, v0, 7.0f / 128.0f, 7.2f / 48.0f};
            PushQuad(cmd);
        }
        cursorX += advanceX;
    }
}

u32 RenderList::ComputeShadowVisibilityPolygon(
    Vec2 lightPos,
    f32 lightRadius,
    std::span<const ShadowOccluder2D> occluders,
    std::span<Vec2> outPolygonVertices) noexcept {
    if (outPolygonVertices.empty() || lightRadius <= 0.0f) return 0U;
    const u32 rayCount = std::min<u32>(static_cast<u32>(outPolygonVertices.size()), 64U);
    const f32 step = kTwoPi / static_cast<f32>(rayCount);

    for (u32 i = 0U; i < rayCount; ++i) {
        f32 c = 1.0f;
        f32 s = 0.0f;
        FastSinCos(static_cast<f32>(i) * step, c, s);
        const Vec2 dir{c, s};
        f32 bestDist = lightRadius;
        for (const ShadowOccluder2D& occ : occluders) {
            bestDist = RaySegmentIntersectDist(lightPos, dir, occ.a, occ.b, bestDist);
        }
        outPolygonVertices[i] = lightPos + dir * bestDist;
    }
    return rayCount;
}

void RenderList::Compile() noexcept {
    if (m_compiled) return;
    m_compiled = true;

    const u32 count = m_stats.submittedQuads;
    if (count == 0U) return;

    m_stats.monotonicFastPath = m_isMonotonic;
    m_stats.radixPassesRun = 0U;

    if (!m_isMonotonic && count > 1U) {
        const u64 varyingSortBits = m_varyingBits & SortKey::kSortStateMask;
        std::array<u32, 6> activeShifts{};
        u8 numPasses = 0U;
        for (const u32 shift : {16U, 24U, 32U, 40U, 48U, 56U}) {
            if (((varyingSortBits >> shift) & 0xFFULL) != 0ULL) {
                activeShifts[numPasses++] = shift;
            }
        }

        if (numPasses > 0U) {
            std::array<std::array<u32, 256>, 6> histograms{};
            for (u32 i = 0U; i < count; ++i) {
                const u64 k = m_keys[i];
                for (u8 p = 0U; p < numPasses; ++p) {
                    ++histograms[p][(k >> activeShifts[p]) & 0xFFU];
                }
            }

            for (u8 p = 0U; p < numPasses; ++p) {
                u32 prefix = 0U;
                for (u32 b = 0U; b < 256U; ++b) {
                    const u32 c = histograms[p][b];
                    histograms[p][b] = prefix;
                    prefix += c;
                }
            }

            u64* src = m_keys.data();
            u64* dst = m_scratchKeys.data();
            for (u8 p = 0U; p < numPasses; ++p) {
                const u32 shift = activeShifts[p];
                auto& hist = histograms[p];
                for (u32 i = 0U; i < count; ++i) {
                    const u64 k = src[i];
                    dst[hist[(k >> shift) & 0xFFU]++] = k;
                }
                std::swap(src, dst);
            }

            if (src != m_keys.data()) {
                std::memcpy(m_keys.data(), src, static_cast<usize>(count) * sizeof(u64));
            }
            m_stats.radixPassesRun = numPasses;
        }
    }

    // Coalesce sorted runs sharing identical (Pass, Blend, View, TextureIdx) without merging incompatible overflow batches
    u32 batchIdx = 0U;
    u64 curState = m_keys[0] & SortKey::kBatchStateMask;
    u32 batchStart = 0U;

    for (u32 i = 1U; i < count; ++i) {
        const u64 kState = m_keys[i] & SortKey::kBatchStateMask;
        if (kState != curState) {
            if (batchIdx < kMaxBatches) {
                const PreparedQuad& firstQ = m_quads[SortKey::QuadIndex(m_keys[batchStart])];
                const TextureHandle cleanTex{
                    static_cast<u16>(firstQ.texture.index & PreparedQuad::kTexIndexMask),
                    firstQ.texture.generation};
                m_batches[batchIdx++] = DrawBatch{
                    batchStart,
                    i - batchStart,
                    cleanTex,
                    SortKey::PassId(curState),
                    SortKey::ViewSlot(curState),
                    SortKey::Blend(curState),
                    SortKey::Layer(m_keys[batchStart]),
                };
            }
            curState = kState;
            batchStart = i;
        }
    }

    if (batchIdx < kMaxBatches) {
        const PreparedQuad& firstQ = m_quads[SortKey::QuadIndex(m_keys[batchStart])];
        const TextureHandle cleanTex{
            static_cast<u16>(firstQ.texture.index & PreparedQuad::kTexIndexMask),
            firstQ.texture.generation};
        m_batches[batchIdx++] = DrawBatch{
            batchStart,
            count - batchStart,
            cleanTex,
            SortKey::PassId(curState),
            SortKey::ViewSlot(curState),
            SortKey::Blend(curState),
            SortKey::Layer(m_keys[batchStart]),
        };
    }

    m_stats.drawBatches = batchIdx;
}

void RenderList::ExpandVertices(
    u32 firstQuad,
    u32 quadCount,
    std::span<Vertex2D> outVertices) const noexcept {
    const u32 limit = std::min(firstQuad + quadCount, m_stats.submittedQuads);
    usize vOut = 0U;
    for (u32 i = firstQuad; i < limit && vOut + 4U <= outVertices.size(); ++i) {
        const u16 qIdx = SortKey::QuadIndex(m_keys[i]);
        const PreparedQuad& q = m_quads[qIdx];
        Color c0 = q.color;
        Color c1 = q.color;
        Color c2 = q.color;
        Color c3 = q.color;
        if ((q.texture.index & PreparedQuad::kGradActiveBit) != 0U) {
            const Color endCol = UnpackRGBA8(q.c1Rgba8);
            if ((q.texture.index & PreparedQuad::kGradHorizontalBit) != 0U) {
                c1 = endCol;
                c2 = endCol;
            } else {
                c2 = endCol;
                c3 = endCol;
            }
        }
        outVertices[vOut + 0U] = Vertex2D{q.cx - q.d0x, q.cy - q.d0y, c0, q.u0, q.v0};
        outVertices[vOut + 1U] = Vertex2D{q.cx + q.d1x, q.cy + q.d1y, c1, q.u1, q.v0};
        outVertices[vOut + 2U] = Vertex2D{q.cx + q.d0x, q.cy + q.d0y, c2, q.u1, q.v1};
        outVertices[vOut + 3U] = Vertex2D{q.cx - q.d1x, q.cy - q.d1y, c3, q.u0, q.v1};
        vOut += 4U;
    }
}

std::span<const u16> RenderList::SharedQuadIndices(u32 quadCount) noexcept {
    const u32 clampedQuads = std::min(quadCount, kChunkQuads);
    return std::span<const u16>(kSharedQuadIndices.data(), static_cast<usize>(clampedQuads) * 6U);
}

u64 RenderList::ComputeAuditHash() const noexcept {
    const u64 header = static_cast<u64>(m_stats.submittedQuads) |
                       (static_cast<u64>(m_stats.drawBatches) << 20U) |
                       (static_cast<u64>(m_stats.culledQuads) << 36U) |
                       (static_cast<u64>(m_stats.radixPassesRun) << 52U) |
                       (static_cast<u64>(m_stats.monotonicFastPath ? 1U : 0U) << 60U);
    u64 h = Mix64(header);
    for (u32 i = 0U; i < m_stats.submittedQuads; ++i) {
        const u64 k = m_keys[i];
        const PreparedQuad& q = m_quads[SortKey::QuadIndex(k)];
        h = HashCombine64(h, k);
        const u64 posBits = static_cast<u64>(std::bit_cast<u32>(q.cx)) |
                            (static_cast<u64>(std::bit_cast<u32>(q.cy)) << 32U);
        h = HashCombine64(h, posBits);
    }
    return h;
}

} // namespace en
