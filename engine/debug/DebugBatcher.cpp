#include "debug/DebugBatcher.h"

#include <charconv>
#include <cmath>
#include <cstring>

namespace en {

void DebugDraw::Rect(Rect r, Color c) noexcept {
    if (!m_list || r.w <= 0.0f || r.h <= 0.0f) return;
    m_list->PushRect(r, c, m_pass, 250U, m_depth, m_view);
}

void DebugDraw::Box(Rect r, f32 thick, Color c) noexcept {
    if (!m_list || r.w <= 0.0f || r.h <= 0.0f) return;
    m_list->PushOutline(r, thick, c, m_pass, 251U, m_depth, m_view);
}

void DebugDraw::Obb(Vec2 c, Vec2 he, f32 cosR, f32 sinR, f32 thick, Color col) noexcept {
    if (!m_list) return;
    const Vec2 ux{cosR * he.x, sinR * he.x};
    const Vec2 uy{-sinR * he.y, cosR * he.y};
    const Vec2 p0{c.x - ux.x - uy.x, c.y - ux.y - uy.y};
    const Vec2 p1{c.x + ux.x - uy.x, c.y + ux.y - uy.y};
    const Vec2 p2{c.x + ux.x + uy.x, c.y + ux.y + uy.y};
    const Vec2 p3{c.x - ux.x + uy.x, c.y - ux.y + uy.y};
    Line(p0.x, p0.y, p1.x, p1.y, thick, col);
    Line(p1.x, p1.y, p2.x, p2.y, thick, col);
    Line(p2.x, p2.y, p3.x, p3.y, thick, col);
    Line(p3.x, p3.y, p0.x, p0.y, thick, col);
}

void DebugDraw::Line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thick, Color c) noexcept {
    if (!m_list) return;
    m_list->PushLine({x0, y0}, {x1, y1}, thick, c, m_pass, 251U, m_depth, m_view);
}

void DebugDraw::Arrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 head, f32 thick, Color c) noexcept {
    Line(x0, y0, x1, y1, thick, c);
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq <= 1e-10f || head <= 1e-4f) return;
    const f32 inv = 1.0f / std::sqrt(lenSq);
    const f32 nx = dx * inv;
    const f32 ny = dy * inv;
    const f32 px = -ny;
    const f32 py = nx;
    const f32 hx = x1 - nx * head;
    const f32 hy = y1 - ny * head;
    Line(x1, y1, hx + px * head * 0.5f, hy + py * head * 0.5f, thick, c);
    Line(x1, y1, hx - px * head * 0.5f, hy - py * head * 0.5f, thick, c);
}

void DebugDraw::Circle(f32 cx, f32 cy, f32 r, u8 segs, f32 thick, Color c) noexcept {
    if (!m_list || r <= 0.0f) return;
    m_list->PushCircle({cx, cy}, r, segs, thick, c, m_pass, 251U, m_view);
}

void DebugDraw::Capsule(f32 cx, f32 cy, f32 hw, f32 hh, f32 thick, Color c) noexcept {
    if (!m_list || hw <= 0.0f || hh <= 0.0f) return;
    const f32 body = std::max(0.0f, hh - hw);
    Line(cx - hw, cy - body, cx - hw, cy + body, thick, c);
    Line(cx + hw, cy - body, cx + hw, cy + body, thick, c);
    Circle(cx, cy - body, hw, 12U, thick, c);
    Circle(cx, cy + body, hw, 12U, thick, c);
}

void DebugDraw::Cross(f32 cx, f32 cy, f32 half, f32 thick, Color c) noexcept {
    Line(cx - half, cy, cx + half, cy, thick, c);
    Line(cx, cy - half, cx, cy + half, thick, c);
}

void DebugDraw::Text(f32 x, f32 y, std::string_view s, Color c, f32 scale) noexcept {
    if (!m_list || s.empty()) return;
    m_list->PushText({x, y}, s, c, scale, m_pass, 252U, m_view);
}

char* DebugDraw::Append(char* o, char* e, std::string_view s) noexcept {
    if (!o || o >= e) return o;
    const usize n = std::min(static_cast<usize>(e - o - 1), s.size());
    if (n) std::memcpy(o, s.data(), n);
    o[n] = '\0';
    return o + n;
}

char* DebugDraw::FmtI64(char* o, char* e, i64 v) noexcept {
    if (!o || o >= e) return o;
    const auto r = std::to_chars(o, e - 1, v);
    if (r.ec != std::errc{}) { *o = '\0'; return o; }
    *r.ptr = '\0';
    return r.ptr;
}

char* DebugDraw::FmtF2(char* o, char* e, f32 v) noexcept {
    if (!o || o >= e) return o;
    const auto r = std::to_chars(o, e - 1, v, std::chars_format::fixed, 2);
    if (r.ec != std::errc{}) { *o = '\0'; return o; }
    *r.ptr = '\0';
    return r.ptr;
}

