#include "debug/DebugDraw.h"

#include <charconv>

namespace en {

void DebugDraw::Rect(en::Rect r, Color c) noexcept {
    if (!m_list || r.IsEmpty()) return;
    m_list->PushRect(r, c, m_pass, 250U, m_depth, m_view);
}

void DebugDraw::Box(en::Rect r, f32 thick, Color c) noexcept {
    if (!m_list || r.IsEmpty()) return;
    m_list->PushOutline(r, thick, c, m_pass, 251U, m_depth, m_view);
}

void DebugDraw::Line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thick, Color c) noexcept {
    if (!m_list) return;
    m_list->PushLine({x0, y0}, {x1, y1}, thick, c, m_pass, 251U, m_depth, m_view);
}

void DebugDraw::Arrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 head, f32 thick, Color c) noexcept {
    Line(x0, y0, x1, y1, thick, c);
    const Vec2 delta{x1 - x0, y1 - y0};
    const f32  lenSq = delta.LengthSq();
    if (lenSq <= 1e-10f || head <= 1e-4f) return;
    const Vec2 dir  = delta * (1.0f / std::sqrt(lenSq));
    const Vec2 perp = dir.PerpCCW();
    const Vec2 base = Vec2{x1, y1} - dir * head;
    Line(x1, y1, base.x + perp.x * head * 0.5f, base.y + perp.y * head * 0.5f, thick, c);
    Line(x1, y1, base.x - perp.x * head * 0.5f, base.y - perp.y * head * 0.5f, thick, c);
}

void DebugDraw::Circle(f32 cx, f32 cy, f32 r, u8 segs, f32 thick, Color c) noexcept {
    if (!m_list || r <= 0.0f) return;
    m_list->PushCircle({cx, cy}, r, segs, thick, c, m_pass, 251U, m_view);
}

void DebugDraw::Cross(f32 cx, f32 cy, f32 half, f32 thick, Color c) noexcept {
    Line(cx - half, cy, cx + half, cy, thick, c);
    Line(cx, cy - half, cx, cy + half, thick, c);
}

void DebugDraw::Text(f32 x, f32 y, std::string_view s, Color c, f32 scale) noexcept {
    if (!m_list || s.empty()) return;
    m_list->PushText({x, y}, s, c, scale, m_pass, 252U, m_view);
}

char* DebugDraw::Append(char* out, char* end, std::string_view s) noexcept {
    if (!out || out >= end) return out;
    const usize n = std::min(static_cast<usize>(end - out - 1), s.size());
    if (n > 0U) std::memcpy(out, s.data(), n);
    out[n] = '\0';
    return out + n;
}

char* DebugDraw::FmtI64(char* out, char* end, i64 v) noexcept {
    if (!out || out >= end) return out;
    const auto res = std::to_chars(out, end - 1, v);
    if (res.ec != std::errc{}) {
        *out = '\0';
        return out;
    }
    *res.ptr = '\0';
    return res.ptr;
}

char* DebugDraw::FmtF2(char* out, char* end, f32 v) noexcept {
    if (!out || out >= end) return out;
    const auto res = std::to_chars(out, end - 1, v, std::chars_format::fixed, 2);
    if (res.ec != std::errc{}) {
        *out = '\0';
        return out;
    }
    *res.ptr = '\0';
    return res.ptr;
}

ShapeCmd* ShapePool::Push(
    ShapeCmd::Kind kind, f32 x0, f32 y0, Color c, f32 thick, u32 life) noexcept {
    if (!m_pool) return nullptr;
    const u32 slot = (m_head + m_count) & (kMaxGizmos - 1U);
    if (m_count < kMaxGizmos) ++m_count;
    else m_head = (m_head + 1U) & (kMaxGizmos - 1U);

    ShapeCmd& cmd = m_pool[slot];
    cmd           = ShapeCmd{};
    cmd.kind      = kind;
    cmd.x0        = x0;
    cmd.y0        = y0;
    cmd.rgba      = c.ToRGBA8();
    cmd.thickQ8   = static_cast<u16>(std::clamp(thick * 256.0f, 1.0f, 65535.0f));
    cmd.expire    = life > 0U ? (m_tick + life) : 0U;
    return &cmd;
}

void ShapePool::Line(f32 x0, f32 y0, f32 x1, f32 y1, Color c, f32 thick, u32 lifeTicks) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::Line, x0, y0, c, thick, lifeTicks)) {
        s->u.g.x1 = x1;
        s->u.g.y1 = y1;
    }
}

