#include "debug/DebugBatcher.h"

#include "renderer/RenderList.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace en::debug {

namespace {

[[nodiscard]] constexpr u16 PackThicknessQ8(f32 thickness) noexcept {
    const f32 clamped = std::clamp(thickness, 0.25f, 255.0f);
    return static_cast<u16>(clamped * 256.0f + 0.5f);
}

struct Rotor2D {
    u8  segments;
    f32 cosStep;
    f32 sinStep;
};

// Precomputed unit-circle incremental rotors for common segment counts to avoid runtime sin/cos loops.
inline constexpr std::array<Rotor2D, 6> kCircleRotors{{
    {8,  0.70710678f, 0.70710678f},
    {12, 0.86602540f, 0.50000000f},
    {16, 0.92387953f, 0.38268343f},
    {20, 0.95105652f, 0.30901699f},
    {24, 0.96592583f, 0.25881905f},
    {32, 0.98078528f, 0.19509032f}
}};

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
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    renderer::QuadCmd cmd = m_templateQuad;
    cmd.x     = rect.x + rect.w * 0.5f;
    cmd.y     = rect.y + rect.h * 0.5f;
    cmd.w     = rect.w;
    cmd.h     = rect.h;
    cmd.color = color;
    m_list.PushQuad(cmd);
}

void DebugBatcher::PushBoxOutline(renderer::FRect rect, f32 thickness, renderer::Color color) noexcept {
    if (rect.w <= 0.0f || rect.h <= 0.0f || thickness <= 0.0f) {
        return;
    }
    const f32 t = std::min({thickness, rect.w * 0.5f, rect.h * 0.5f});
    PushSolidRect({rect.x, rect.y, rect.w, t}, color);
    PushSolidRect({rect.x, rect.y + rect.h - t, rect.w, t}, color);
    if (rect.h > t * 2.0f) {
        const f32 innerH = rect.h - t * 2.0f;
        PushSolidRect({rect.x, rect.y + t, t, innerH}, color);
        PushSolidRect({rect.x + rect.w - t, rect.y + t, t, innerH}, color);
    }
}

void DebugBatcher::PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, renderer::Color color) noexcept {
    const f32 dx     = x1 - x0;
    const f32 dy     = y1 - y0;
    const f32 lenSq  = dx * dx + dy * dy;
    if (lenSq < 1e-6f || thickness <= 0.0f) {
        return;
    }
    const f32 len    = std::sqrt(lenSq);
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

void DebugBatcher::PushArrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 headSize, f32 thickness, renderer::Color color) noexcept {
    const f32 dx    = x1 - x0;
    const f32 dy    = y1 - y0;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-6f || thickness <= 0.0f) {
        return;
    }
    const f32 len    = std::sqrt(lenSq);
    const f32 invLen = 1.0f / len;
    const f32 ux     = dx * invLen;
    const f32 uy     = dy * invLen;

    PushLine(x0, y0, x1, y1, thickness, color);
    if (headSize > 0.0f) {
        const f32 hLen = std::min(headSize, len * 0.6f);
        const f32 bx   = x1 - ux * hLen;
        const f32 by   = y1 - uy * hLen;
        const f32 px   = -uy * (hLen * 0.55f);
        const f32 py   =  ux * (hLen * 0.55f);
        PushLine(x1, y1, bx + px, by + py, thickness, color);
        PushLine(x1, y1, bx - px, by - py, thickness, color);
    }
}

void DebugBatcher::PushCircleOutline(f32 cx, f32 cy, f32 radius, u8 segments, f32 thickness, renderer::Color color) noexcept {
    if (radius <= 0.0f || thickness <= 0.0f) {
        return;
    }
    const u8 segCount = std::clamp<u8>(segments, 8u, 32u);
    f32 cosStep = 0.92387953f;
    f32 sinStep = 0.38268343f;
    u8  actualSegs = 16;

    for (const Rotor2D& r : kCircleRotors) {
        if (segCount <= r.segments) {
            actualSegs = r.segments;
            cosStep    = r.cosStep;
            sinStep    = r.sinStep;
            break;
        }
    }

    f32 vx = radius;
    f32 vy = 0.0f;
    for (u8 i = 0; i < actualSegs; ++i) {
        const f32 nx = vx * cosStep - vy * sinStep;
        const f32 ny = vx * sinStep + vy * cosStep;
        PushLine(cx + vx, cy + vy, cx + nx, cy + ny, thickness, color);
        vx = nx;
        vy = ny;
    }
}

