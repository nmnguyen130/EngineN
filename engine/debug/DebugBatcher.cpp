#include "debug/DebugBatcher.h"
#include <charconv>
#include <cmath>

namespace en {

void DebugBatcher::PushSolidRect(Rect rect, Color color) noexcept {
    m_list.PushRect(rect, color, m_passId, 230, 32768, m_viewSlot);
}

void DebugBatcher::PushBoxOutline(Rect rect, f32 thickness, Color color) noexcept {
    const Vec2 p0{rect.x, rect.y};
    const Vec2 p1{rect.Right(), rect.y};
    const Vec2 p2{rect.Right(), rect.Bottom()};
    const Vec2 p3{rect.x, rect.Bottom()};
    PushLine(p0, p1, thickness, color);
    PushLine(p1, p2, thickness, color);
    PushLine(p2, p3, thickness, color);
    PushLine(p3, p0, thickness, color);
}

void DebugBatcher::PushLine(Vec2 a, Vec2 b, f32 thickness, Color color) noexcept {
    m_list.PushLine(a, b, thickness, color, m_passId, 231, 32768, m_viewSlot);
}

void DebugBatcher::PushArrow(Vec2 start, Vec2 end, f32 headSize, f32 thickness, Color color) noexcept {
    PushLine(start, end, thickness, color);
    const Vec2 dir = (end - start).Normalized();
    if (dir.LengthSq() < 0.1f) return;
    const Vec2 perp = dir.PerpCCW();
    const Vec2 left = end - dir * headSize + perp * (headSize * 0.5f);
    const Vec2 right = end - dir * headSize - perp * (headSize * 0.5f);
    PushLine(end, left, thickness, color);
    PushLine(end, right, thickness, color);
}

void DebugBatcher::PushCircleOutline(
    Vec2 center, f32 radius, u32 segments, f32 thickness, Color color) noexcept {
    m_list.PushCircleOutline(center, radius, segments, thickness, color, m_passId, 231, m_viewSlot);
}

void DebugBatcher::PushText(Vec2 pos, std::string_view text, Color color, f32 scale) noexcept {
    m_list.PushText(pos, text, color, scale, m_passId, 240, m_viewSlot, m_atlas);
}

char* DebugBatcher::FormatI64(char* out, char* end, i64 val) noexcept {
    if (out >= end) return out;
    auto [ptr, ec] = std::to_chars(out, end, val);
    return (ec == std::errc{}) ? ptr : out;
}

char* DebugBatcher::FormatU64Hex(char* out, char* end, u64 val, u8 digits) noexcept {
    if (out + digits > end) return out;
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (u8 i = 0; i < digits; ++i) {
        const u8 shift = static_cast<u8>((digits - 1U - i) * 4U);
        out[i] = kHex[(val >> shift) & 0x0FU];
    }
    return out + digits;
}

char* DebugBatcher::FormatF32Fixed2(char* out, char* end, f32 val) noexcept {
    if (out >= end) return out;
    if (val < 0.0f) {
        *out++ = '-';
        val = -val;
    }
    const i64 scaled = static_cast<i64>(val * 100.0f + 0.5f);
    out = FormatI64(out, end, scaled / 100);
    if (out + 3 <= end) {
        *out++ = '.';
        const i64 frac = scaled % 100;
        *out++ = static_cast<char>('0' + (frac / 10));
        *out++ = static_cast<char>('0' + (frac % 10));
    }
    return out;
}

bool DebugShapePool::Init(AlignedArena& arena, u32 maxShapes) noexcept {
    m_shapes = arena.Carve<ShapeCmd>(maxShapes);
    m_activeCount = 0;
    m_currentTick = 0;
    return !m_shapes.empty();
}

void DebugShapePool::Clear() noexcept {
    for (ShapeCmd& s : m_shapes) s.active = false;
    m_activeCount = 0;
}

void DebugShapePool::PushShape(const ShapeCmd& cmd, u32 durationTicks) noexcept {
    for (ShapeCmd& s : m_shapes) {
        if (!s.active) {
            s = cmd;
            s.expireTick = m_currentTick + std::max(1U, durationTicks);
            s.active = true;
            ++m_activeCount;
            return;
        }
    }
}

void DebugShapePool::AddLine(Vec2 a, Vec2 b, Color color, u32 durationTicks, f32 thickness) noexcept {
    ShapeCmd c{};
    c.a = a;
    c.b = b;
    c.color = color;
    c.thickness = thickness;
    c.kind = ShapeKind::Line;
    PushShape(c, durationTicks);
}

void DebugShapePool::AddBox(Rect rect, Color color, u32 durationTicks, f32 thickness) noexcept {
    ShapeCmd c{};
    c.a = {rect.x, rect.y};
    c.b = {rect.w, rect.h};
    c.color = color;
    c.thickness = thickness;
    c.kind = ShapeKind::Box;
    PushShape(c, durationTicks);
}

void DebugShapePool::AddArrow(Vec2 start, Vec2 end, Color color, u32 durationTicks, f32 thickness) noexcept {
    ShapeCmd c{};
    c.a = start;
    c.b = end;
    c.color = color;
    c.thickness = thickness;
    c.kind = ShapeKind::Arrow;
    PushShape(c, durationTicks);
}

void DebugShapePool::AddCircle(Vec2 center, f32 radius, Color color, u32 durationTicks, f32 thickness) noexcept {
    ShapeCmd c{};
    c.a = center;
    c.b = {radius, 0.0f};
    c.color = color;
    c.thickness = thickness;
    c.kind = ShapeKind::Circle;
    PushShape(c, durationTicks);
}

void DebugShapePool::EmitAndExpire(u64 currentTick, DebugBatcher& worldBatcher) noexcept {
    m_currentTick = static_cast<u32>(currentTick);
    u32 active = 0;
    for (ShapeCmd& s : m_shapes) {
        if (!s.active) continue;
        if (m_currentTick >= s.expireTick) {
            s.active = false;
            continue;
        }
        ++active;
        switch (s.kind) {
            case ShapeKind::Line:
                worldBatcher.PushLine(s.a, s.b, s.thickness, s.color);
                break;
            case ShapeKind::Box:
                worldBatcher.PushBoxOutline({s.a.x, s.a.y, s.b.x, s.b.y}, s.thickness, s.color);
                break;
            case ShapeKind::Arrow:
                worldBatcher.PushArrow(s.a, s.b, s.thickness * 3.5f, s.thickness, s.color);
                break;
            case ShapeKind::Circle:
                worldBatcher.PushCircleOutline(s.a, s.b.x, 16, s.thickness, s.color);
                break;
        }
    }
    m_activeCount = active;
}

} // namespace en