void ShapePool::Box(Rect r, Color c, f32 thick, bool solid, u32 lifeTicks) noexcept {
    const auto k = solid ? ShapeCmd::Kind::Solid : ShapeCmd::Kind::Box;
    if (auto* s = Push(k, r.x, r.y, c, thick, lifeTicks)) {
        s->u.g.x1 = r.w;
        s->u.g.y1 = r.h;
    }
}

void ShapePool::Arrow(
    f32 x0, f32 y0, f32 x1, f32 y1, f32 head, Color c, f32 thick, u32 lifeTicks) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::Arrow, x0, y0, c, thick, lifeTicks)) {
        s->u.g.x1  = x1;
        s->u.g.y1  = y1;
        s->u.g.aux = head;
    }
}

void ShapePool::Circle(
    f32 cx, f32 cy, f32 r, Color c, u8 segs, f32 thick, u32 lifeTicks) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::Circle, cx, cy, c, thick, lifeTicks)) {
        s->u.g.x1 = r;
        s->aux    = segs;
    }
}

void ShapePool::Cross(f32 cx, f32 cy, f32 half, Color c, f32 thick, u32 lifeTicks) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::Cross, cx, cy, c, thick, lifeTicks)) {
        s->u.g.x1 = half;
    }
}

void ShapePool::TextWorld(
    f32 x, f32 y, std::string_view text, Color c, f32 scale, u32 lifeTicks) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::TextWorld, x, y, c, scale, lifeTicks)) {
        const usize n = std::min(text.size(), sizeof(s->u.text) - 1U);
        std::memcpy(s->u.text, text.data(), n);
        s->u.text[n] = '\0';
        s->aux       = static_cast<u8>(n);
    }
}

void ShapePool::TextHud(
    f32 x, f32 y, std::string_view text, Color c, f32 scale, u32 lifeTicks) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::TextHud, x, y, c, scale, lifeTicks)) {
        const usize n = std::min(text.size(), sizeof(s->u.text) - 1U);
        std::memcpy(s->u.text, text.data(), n);
        s->u.text[n] = '\0';
        s->aux       = static_cast<u8>(n);
    }
}

void ShapePool::Emit(u32 tick, DebugDraw& world, DebugDraw& hud) noexcept {
    if (!m_pool || m_count == 0U) return;

    u32 keep = 0U;
    for (u32 i = 0U; i < m_count; ++i) {
        const u32 slot = (m_head + i) & (kMaxGizmos - 1U);
        ShapeCmd  cmd  = m_pool[slot];
        if (cmd.expire != 0U && tick >= cmd.expire) continue;

        const Color c{
            static_cast<f32>((cmd.rgba >> 24U) & 0xFFU) * (1.0f / 255.0f),
            static_cast<f32>((cmd.rgba >> 16U) & 0xFFU) * (1.0f / 255.0f),
            static_cast<f32>((cmd.rgba >>  8U) & 0xFFU) * (1.0f / 255.0f),
            static_cast<f32>((cmd.rgba >>  0U) & 0xFFU) * (1.0f / 255.0f),
        };
        const f32  th = static_cast<f32>(cmd.thickQ8) * (1.0f / 256.0f);
        DebugDraw& d  = (cmd.kind == ShapeCmd::Kind::TextHud) ? hud : world;

        switch (cmd.kind) {
            case ShapeCmd::Kind::Line:
                d.Line(cmd.x0, cmd.y0, cmd.u.g.x1, cmd.u.g.y1, th, c);
                break;
            case ShapeCmd::Kind::Box:
                d.Box(Rect{cmd.x0, cmd.y0, cmd.u.g.x1, cmd.u.g.y1}, th, c);
                break;
            case ShapeCmd::Kind::Solid:
                d.Rect(Rect{cmd.x0, cmd.y0, cmd.u.g.x1, cmd.u.g.y1}, c);
                break;
            case ShapeCmd::Kind::Arrow:
                d.Arrow(cmd.x0, cmd.y0, cmd.u.g.x1, cmd.u.g.y1, cmd.u.g.aux, th, c);
                break;
            case ShapeCmd::Kind::Circle:
                d.Circle(cmd.x0, cmd.y0, cmd.u.g.x1, cmd.aux, th, c);
                break;
            case ShapeCmd::Kind::Cross:
                d.Cross(cmd.x0, cmd.y0, cmd.u.g.x1, th, c);
                break;
            case ShapeCmd::Kind::TextWorld:
            case ShapeCmd::Kind::TextHud:
                d.Text(cmd.x0, cmd.y0, {cmd.u.text, cmd.aux}, c, th);
                break;
        }

        if (keep != i) m_pool[(m_head + keep) & (kMaxGizmos - 1U)] = cmd;
        ++keep;
    }
    m_count = keep;
}

} // namespace en
