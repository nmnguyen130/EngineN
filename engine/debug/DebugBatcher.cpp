#include "debug/DebugBatcher.h"

#include "renderer/RenderList.h"

#include <algorithm>
#include <charconv>
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
    f32 chordScale; // 2.0f * sin(pi / segments)
};

// Precomputed unit-circle incremental rotors and chord scales to eliminate runtime sin/cos/sqrt loops.
inline constexpr std::array<Rotor2D, 6> kCircleRotors{{
    {8,  0.70710678f, 0.70710678f, 0.76536686f},
    {12, 0.86602540f, 0.50000000f, 0.51763809f},
    {16, 0.92387953f, 0.38268343f, 0.39018064f},
    {20, 0.95105652f, 0.30901699f, 0.31286893f},
    {24, 0.96592583f, 0.25881905f, 0.26105238f},
    {32, 0.98078528f, 0.19509032f, 0.19603428f}
}};

} // namespace

void DebugBatcher::PushSolidRect(renderer::FRect rect, renderer::Color color) noexcept {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    m_list.PushQuad(MakeQuad(rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f, rect.w, rect.h, color));
}

void DebugBatcher::PushBoxOutline(renderer::FRect rect, f32 thickness, renderer::Color color) noexcept {
    if (rect.w <= 0.0f || rect.h <= 0.0f || thickness <= 0.0f) {
        return;
    }
    const f32 t    = std::min({thickness, rect.w * 0.5f, rect.h * 0.5f});
    const f32 half = t * 0.5f;
    const f32 midX = rect.x + rect.w * 0.5f;

    renderer::QuadCmd cmd = MakeQuad(midX, rect.y + half, rect.w, t, color);
    m_list.PushQuad(cmd);

    cmd.y = rect.y + rect.h - half;
    m_list.PushQuad(cmd);

    const f32 innerH = rect.h - t * 2.0f;
    if (innerH > 0.0f) {
        cmd.x = rect.x + half;
        cmd.y = rect.y + rect.h * 0.5f;
        cmd.w = t;
        cmd.h = innerH;
        m_list.PushQuad(cmd);

        cmd.x = rect.x + rect.w - half;
        m_list.PushQuad(cmd);
    }
}

