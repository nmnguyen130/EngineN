#include "debug/DebugBatcher.h"
#include "renderer/RendererList.h"

#include <algorithm>
#include <cmath>

namespace en::debug {

namespace {

// 16-segment unit circle lookup table to avoid std::sin / std::cos during CircleOutline emission.
struct UnitVec2 { f32 x; f32 y; };
inline constexpr std::array<UnitVec2, 16> kUnitCircle16 = {{
    { 1.0000000f,  0.0000000f}, { 0.9238795f,  0.3826834f},
    { 0.7071068f,  0.7071068f}, { 0.3826834f,  0.9238795f},
    { 0.0000000f,  1.0000000f}, {-0.3826834f,  0.9238795f},
    {-0.7071068f,  0.7071068f}, {-0.9238795f,  0.3826834f},
    {-1.0000000f,  0.0000000f}, {-0.9238795f, -0.3826834f},
    {-0.7071068f, -0.7071068f}, {-0.3826834f, -0.9238795f},
    { 0.0000000f, -1.0000000f}, { 0.3826834f, -0.9238795f},
    { 0.7071068f, -0.7071068f}, { 0.9238795f, -0.3826834f}
}};

[[nodiscard]] constexpr u16 PackThicknessQ8(f32 thickness) noexcept {
    const f32 clamped = std::clamp(thickness, 0.05f, 255.0f);
    return static_cast<u16>(clamped * 256.0f + 0.5f);
}

[[nodiscard]] constexpr f32 UnpackThicknessQ8(u16 q8) noexcept {
    return static_cast<f32>(q8) * (1.0f / 256.0f);
}

} // namespace

DebugBatcher::DebugBatcher(renderer::RenderList& list,
                           renderer::TextureHandle debugAtlas,
                           u8 passId,
                           u8 viewSlot) noexcept
    : m_list(list) {
    m_templateQuad.cosRot   = 1.0f;
    m_templateQuad.sinRot   = 0.0f;
    m_templateQuad.uv       = kSolidWhiteUv;
    m_templateQuad.texture  = debugAtlas;
    m_templateQuad.depth    = kDebugFixedDepth;
    m_templateQuad.passId   = passId;
    m_templateQuad.layer    = 0;
    m_templateQuad.viewSlot = viewSlot;
    m_templateQuad.blend    = renderer::BlendMode::PremultipliedAlpha;
    m_templateQuad.flipX    = false;
    m_templateQuad.flipY    = false;
}

void DebugBatcher::PushSolidRect(renderer::FRect rect, renderer::Color color) noexcept {
    renderer::QuadCmd cmd = m_templateQuad;
    cmd.x     = rect.x + rect.w * 0.5f;
    cmd.y     = rect.y + rect.h * 0.5f;
    cmd.w     = rect.w;
    cmd.h     = rect.h;
    cmd.color = color;
    m_list.PushQuad(cmd);
}

void DebugBatcher::PushBoxOutline(renderer::FRect rect, f32 thickness, renderer::Color color) noexcept {
    const f32 t = std::max(thickness, 0.01f);
    PushSolidRect({rect.x, rect.y, rect.w, t}, color);
    PushSolidRect({rect.x, rect.y + rect.h - t, rect.w, t}, color);
    PushSolidRect({rect.x, rect.y + t, t, std::max(0.0f, rect.h - 2.0f * t)}, color);
    PushSolidRect({rect.x + rect.w - t, rect.y + t, t, std::max(0.0f, rect.h - 2.0f * t)}, color);
}

void DebugBatcher::PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, renderer::Color color) noexcept {
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-8f) {
        return;
    }
    const f32 len = std::sqrt(lenSq);
    const f32 invLen = 1.0f / len;

    renderer::QuadCmd cmd = m_templateQuad;
    cmd.x      = (x0 + x1) * 0.5f;
    cmd.y      = (y0 + y1) * 0.5f;
    cmd.w      = len;
    cmd.h      = thickness;
    cmd.cosRot = dx * invLen;
    cmd.sinRot = dy * invLen;
    cmd.color  = color;
    m_list.PushQuad(cmd);
}