namespace {

u32 Pack(Color c) noexcept {
    const auto q = [](f32 v) -> u32 {
        return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (q(c.r) << 24U) | (q(c.g) << 16U) | (q(c.b) << 8U) | q(c.a);
}

} // namespace

ShapeCmd* ShapePool::Push(ShapeCmd::Kind k, f32 x0, f32 y0, Color c, f32 thick, u32 life) noexcept {
    if (!m_pool) return nullptr;
    const u32 slot = (m_head + m_count) & (kMaxShapes - 1U);
    if (m_count < kMaxShapes) ++m_count;
    else m_head = (m_head + 1U) & (kMaxShapes - 1U);
    ShapeCmd& cmd = m_pool[slot];
    cmd = ShapeCmd{};
    cmd.kind = k;
    cmd.x0 = x0;
    cmd.y0 = y0;
    cmd.rgba = Pack(c);
    cmd.thickQ8 = static_cast<u16>(std::clamp(thick * 256.0f, 1.0f, 65535.0f));
    cmd.expire = life ? (m_tick + life) : 0U;
    return &cmd;
}

void ShapePool::Line(f32 x0, f32 y0, f32 x1, f32 y1, Color c, f32 thick, u32 life) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::Line, x0, y0, c, thick, life)) {
        s->u.g.x1 = x1;
        s->u.g.y1 = y1;
    }
}

void ShapePool::Box(Rect r, Color c, f32 thick, bool solid, u32 life) noexcept {
    const auto k = solid ? ShapeCmd::Kind::Solid : ShapeCmd::Kind::Box;
    if (auto* s = Push(k, r.x, r.y, c, thick, life)) {
        s->u.g.x1 = r.w;
        s->u.g.y1 = r.h;
    }
}

void ShapePool::Arrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 head, Color c, f32 thick, u32 life) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::Arrow, x0, y0, c, thick, life)) {
        s->u.g.x1 = x1;
        s->u.g.y1 = y1;
        s->u.g.aux = head;
    }
}

void ShapePool::Circle(f32 cx, f32 cy, f32 r, Color c, u8 segs, f32 thick, u32 life) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::Circle, cx, cy, c, thick, life)) {
        s->u.g.x1 = r;
        s->aux = segs;
    }
}

void ShapePool::Capsule(f32 cx, f32 cy, f32 hw, f32 hh, Color c, f32 thick, u32 life) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::Capsule, cx, cy, c, thick, life)) {
        s->u.g.x1 = hw;
        s->u.g.y1 = hh;
    }
}

void ShapePool::TextWorld(f32 x, f32 y, std::string_view text, Color c, f32 scale, u32 life) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::TextWorld, x, y, c, scale, life)) {
        const usize n = std::min(text.size(), sizeof(s->u.text) - 1U);
        std::memcpy(s->u.text, text.data(), n);
        s->u.text[n] = '\0';
        s->aux = static_cast<u8>(n);
    }
}

void ShapePool::TextHud(f32 x, f32 y, std::string_view text, Color c, f32 scale, u32 life) noexcept {
    if (auto* s = Push(ShapeCmd::Kind::TextHud, x, y, c, scale, life)) {
        const usize n = std::min(text.size(), sizeof(s->u.text) - 1U);
        std::memcpy(s->u.text, text.data(), n);
        s->u.text[n] = '\0';
        s->aux = static_cast<u8>(n);
    }
}

void ShapePool::Emit(u32 tick, DebugDraw& world, DebugDraw& hud) noexcept {
    if (!m_pool || m_count == 0U) return;

    u32 keep = 0U;
    for (u32 i = 0U; i < m_count; ++i) {
        const u32 slot = (m_head + i) & (kMaxShapes - 1U);
        ShapeCmd& cmd = m_pool[slot];
        if (cmd.expire && tick >= cmd.expire) continue;

        const Color c = DebugDraw::Unpack(cmd.rgba);
        const f32 th = static_cast<f32>(cmd.thickQ8) * (1.0f / 256.0f);
        DebugDraw& d = (cmd.kind == ShapeCmd::Kind::TextHud) ? hud : world;

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
        case ShapeCmd::Kind::Capsule:
            d.Capsule(cmd.x0, cmd.y0, cmd.u.g.x1, cmd.u.g.y1, th, c);
            break;
        case ShapeCmd::Kind::TextWorld:
        case ShapeCmd::Kind::TextHud:
            d.Text(cmd.x0, cmd.y0, {cmd.u.text, cmd.aux}, c, th);
            break;
        }

        if (keep != i) m_pool[(m_head + keep) & (kMaxShapes - 1U)] = cmd;
        ++keep;
    }
    m_count = keep;
}

} // namespace en