void DebugBatcher::PushText(f32 x, f32 y, std::string_view text, renderer::Color color, f32 scale) noexcept {
    if (text.empty() || scale <= 0.0f) {
        return;
    }
    constexpr f32 kInvAtlasW = 1.0f / static_cast<f32>(kDebugAtlasWidth);
    constexpr f32 kInvAtlasH = 1.0f / static_cast<f32>(kDebugAtlasHeight);
    constexpr f32 kGlyphUvW  = kGlyphWidth * kInvAtlasW;
    constexpr f32 kGlyphUvH  = kGlyphHeight * kInvAtlasH;

    const f32 advX  = kGlyphWidth * scale;
    const f32 advY  = (kGlyphHeight + 2.0f) * scale;
    const f32 quadW = kGlyphWidth * scale;
    const f32 quadH = kGlyphHeight * scale;
    const f32 halfW = quadW * 0.5f;
    const f32 halfH = quadH * 0.5f;

    renderer::QuadCmd cmd = m_templateQuad;
    cmd.w     = quadW;
    cmd.h     = quadH;
    cmd.color = color;

    f32 curX = x;
    f32 curY = y;
    for (const char ch : text) {
        if (ch == '\n') {
            curX = x;
            curY += advY;
            continue;
        }
        const u8 code = static_cast<u8>(ch);
        if (code <= 0x20u) {
            curX += advX;
            continue;
        }
        const u32 glyphIdx = (code <= 0x7Fu) ? static_cast<u32>(code - 0x20u) : 95u;
        const f32 u0       = static_cast<f32>((glyphIdx & 15u) * 8u) * kInvAtlasW;
        const f32 v0       = static_cast<f32>((glyphIdx >> 4u) * 8u) * kInvAtlasH;

        cmd.x  = curX + halfW;
        cmd.y  = curY + halfH;
        cmd.uv = {u0, v0, kGlyphUvW, kGlyphUvH};
        m_list.PushQuad(cmd);
        curX += advX;
    }
}

char* DebugBatcher::FormatI64(char* out, char* end, i64 val) noexcept {
    if (out >= end) {
        return out;
    }
    u64 mag = 0;
    if (val < 0) {
        *out++ = '-';
        if (out >= end) {
            return out;
        }
        mag = static_cast<u64>(-(val + 1)) + 1ull;
    } else {
        mag = static_cast<u64>(val);
    }

    std::array<char, 20> rev{};
    u32 len = 0;
    do {
        rev[len++] = static_cast<char>('0' + (mag % 10ull));
        mag /= 10ull;
    } while (mag != 0ull);

    while (len > 0 && out < end) {
        *out++ = rev[--len];
    }
    return out;
}

char* DebugBatcher::FormatU64Hex(char* out, char* end, u64 val, u8 digits) noexcept {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    const u8 count = std::clamp<u8>(digits, 1u, 16u);
    for (u8 i = 0; i < count && out < end; ++i) {
        const u8 shift = static_cast<u8>((count - 1u - i) * 4u);
        *out++ = kHex[static_cast<usize>((val >> shift) & 0xFull)];
    }
    return out;
}