void DebugBatcher::PushArrow(f32 x0, f32 y0, f32 x1, f32 y1,
                             f32 headSize, f32 thickness, renderer::Color color) noexcept {
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-8f) {
        return;
    }
    const f32 invLen = 1.0f / std::sqrt(lenSq);
    const f32 ux = dx * invLen;
    const f32 uy = dy * invLen;
    const f32 px = -uy;
    const f32 py = ux;

    PushLine(x0, y0, x1, y1, thickness, color);
    const f32 bx = x1 - ux * headSize;
    const f32 by = y1 - uy * headSize;
    const f32 wing = headSize * 0.55f;
    PushLine(x1, y1, bx + px * wing, by + py * wing, thickness, color);
    PushLine(x1, y1, bx - px * wing, by - py * wing, thickness, color);
}

void DebugBatcher::PushCircleOutline(f32 cx, f32 cy, f32 radius,
                                     u8 segments, f32 thickness, renderer::Color color) noexcept {
    const u32 step = (segments <= 8) ? 2u : 1u;
    for (u32 i = 0; i < 16; i += step) {
        const u32 next = (i + step) & 15u;
        const f32 x0 = cx + kUnitCircle16[i].x * radius;
        const f32 y0 = cy + kUnitCircle16[i].y * radius;
        const f32 x1 = cx + kUnitCircle16[next].x * radius;
        const f32 y1 = cy + kUnitCircle16[next].y * radius;
        PushLine(x0, y0, x1, y1, thickness, color);
    }
}

void DebugBatcher::PushText(f32 x, f32 y, std::string_view text, renderer::Color color, f32 scale) noexcept {
    if (text.empty()) {
        return;
    }

    const f32 glyphW = kGlyphWidth * scale;
    const f32 glyphH = kGlyphHeight * scale;
    constexpr f32 kInvAtlasW = 1.0f / static_cast<f32>(kDebugAtlasWidth);
    constexpr f32 kInvAtlasH = 1.0f / static_cast<f32>(kDebugAtlasHeight);
    constexpr f32 kUvSpanU   = 8.0f * kInvAtlasW;
    constexpr f32 kUvSpanV   = 8.0f * kInvAtlasH;

    renderer::QuadCmd cmd = m_templateQuad;
    cmd.w     = glyphW;
    cmd.h     = glyphH;
    cmd.color = color;

    f32 cursorX = x + glyphW * 0.5f;
    f32 cursorY = y + glyphH * 0.5f;

    for (const char ch : text) {
        if (ch == '\n') {
            cursorX = x + glyphW * 0.5f;
            cursorY += glyphH + 2.0f * scale;
            continue;
        }
        const u8 code = static_cast<u8>(ch);
        if (code <= 0x20u || code > 0x7Fu) {
            cursorX += glyphW;
            continue;
        }

        const u32 glyphIdx = static_cast<u32>(code - 0x20u);
        const u32 col = glyphIdx & 15u;
        const u32 row = glyphIdx >> 4u;

        cmd.x  = cursorX;
        cmd.y  = cursorY;
        cmd.uv = {
            static_cast<f32>(col * 8u) * kInvAtlasW,
            static_cast<f32>(row * 8u) * kInvAtlasH,
            kUvSpanU,
            kUvSpanV
        };
        m_list.PushQuad(cmd);
        cursorX += glyphW;
    }
}

char* DebugBatcher::FormatI64(char* out, char* end, i64 val) noexcept {
    if (out >= end) {
        return out;
    }
    u64 uval = static_cast<u64>(val);
    if (val < 0) {
        *out++ = '-';
        uval = 0ull - uval;
        if (out >= end) {
            return out;
        }
    }
    std::array<char, 20> rev{};
    usize count = 0;
    do {
        rev[count++] = static_cast<char>('0' + (uval % 10ull));
        uval /= 10ull;
    } while (uval != 0ull && count < rev.size());

    while (count > 0 && out < end) {
        *out++ = rev[--count];
    }
    return out;
}

char* DebugBatcher::FormatU64Hex(char* out, char* end, u64 val, u8 digits) noexcept {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (i32 i = static_cast<i32>(digits) - 1; i >= 0 && out < end; --i) {
        *out++ = kHex[(val >> (i * 4)) & 0xFull];
    }
    return out;
}

char* DebugBatcher::FormatF32Fixed2(char* out, char* end, f32 val) noexcept {
    if (out >= end) {
        return out;
    }
    if (val < 0.0f) {
        *out++ = '-';
        val = -val;
        if (out >= end) {
            return out;
        }
    }
    const u64 scaled = static_cast<u64>(val * 100.0f + 0.5f);
    const u64 intPart = scaled / 100ull;
    const u64 fracPart = scaled % 100ull;

    out = FormatI64(out, end, static_cast<i64>(intPart));
    if (end - out >= 3) {
        *out++ = '.';
        *out++ = static_cast<char>('0' + (fracPart / 10ull));
        *out++ = static_cast<char>('0' + (fracPart % 10ull));
    }
    return out;
}