void DebugBatcher::PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, renderer::Color color) noexcept {
    const f32 dx    = x1 - x0;
    const f32 dy    = y1 - y0;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-6f || thickness <= 0.0f) {
        return;
    }
    const f32 len    = std::sqrt(lenSq);
    const f32 invLen = 1.0f / len;
    m_list.PushQuad(MakeQuad((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, len, thickness,
                             color, kSolidWhiteUv, dx * invLen, dy * invLen));
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

    m_list.PushQuad(MakeQuad((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, len, thickness,
                             color, kSolidWhiteUv, ux, uy));

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
    Rotor2D rotor     = kCircleRotors[2];
    for (const Rotor2D& r : kCircleRotors) {
        if (segCount <= r.segments) {
            rotor = r;
            break;
        }
    }

    const f32 chordLen    = radius * rotor.chordScale;
    const f32 invChordLen = 1.0f / chordLen;
    renderer::QuadCmd cmd = MakeQuad(0.0f, 0.0f, chordLen, thickness, color);

    f32 vx = radius;
    f32 vy = 0.0f;
    for (u8 i = 0; i < rotor.segments; ++i) {
        const f32 nx = vx * rotor.cosStep - vy * rotor.sinStep;
        const f32 ny = vx * rotor.sinStep + vy * rotor.cosStep;
        cmd.x      = cx + (vx + nx) * 0.5f;
        cmd.y      = cy + (vy + ny) * 0.5f;
        cmd.cosRot = (nx - vx) * invChordLen;
        cmd.sinRot = (ny - vy) * invChordLen;
        m_list.PushQuad(cmd);
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

    renderer::QuadCmd cmd = MakeQuad(0.0f, 0.0f, quadW, quadH, color, {0.0f, 0.0f, kGlyphUvW, kGlyphUvH});

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
        cmd.x    = curX + halfW;
        cmd.y    = curY + halfH;
        cmd.uv.x = static_cast<f32>((glyphIdx & 15u) * 8u) * kInvAtlasW;
        cmd.uv.y = static_cast<f32>((glyphIdx >> 4u) * 8u) * kInvAtlasH;
        m_list.PushQuad(cmd);
        curX += advX;
    }
}

char* DebugBatcher::FormatI64(char* out, char* end, i64 val) noexcept {
    if (out >= end) {
        return out;
    }
    const auto [ptr, ec] = std::to_chars(out, end, val);
    return ec == std::errc{} ? ptr : out;
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
        for (const char c : std::string_view{"NaN"}) {
            if (out < end) *out++ = c;
        }
        return out;
    }
    if (val < 0.0f) {
        *out++ = '-';
        if (out >= end) return out;
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
    m_activeWords.fill(0ull);
    m_nonEmptyWordMask = 0u;
    m_nonFullWordMask  = 0xFFFFFFFFu;
    m_activeCount      = 0u;
}

void DebugShapePool::Clear() noexcept {
    u32 mask = m_nonEmptyWordMask;
    while (mask != 0u) {
        const u32 w = static_cast<u32>(std::countr_zero(mask));
        mask &= (mask - 1u);
        m_activeWords[w] = 0ull;
    }
    m_nonEmptyWordMask = 0u;
    m_nonFullWordMask  = 0xFFFFFFFFu;
    m_activeCount      = 0u;
}

DebugShapeCmd* DebugShapePool::AllocateSlot(DebugShapeKind kind, f32 x0, f32 y0,
                                            u32 rgba8, f32 thickness, u32 durationTicks) noexcept {
    if (m_pool.empty()) {
        return nullptr;
    }
    u32 chosenSlot = m_currentTickLow & static_cast<u32>(kMaxShapes - 1);
    if (m_nonFullWordMask != 0u) {
        // Pack active shapes into the lowest available word/bit to keep active commands in L1D cache.
        const u32 w       = static_cast<u32>(std::countr_zero(m_nonFullWordMask));
        const u32 bit     = static_cast<u32>(std::countr_one(m_activeWords[w]));
        chosenSlot        = (w << 6u) | bit;
        const u64 newWord = m_activeWords[w] | (1ull << bit);
        m_activeWords[w]  = newWord;
        m_nonEmptyWordMask |= (1u << w);
        if (newWord == ~0ull) {
            m_nonFullWordMask &= ~(1u << w);
        }
        ++m_activeCount;
    }

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

void DebugShapePool::AddTextInternal(DebugShapeKind kind, f32 x, f32 y,
                                     std::string_view text12, u32 rgba8, u32 durationTicks) noexcept {
    if (DebugShapeCmd* cmd = AllocateSlot(kind, x, y, rgba8, 1.0f, durationTicks)) {
        const u8 len  = static_cast<u8>(std::min<usize>(text12.size(), cmd->inlineText.size()));
        cmd->auxParam = len;
        if (len > 0) {
            std::memcpy(cmd->inlineText.data(), text12.data(), len);
        }
    }
}

void DebugShapePool::AddTextWorld(f32 wx, f32 wy, std::string_view text12, u32 rgba8, u32 durationTicks) noexcept {
    AddTextInternal(DebugShapeKind::TextWorld, wx, wy, text12, rgba8, durationTicks);
}

void DebugShapePool::AddTextScreen(f32 sx, f32 sy, std::string_view text12, u32 rgba8, u32 durationTicks) noexcept {
    AddTextInternal(DebugShapeKind::TextScreen, sx, sy, text12, rgba8, durationTicks);
}

void DebugShapePool::EmitAndExpire(u64 currentTickIndex,
                                   DebugBatcher& worldBatcher,
                                   DebugBatcher& hudBatcher) noexcept {
    m_currentTickLow = static_cast<u32>(currentTickIndex);
    if (m_nonEmptyWordMask == 0u || m_pool.empty()) {
        return;
    }

    constexpr f32 kInvQ8 = 1.0f / 256.0f;
    u32 activeWordMask   = m_nonEmptyWordMask;
    while (activeWordMask != 0u) {
        const u32 w = static_cast<u32>(std::countr_zero(activeWordMask));
        activeWordMask &= (activeWordMask - 1u);

        u64 word        = m_activeWords[w];
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

        if (expiredMask != 0ull) {
            const u64 remaining = m_activeWords[w] & ~expiredMask;
            m_activeWords[w]    = remaining;
            m_nonFullWordMask  |= (1u << w);
            if (remaining == 0ull) {
                m_nonEmptyWordMask &= ~(1u << w);
            }
        }
    }
}

u64 DebugShapePool::ComputeAuditHash() const noexcept {
    u64 h = MixAuditHash(0xD3B654A9E001ull, (static_cast<u64>(m_nonEmptyWordMask) << 32) | m_activeCount);
    u32 mask = m_nonEmptyWordMask;
    while (mask != 0u) {
        const u32 w = static_cast<u32>(std::countr_zero(mask));
        mask &= (mask - 1u);
        h = MixAuditHash(h, m_activeWords[w] ^ (static_cast<u64>(w) << 56));
    }
    return h;
}

} // namespace en::debug
