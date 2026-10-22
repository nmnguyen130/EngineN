#include "debug/DebugBatcher.h"

#include <charconv>
#include <cstring>

namespace en {

void DebugBatcher::PushSolidRect(Rect rect, Color color) noexcept {
    if (!m_list || rect.w <= 0.0f || rect.h <= 0.0f) return;
    QuadCmd cmd{};
    cmd.center = {rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f};
    cmd.size = {rect.w, rect.h};
    cmd.pivot = {0.5f, 0.5f};
    cmd.uv = {0.0f, 0.0f, 1.0f, 1.0f};
    cmd.color = color;
    cmd.texture = kWhiteTexture;
    cmd.depth = m_depth;
    cmd.passId = m_passId;
    cmd.layer = 250U;
    cmd.viewSlot = m_viewSlot;
    (void)m_list->PushQuad(cmd);
}

void DebugBatcher::PushBoxOutline(Rect rect, f32 thickness, Color color) noexcept {
    if (!m_list || rect.w <= 0.0f || rect.h <= 0.0f) return;
    const f32 x0 = rect.x;
    const f32 y0 = rect.y;
    const f32 x1 = rect.x + rect.w;
    const f32 y1 = rect.y + rect.h;
    PushLine(x0, y0, x1, y0, thickness, color);
    PushLine(x1, y0, x1, y1, thickness, color);
    PushLine(x1, y1, x0, y1, thickness, color);
    PushLine(x0, y1, x0, y0, thickness, color);
}

void DebugBatcher::PushObbOutline(
    Vec2 center,
    Vec2 halfExtents,
    f32 cosRot,
    f32 sinRot,
    f32 thickness,
    Color color) noexcept {
    const Vec2 ux{cosRot * halfExtents.x, sinRot * halfExtents.x};
    const Vec2 uy{-sinRot * halfExtents.y, cosRot * halfExtents.y};
    const Vec2 c0{center.x - ux.x - uy.x, center.y - ux.y - uy.y};
    const Vec2 c1{center.x + ux.x - uy.x, center.y + ux.y - uy.y};
    const Vec2 c2{center.x + ux.x + uy.x, center.y + ux.y + uy.y};
    const Vec2 c3{center.x - ux.x + uy.x, center.y - ux.y + uy.y};
    PushLine(c0.x, c0.y, c1.x, c1.y, thickness, color);
    PushLine(c1.x, c1.y, c2.x, c2.y, thickness, color);
    PushLine(c2.x, c2.y, c3.x, c3.y, thickness, color);
    PushLine(c3.x, c3.y, c0.x, c0.y, thickness, color);
}

void DebugBatcher::PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Color color) noexcept {
    if (!m_list) return;
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len <= 1e-5f) return;

    const f32 invLen = 1.0f / len;
    QuadCmd cmd{};
    cmd.center = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f};
    cmd.size = {len, std::max(1e-3f, thickness)};
    cmd.pivot = {0.5f, 0.5f};
    cmd.cosRot = dx * invLen;
    cmd.sinRot = dy * invLen;
    cmd.uv = {0.0f, 0.0f, 1.0f, 1.0f};
    cmd.color = color;
    cmd.texture = kWhiteTexture;
    cmd.depth = m_depth;
    cmd.passId = m_passId;
    cmd.layer = 251U;
    cmd.viewSlot = m_viewSlot;
    (void)m_list->PushQuad(cmd);
}

void DebugBatcher::PushArrow(
    f32 x0,
    f32 y0,
    f32 x1,
    f32 y1,
    f32 headSize,
    f32 thickness,
    Color color) noexcept {
    PushLine(x0, y0, x1, y1, thickness, color);
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len <= 1e-5f || headSize <= 1e-4f) return;

    const f32 ux = dx / len;
    const f32 uy = dy / len;
    const f32 px = -uy;
    const f32 py = ux;
    const f32 bx = x1 - ux * headSize;
    const f32 by = y1 - uy * headSize;
    const f32 wing = headSize * 0.55f;
    PushLine(x1, y1, bx + px * wing, by + py * wing, thickness, color);
    PushLine(x1, y1, bx - px * wing, by - py * wing, thickness, color);
}