char* DebugBatcher::FormatF32Fixed2(char* out, char* end, f32 val) noexcept {
    if (out >= end) {
        return out;
    }
    if (!std::isfinite(val)) {
        const std::string_view nanStr = "NaN";
        for (const char c : nanStr) {
            if (out < end) *out++ = c;
        }
        return out;
    }
    if (val < 0.0f) {
        *out++ = '-';
        if (out >= end) {
            return out;
        }
        val = -val;
    }
    const u64 scaled = static_cast<u64>(val * 100.0f + 0.5f);
    out = FormatI64(out, end, static_cast<i64>(scaled / 100ull));
    if (out + 3 <= end) {
        const u64 frac = scaled % 100ull;
        *out++ = '.';
        *out++ = static_cast<char>('0' + (frac / 10ull));
        *out++ = static_cast<char>('0' + (frac % 10ull));
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
    m_activeCount = 0;
}

DebugShapeCmd* DebugShapePool::AllocateSlot(DebugShapeKind kind, f32 x0, f32 y0,
                                            u32 rgba8, f32 thickness, u32 durationTicks) noexcept {
    if (m_pool.empty()) {
        return nullptr;
    }
    u32 chosenSlot = m_allocCursor & static_cast<u32>(kMaxShapes - 1);
    const u32 startWord = chosenSlot >> 6u;

    for (u32 i = 0; i < kWordCount; ++i) {
        const u32 w    = (startWord + i) & static_cast<u32>(kWordCount - 1);
        const u64 word = m_activeWords[w];
        if (word != ~0ull) {
            const u32 bit = static_cast<u32>(std::countr_one(word));
            chosenSlot    = (w << 6u) | bit;
            m_activeWords[w] |= (1ull << bit);
            ++m_activeCount;
            m_allocCursor = (chosenSlot + 1u) & static_cast<u32>(kMaxShapes - 1);
            goto init_slot;
        }
    }
    // Pool is full; overwrite cursor slot in O(1) without growing memory.
    m_allocCursor = (chosenSlot + 1u) & static_cast<u32>(kMaxShapes - 1);

init_slot:
    DebugShapeCmd& cmd = m_pool[chosenSlot];
    cmd.x0          = x0;
    cmd.y0          = y0;
    cmd.rgba8       = rgba8;
    cmd.expireTick  = m_currentTickLow + std::max(durationTicks, 1u);
    cmd.kind        = kind;
    cmd.auxParam    = 0;
    cmd.thicknessQ8 = PackThicknessQ8(thickness);
    return &cmd;
}

void DebugShapePool::AddLine(f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba8, f32 thickness, u32 durationTicks) noexcept {
    if (DebugShapeCmd* cmd = AllocateSlot(DebugShapeKind::Line, x0, y0, rgba8, thickness, durationTicks)) {
        cmd->geom = {x1, y1, 0.0f};
    }
}

void DebugShapePool::AddBox(f32 cx, f32 cy, f32 halfW, f32 halfH, u32 rgba8,
                            bool filled, f32 thickness, u32 durationTicks) noexcept {
    const DebugShapeKind kind = filled ? DebugShapeKind::BoxFilled : DebugShapeKind::BoxOutline;
    if (DebugShapeCmd* cmd = AllocateSlot(kind, cx, cy, rgba8, thickness, durationTicks)) {
        cmd->geom = {halfW, halfH, 0.0f};
    }
}

void DebugShapePool::AddArrow(f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba8,
                              f32 headSize, f32 thickness, u32 durationTicks) noexcept {
    if (DebugShapeCmd* cmd = AllocateSlot(DebugShapeKind::Arrow, x0, y0, rgba8, thickness, durationTicks)) {
        cmd->geom = {x1, y1, headSize};
    }
}

void DebugShapePool::AddCircle(f32 cx, f32 cy, f32 radius, u32 rgba8,
                               u8 segments, f32 thickness, u32 durationTicks) noexcept {
    if (DebugShapeCmd* cmd = AllocateSlot(DebugShapeKind::CircleOutline, cx, cy, rgba8, thickness, durationTicks)) {
        cmd->auxParam = segments;
        cmd->geom     = {radius, 0.0f, 0.0f};
    }
}

void DebugShapePool::AddTextWorld(f32 wx, f32 wy, std::string_view text12, u32 rgba8, u32 durationTicks) noexcept {
    if (DebugShapeCmd* cmd = AllocateSlot(DebugShapeKind::TextWorld, wx, wy, rgba8, 1.0f, durationTicks)) {
        const u8 len  = static_cast<u8>(std::min<usize>(text12.size(), cmd->inlineText.size()));
        cmd->auxParam = len;
        cmd->inlineText.fill('\0');
        std::memcpy(cmd->inlineText.data(), text12.data(), len);
    }
}

void DebugShapePool::AddTextScreen(f32 sx, f32 sy, std::string_view text12, u32 rgba8, u32 durationTicks) noexcept {
    if (DebugShapeCmd* cmd = AllocateSlot(DebugShapeKind::TextScreen, sx, sy, rgba8, 1.0f, durationTicks)) {
        const u8 len  = static_cast<u8>(std::min<usize>(text12.size(), cmd->inlineText.size()));
        cmd->auxParam = len;
        cmd->inlineText.fill('\0');
        std::memcpy(cmd->inlineText.data(), text12.data(), len);
    }
}

void DebugShapePool::EmitAndExpire(u64 currentTickIndex,
                                   DebugBatcher& worldBatcher,
                                   DebugBatcher& hudBatcher) noexcept {
    m_currentTickLow = static_cast<u32>(currentTickIndex);
    if (m_activeCount == 0 || m_pool.empty()) {
        return;
    }

    constexpr f32 kInvQ8 = 1.0f / 256.0f;
    for (u32 w = 0; w < kWordCount; ++w) {
        u64 word = m_activeWords[w];
        if (word == 0ull) {
            continue;
        }
        u64 expiredMask = 0ull;
        while (word != 0ull) {
            const u32 bit  = static_cast<u32>(std::countr_zero(word));
            word &= (word - 1ull);
            const u32 slot = (w << 6u) | bit;
            const DebugShapeCmd& cmd = m_pool[slot];

            const renderer::Color color = renderer::Color::FromRGBA8(cmd.rgba8);
            const f32 thickness         = static_cast<f32>(cmd.thicknessQ8) * kInvQ8;

            switch (cmd.kind) {
                case DebugShapeKind::Line:
                    worldBatcher.PushLine(cmd.x0, cmd.y0, cmd.geom.x1, cmd.geom.y1, thickness, color);
                    break;
                case DebugShapeKind::BoxOutline: {
                    const renderer::FRect r{cmd.x0 - cmd.geom.x1, cmd.y0 - cmd.geom.y1,
                                            cmd.geom.x1 * 2.0f, cmd.geom.y1 * 2.0f};
                    worldBatcher.PushBoxOutline(r, thickness, color);
                    break;
                }
                case DebugShapeKind::BoxFilled: {
                    const renderer::FRect r{cmd.x0 - cmd.geom.x1, cmd.y0 - cmd.geom.y1,
                                            cmd.geom.x1 * 2.0f, cmd.geom.y1 * 2.0f};
                    worldBatcher.PushSolidRect(r, color);
                    break;
                }
                case DebugShapeKind::CircleOutline:
                    worldBatcher.PushCircleOutline(cmd.x0, cmd.y0, cmd.geom.x1, cmd.auxParam, thickness, color);
                    break;
                case DebugShapeKind::Arrow:
                    worldBatcher.PushArrow(cmd.x0, cmd.y0, cmd.geom.x1, cmd.geom.y1, cmd.geom.headSize, thickness, color);
                    break;
                case DebugShapeKind::TextWorld: {
                    const usize len = std::min<usize>(cmd.auxParam, cmd.inlineText.size());
                    worldBatcher.PushText(cmd.x0, cmd.y0, {cmd.inlineText.data(), len}, color);
                    break;
                }
                case DebugShapeKind::TextScreen: {
                    const usize len = std::min<usize>(cmd.auxParam, cmd.inlineText.size());
                    hudBatcher.PushText(cmd.x0, cmd.y0, {cmd.inlineText.data(), len}, color);
                    break;
                }
            }

            if (static_cast<i32>(m_currentTickLow - cmd.expireTick) >= 0) {
                expiredMask |= (1ull << bit);
                --m_activeCount;
            }
        }
        m_activeWords[w] &= ~expiredMask;
    }
}

void DebugShapePool::EmitAndExpire(u64 currentTickIndex,
                                   renderer::TextureHandle debugAtlas,
                                   renderer::RenderList& outList) noexcept {
    DebugBatcher worldBatcher(outList, debugAtlas, kDebugWorldPassId, 0);
    DebugBatcher hudBatcher(outList, debugAtlas, kDebugHudPassId, kDebugHudViewSlot);
    EmitAndExpire(currentTickIndex, worldBatcher, hudBatcher);
}

u64 DebugShapePool::ComputeAuditHash() const noexcept {
    u64 h = MixAuditHash(0xD3B654A9E001ull, m_activeCount);
    for (const u64 word : m_activeWords) {
        h = MixAuditHash(h, word);
    }
    return h;
}

} // namespace en::debug