void DebugShapePool::BindArena(std::span<DebugShapeCmd, kMaxShapes> pool) noexcept {
    m_pool = pool;
    Clear();
}

void DebugShapePool::Clear() noexcept {
    m_activeWords.fill(0ull);
    m_allocCursor = 0;
}

DebugShapeCmd& DebugShapePool::AllocateSlot(u32 durationTicks) noexcept {
    for (usize i = 0; i < kWordCount; ++i) {
        const usize wordIdx = (m_allocCursor + i) & (kWordCount - 1u);
        const u64 word = m_activeWords[wordIdx];
        if (~word != 0ull) {
            const u32 bit = static_cast<u32>(std::countr_one(word));
            m_activeWords[wordIdx] |= (1ull << bit);
            m_allocCursor = static_cast<u32>(wordIdx);
            DebugShapeCmd& cmd = m_pool[wordIdx * 64u + bit];
            cmd = DebugShapeCmd{};
            cmd.expireTick = m_currentTickLow + std::max(1u, durationTicks);
            return cmd;
        }
    }

    // Pool is completely full: deterministically overwrite the roving slot.
    const u32 slotIdx = (m_allocCursor * 64u) & (kMaxShapes - 1u);
    m_allocCursor = (m_allocCursor + 1u) & (kWordCount - 1u);
    DebugShapeCmd& cmd = m_pool[slotIdx];
    cmd = DebugShapeCmd{};
    cmd.expireTick = m_currentTickLow + std::max(1u, durationTicks);
    return cmd;
}

void DebugShapePool::AddLine(f32 x0, f32 y0, f32 x1, f32 y1,
                             u32 rgba8, f32 thickness, u32 durationTicks) noexcept {
    if (m_pool.empty()) return;
    DebugShapeCmd& cmd = AllocateSlot(durationTicks);
    cmd.kind        = DebugShapeKind::Line;
    cmd.x0          = x0;
    cmd.y0          = y0;
    cmd.rgba8       = rgba8;
    cmd.thicknessQ8 = PackThicknessQ8(thickness);
    cmd.geom        = {x1, y1, 0.0f};
}

void DebugShapePool::AddBox(f32 cx, f32 cy, f32 halfW, f32 halfH,
                            u32 rgba8, bool filled, f32 thickness, u32 durationTicks) noexcept {
    if (m_pool.empty()) return;
    DebugShapeCmd& cmd = AllocateSlot(durationTicks);
    cmd.kind        = filled ? DebugShapeKind::BoxFilled : DebugShapeKind::BoxOutline;
    cmd.x0          = cx;
    cmd.y0          = cy;
    cmd.rgba8       = rgba8;
    cmd.thicknessQ8 = PackThicknessQ8(thickness);
    cmd.geom        = {halfW, halfH, 0.0f};
}

void DebugShapePool::AddArrow(f32 x0, f32 y0, f32 x1, f32 y1,
                              u32 rgba8, f32 headSize, f32 thickness, u32 durationTicks) noexcept {
    if (m_pool.empty()) return;
    DebugShapeCmd& cmd = AllocateSlot(durationTicks);
    cmd.kind        = DebugShapeKind::Arrow;
    cmd.x0          = x0;
    cmd.y0          = y0;
    cmd.rgba8       = rgba8;
    cmd.thicknessQ8 = PackThicknessQ8(thickness);
    cmd.geom        = {x1, y1, headSize};
}

void DebugShapePool::AddCircle(f32 cx, f32 cy, f32 radius,
                               u32 rgba8, u8 segments, f32 thickness, u32 durationTicks) noexcept {
    if (m_pool.empty()) return;
    DebugShapeCmd& cmd = AllocateSlot(durationTicks);
    cmd.kind        = DebugShapeKind::CircleOutline;
    cmd.x0          = cx;
    cmd.y0          = cy;
    cmd.rgba8       = rgba8;
    cmd.auxParam    = segments;
    cmd.thicknessQ8 = PackThicknessQ8(thickness);
    cmd.geom        = {radius, 0.0f, 0.0f};
}