void DebugBatcher::PushCircleOutline(
    f32 cx,
    f32 cy,
    f32 radius,
    u8 segments,
    f32 thickness,
    Color color) noexcept {
    if (!m_list || radius <= 1e-4f) return;
    const u32 segs = std::clamp<u32>(segments, 6U, 64U);
    const f32 stepRad = (2.0f * kPi) / static_cast<f32>(segs);
    f32 prevX = cx + radius;
    f32 prevY = cy;
    for (u32 i = 1U; i <= segs; ++i) {
        f32 s = 0.0f, c = 1.0f;
        FastSinCos(static_cast<f32>(i) * stepRad, c, s);
        const f32 nextX = cx + c * radius;
        const f32 nextY = cy + s * radius;
        PushLine(prevX, prevY, nextX, nextY, thickness, color);
        prevX = nextX;
        prevY = nextY;
    }
}

void DebugBatcher::PushCapsuleYOutline(
    f32 cx,
    f32 cy,
    f32 halfW,
    f32 halfH,
    f32 thickness,
    Color color) noexcept {
    const f32 r = std::max(1e-3f, halfW);
    const f32 spineHalf = std::max(0.0f, halfH - r);
    PushLine(cx - r, cy - spineHalf, cx - r, cy + spineHalf, thickness, color);
    PushLine(cx + r, cy - spineHalf, cx + r, cy + spineHalf, thickness, color);
    PushCircleOutline(cx, cy + spineHalf, r, 12U, thickness, color);
    PushCircleOutline(cx, cy - spineHalf, r, 12U, thickness, color);
}

void DebugBatcher::PushCrosshair(f32 cx, f32 cy, f32 halfSize, f32 thickness, Color color) noexcept {
    PushLine(cx - halfSize, cy, cx + halfSize, cy, thickness, color);
    PushLine(cx, cy - halfSize, cx, cy + halfSize, thickness, color);
}

void DebugBatcher::PushText(
    f32 x,
    f32 y,
    std::string_view text,
    Color color,
    f32 scale) noexcept {
    if (!m_list || text.empty() || scale <= 0.0f) return;
    const f32 glyphW = 8.0f * scale;
    const f32 glyphH = 8.0f * scale;
    const f32 advanceX = 6.0f * scale;
    constexpr f32 kInvCols = 1.0f / 16.0f;
    constexpr f32 kInvRows = 1.0f / 6.0f;

    f32 cursorX = x;
    f32 cursorY = y;
    for (char rawCh : text) {
        if (rawCh == '\n') {
            cursorX = x;
            cursorY += glyphH;
            continue;
        }
        const u8 ch = static_cast<u8>(rawCh);
        if (ch <= 32U || ch >= 128U) {
            cursorX += advanceX;
            continue;
        }
        const u32 glyphIdx = static_cast<u32>(ch - 32U);
        const f32 u0 = static_cast<f32>(glyphIdx & 15U) * kInvCols;
        const f32 v0 = static_cast<f32>(glyphIdx >> 4U) * kInvRows;

        QuadCmd cmd{};
        cmd.center = {cursorX + glyphW * 0.5f, cursorY + glyphH * 0.5f};
        cmd.size = {glyphW, glyphH};
        cmd.pivot = {0.5f, 0.5f};
        cmd.uv = {u0, v0, kInvCols, kInvRows};
        cmd.color = color;
        cmd.texture = m_fontTex;
        cmd.depth = static_cast<u16>(std::min<u32>(65535U, m_depth + 1U));
        cmd.passId = m_passId;
        cmd.layer = 254U;
        cmd.viewSlot = m_viewSlot;
        (void)m_list->PushQuad(cmd);

        cursorX += advanceX;
    }
}

char* DebugBatcher::FormatI64(char* out, char* end, i64 val) noexcept {
    if (!out || out >= end) return out;
    const auto res = std::to_chars(out, end, val);
    return (res.ec == std::errc{}) ? res.ptr : out;
}

char* DebugBatcher::FormatU64Hex(char* out, char* end, u64 val, u8 digits) noexcept {
    if (!out || out + digits + 2 > end) return out;
    *out++ = '0';
    *out++ = 'x';
    constexpr char kHex[] = "0123456789ABCDEF";
    for (i32 i = static_cast<i32>(digits) - 1; i >= 0; --i) {
        *out++ = kHex[(val >> (static_cast<u32>(i) * 4U)) & 0x0FU];
    }
    return out;
}

char* DebugBatcher::FormatF32Fixed2(char* out, char* end, f32 val) noexcept {
    if (!out || out >= end) return out;
    if (val < 0.0f) {
        *out++ = '-';
        val = -val;
    }
    const i64 scaled = static_cast<i64>(val * 100.0f + 0.5f);
    out = FormatI64(out, end, scaled / 100);
    if (out + 3 <= end) {
        const i64 frac = scaled % 100;
        *out++ = '.';
        *out++ = static_cast<char>('0' + (frac / 10));
        *out++ = static_cast<char>('0' + (frac % 10));
    }
    return out;
}

DebugShapeCmd* DebugShapePool::AllocateSlot(u32 durationTicks) noexcept {
    if (!m_pool) return nullptr;
    const u64 seq = m_totalSubmitted++;
    if (m_nonFullWordMask == 0U) {
        // Pool full: round-robin overwrite across all 2048 slots without incrementing m_activeCount
        DebugShapeCmd& cmd = m_pool[seq & (kMaxDebugShapes - 1U)];
        cmd = DebugShapeCmd{};
        cmd.expireTick = m_currentTick + durationTicks;
        return &cmd;
    }

    const u32 wordIdx = static_cast<u32>(std::countr_zero(m_nonFullWordMask));
    const u64 word = m_activeWords[wordIdx];
    const u32 bitIdx = static_cast<u32>(std::countr_one(word));
    const u64 bit = (1ULL << bitIdx);
    m_activeWords[wordIdx] = word | bit;
    m_nonEmptyWordMask |= (1U << wordIdx);
    if (m_activeWords[wordIdx] == ~0ULL) {
        m_nonFullWordMask &= ~(1U << wordIdx);
    }
    ++m_activeCount;

    DebugShapeCmd& cmd = m_pool[(wordIdx << 6U) + bitIdx];
    cmd = DebugShapeCmd{};
    cmd.expireTick = m_currentTick + durationTicks;
    return &cmd;
}

void DebugShapePool::AddLine(
    f32 x0,
    f32 y0,
    f32 x1,
    f32 y1,
    Color color,
    f32 thickness,
    u32 durationTicks) noexcept {
    DebugShapeCmd* cmd = AllocateSlot(durationTicks);
    if (!cmd) return;
    cmd->kind = DebugShapeKind::Line;
    cmd->x0 = x0;
    cmd->y0 = y0;
    cmd->payload.geom = {x1, y1, 0.0f};
    cmd->rgba8 = DebugBatcher::PackRgba8(color);
    cmd->thicknessQ8 = static_cast<u16>(std::clamp(thickness * 256.0f + 0.5f, 1.0f, 65535.0f));
}

void DebugShapePool::AddBox(
    Rect rect,
    Color color,
    f32 thickness,
    bool solid,
    u32 durationTicks) noexcept {
    DebugShapeCmd* cmd = AllocateSlot(durationTicks);
    if (!cmd) return;
    cmd->kind = solid ? DebugShapeKind::SolidBox : DebugShapeKind::Box;
    cmd->x0 = rect.x;
    cmd->y0 = rect.y;
    cmd->payload.geom = {rect.w, rect.h, 0.0f};
    cmd->rgba8 = DebugBatcher::PackRgba8(color);
    cmd->thicknessQ8 = static_cast<u16>(std::clamp(thickness * 256.0f + 0.5f, 1.0f, 65535.0f));
}