void DebugShapePool::AddTextWorld(f32 wx, f32 wy, std::string_view text12,
                                  u32 rgba8, u32 durationTicks) noexcept {
    if (m_pool.empty()) return;
    DebugShapeCmd& cmd = AllocateSlot(durationTicks);
    cmd.kind     = DebugShapeKind::TextWorld;
    cmd.x0       = wx;
    cmd.y0       = wy;
    cmd.rgba8    = rgba8;
    const usize len = std::min<usize>(text12.size(), 12u);
    cmd.auxParam = static_cast<u8>(len);
    for (usize i = 0; i < len; ++i) {
        cmd.inlineText[i] = text12[i];
    }
}

void DebugShapePool::AddTextScreen(f32 sx, f32 sy, std::string_view text12,
                                   u32 rgba8, u32 durationTicks) noexcept {
    if (m_pool.empty()) return;
    DebugShapeCmd& cmd = AllocateSlot(durationTicks);
    cmd.kind     = DebugShapeKind::TextScreen;
    cmd.x0       = sx;
    cmd.y0       = sy;
    cmd.rgba8    = rgba8;
    const usize len = std::min<usize>(text12.size(), 12u);
    cmd.auxParam = static_cast<u8>(len);
    for (usize i = 0; i < len; ++i) {
        cmd.inlineText[i] = text12[i];
    }
}

void DebugShapePool::EmitAndExpire(u64 currentTickIndex,
                                   renderer::TextureHandle debugAtlas,
                                   renderer::RenderList& outList) noexcept {
    m_currentTickLow = static_cast<u32>(currentTickIndex);
    DebugBatcher worldBatcher(outList, debugAtlas, kDebugWorldPassId, 0);
    DebugBatcher hudBatcher(outList, debugAtlas, kDebugHudPassId, kDebugHudViewSlot);

    for (usize wordIdx = 0; wordIdx < kWordCount; ++wordIdx) {
        u64 word = m_activeWords[wordIdx];
        while (word != 0ull) {
            const u32 bit = static_cast<u32>(std::countr_zero(word));
            word &= (word - 1ull);

            const usize slotIdx = wordIdx * 64u + bit;
            const DebugShapeCmd& cmd = m_pool[slotIdx];

            // Expire only when currentTickLow strictly exceeds expireTick so 1-tick shapes
            // emitted during Tick T remain visible on the render frame following Tick T.
            if (static_cast<i32>(cmd.expireTick - m_currentTickLow) < 0) {
                m_activeWords[wordIdx] &= ~(1ull << bit);
                continue;
            }

            const renderer::Color col = renderer::Color::FromRGBA8(cmd.rgba8);
            const f32 thickness = UnpackThicknessQ8(cmd.thicknessQ8);

            switch (cmd.kind) {
                case DebugShapeKind::Line:
                    worldBatcher.PushLine(cmd.x0, cmd.y0, cmd.geom.x1, cmd.geom.y1, thickness, col);
                    break;
                case DebugShapeKind::BoxOutline: {
                    const renderer::FRect r{cmd.x0 - cmd.geom.x1, cmd.y0 - cmd.geom.y1,
                                            cmd.geom.x1 * 2.0f, cmd.geom.y1 * 2.0f};
                    worldBatcher.PushBoxOutline(r, thickness, col);
                    break;
                }
                case DebugShapeKind::BoxFilled: {
                    const renderer::FRect r{cmd.x0 - cmd.geom.x1, cmd.y0 - cmd.geom.y1,
                                            cmd.geom.x1 * 2.0f, cmd.geom.y1 * 2.0f};
                    worldBatcher.PushSolidRect(r, col);
                    break;
                }
                case DebugShapeKind::CircleOutline:
                    worldBatcher.PushCircleOutline(cmd.x0, cmd.y0, cmd.geom.x1, cmd.auxParam, thickness, col);
                    break;
                case DebugShapeKind::Arrow:
                    worldBatcher.PushArrow(cmd.x0, cmd.y0, cmd.geom.x1, cmd.geom.y1, cmd.geom.headSize, thickness, col);
                    break;
                case DebugShapeKind::TextWorld:
                    worldBatcher.PushText(cmd.x0, cmd.y0,
                                          std::string_view(cmd.inlineText.data(), cmd.auxParam), col, 0.25f);
                    break;
                case DebugShapeKind::TextScreen:
                    hudBatcher.PushText(cmd.x0, cmd.y0,
                                        std::string_view(cmd.inlineText.data(), cmd.auxParam), col, 1.0f);
                    break;
            }
        }
    }
}

} // namespace en::debug