void DebugShapePool::AddArrow(
    f32 x0,
    f32 y0,
    f32 x1,
    f32 y1,
    f32 headSize,
    Color color,
    f32 thickness,
    u32 durationTicks) noexcept {
    DebugShapeCmd* cmd = AllocateSlot(durationTicks);
    if (!cmd) return;
    cmd->kind = DebugShapeKind::Arrow;
    cmd->x0 = x0;
    cmd->y0 = y0;
    cmd->payload.geom = {x1, y1, headSize};
    cmd->rgba8 = DebugBatcher::PackRgba8(color);
    cmd->thicknessQ8 = static_cast<u16>(std::clamp(thickness * 256.0f + 0.5f, 1.0f, 65535.0f));
}

void DebugShapePool::AddCircle(
    f32 cx,
    f32 cy,
    f32 radius,
    Color color,
    u8 segments,
    f32 thickness,
    u32 durationTicks) noexcept {
    DebugShapeCmd* cmd = AllocateSlot(durationTicks);
    if (!cmd) return;
    cmd->kind = DebugShapeKind::Circle;
    cmd->x0 = cx;
    cmd->y0 = cy;
    cmd->payload.geom = {radius, 0.0f, 0.0f};
    cmd->rgba8 = DebugBatcher::PackRgba8(color);
    cmd->thicknessQ8 = static_cast<u16>(std::clamp(thickness * 256.0f + 0.5f, 1.0f, 65535.0f));
    cmd->auxByte = segments;
}

void DebugShapePool::AddCapsuleY(
    f32 cx,
    f32 cy,
    f32 halfW,
    f32 halfH,
    Color color,
    f32 thickness,
    u32 durationTicks) noexcept {
    DebugShapeCmd* cmd = AllocateSlot(durationTicks);
    if (!cmd) return;
    cmd->kind = DebugShapeKind::CapsuleY;
    cmd->x0 = cx;
    cmd->y0 = cy;
    cmd->payload.geom = {halfW, halfH, 0.0f};
    cmd->rgba8 = DebugBatcher::PackRgba8(color);
    cmd->thicknessQ8 = static_cast<u16>(std::clamp(thickness * 256.0f + 0.5f, 1.0f, 65535.0f));
}

void DebugShapePool::AddTextWorld(
    f32 x,
    f32 y,
    std::string_view text,
    Color color,
    f32 scale,
    u32 durationTicks) noexcept {
    DebugShapeCmd* cmd = AllocateSlot(durationTicks);
    if (!cmd) return;
    cmd->kind = DebugShapeKind::TextWorld;
    cmd->x0 = x;
    cmd->y0 = y;
    const usize copyLen = std::min<usize>(text.size(), 11U);
    std::memcpy(cmd->payload.text, text.data(), copyLen);
    cmd->payload.text[copyLen] = '\0';
    cmd->rgba8 = DebugBatcher::PackRgba8(color);
    cmd->thicknessQ8 = static_cast<u16>(std::clamp(scale * 256.0f + 0.5f, 1.0f, 65535.0f));
    cmd->auxByte = static_cast<u8>(copyLen);
}

void DebugShapePool::AddTextScreen(
    f32 x,
    f32 y,
    std::string_view text,
    Color color,
    f32 scale,
    u32 durationTicks) noexcept {
    DebugShapeCmd* cmd = AllocateSlot(durationTicks);
    if (!cmd) return;
    cmd->kind = DebugShapeKind::TextScreen;
    cmd->x0 = x;
    cmd->y0 = y;
    const usize copyLen = std::min<usize>(text.size(), 11U);
    std::memcpy(cmd->payload.text, text.data(), copyLen);
    cmd->payload.text[copyLen] = '\0';
    cmd->rgba8 = DebugBatcher::PackRgba8(color);
    cmd->thicknessQ8 = static_cast<u16>(std::clamp(scale * 256.0f + 0.5f, 1.0f, 65535.0f));
    cmd->auxByte = static_cast<u8>(copyLen);
}

void DebugShapePool::EmitAndExpire(
    u32 currentTickIndex,
    DebugBatcher& worldBatcher,
    DebugBatcher& hudBatcher) noexcept {
    if (!m_pool) return;
    m_currentTick = currentTickIndex;

    u32 wordMask = m_nonEmptyWordMask;
    while (wordMask != 0U) {
        const u32 wordIdx = static_cast<u32>(std::countr_zero(wordMask));
        wordMask &= (wordMask - 1U);

        u64 activeBits = m_activeWords[wordIdx];
        u64 survivingBits = activeBits;
        while (activeBits != 0ULL) {
            const u32 bitIdx = static_cast<u32>(std::countr_zero(activeBits));
            activeBits &= (activeBits - 1ULL);

            const DebugShapeCmd& cmd = m_pool[(wordIdx << 6U) + bitIdx];
            const Color col = DebugBatcher::UnpackRgba8(cmd.rgba8);
            const f32 thick = static_cast<f32>(cmd.thicknessQ8) * (1.0f / 256.0f);

            switch (cmd.kind) {
                case DebugShapeKind::Line:
                    worldBatcher.PushLine(cmd.x0, cmd.y0, cmd.payload.geom.x1, cmd.payload.geom.y1, thick, col);
                    break;
                case DebugShapeKind::Box:
                    worldBatcher.PushBoxOutline({cmd.x0, cmd.y0, cmd.payload.geom.x1, cmd.payload.geom.y1}, thick, col);
                    break;
                case DebugShapeKind::SolidBox:
                    worldBatcher.PushSolidRect({cmd.x0, cmd.y0, cmd.payload.geom.x1, cmd.payload.geom.y1}, col);
                    break;
                case DebugShapeKind::Arrow:
                    worldBatcher.PushArrow(
                        cmd.x0, cmd.y0, cmd.payload.geom.x1, cmd.payload.geom.y1,
                        cmd.payload.geom.auxFloat, thick, col);
                    break;
                case DebugShapeKind::Circle:
                    worldBatcher.PushCircleOutline(cmd.x0, cmd.y0, cmd.payload.geom.x1, cmd.auxByte, thick, col);
                    break;
                case DebugShapeKind::CapsuleY:
                    worldBatcher.PushCapsuleYOutline(
                        cmd.x0, cmd.y0, cmd.payload.geom.x1, cmd.payload.geom.y1, thick, col);
                    break;
                case DebugShapeKind::TextWorld:
                    worldBatcher.PushText(
                        cmd.x0, cmd.y0, std::string_view{cmd.payload.text, cmd.auxByte}, col, thick);
                    break;
                case DebugShapeKind::TextScreen:
                    hudBatcher.PushText(
                        cmd.x0, cmd.y0, std::string_view{cmd.payload.text, cmd.auxByte}, col, thick);
                    break;
            }

            if (currentTickIndex >= cmd.expireTick) {
                survivingBits &= ~(1ULL << bitIdx);
                --m_activeCount;
            }
        }

        m_activeWords[wordIdx] = survivingBits;
        if (survivingBits == 0ULL) {
            m_nonEmptyWordMask &= ~(1U << wordIdx);
        }
        if (survivingBits != ~0ULL) {
            m_nonFullWordMask |= (1U << wordIdx);
        }
    }
}

u64 DebugShapePool::ComputeAuditHash() const noexcept {
    u64 h = 14695981039346656037ULL;
    h ^= static_cast<u64>(m_activeCount);
    h *= 1099511628211ULL;
    h ^= static_cast<u64>(m_nonEmptyWordMask);
    h *= 1099511628211ULL;
    h ^= m_totalSubmitted;
    h *= 1099511628211ULL;
    return h;
}

} // namespace en
