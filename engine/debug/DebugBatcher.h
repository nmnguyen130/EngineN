#pragma once

#include "debug/DebugTypes.h"

namespace en {

class alignas(16) DebugDraw {
public:
    constexpr DebugDraw(
        RenderList& list,
        u8 pass = kDebugWorldPass,
        u8 view = kDebugWorldView,
        u16 depth = kDebugDepth) noexcept
        : m_list(&list), m_depth(depth), m_pass(pass), m_view(view) {}

    void Rect(Rect r, Color c) noexcept;
    void Box(Rect r, f32 thick, Color c) noexcept;
    void Obb(Vec2 c, Vec2 he, f32 cosR, f32 sinR, f32 thick, Color col) noexcept;
    void Line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thick, Color c) noexcept;
    void Arrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 head, f32 thick, Color c) noexcept;
    void Circle(f32 cx, f32 cy, f32 r, u8 segs, f32 thick, Color c) noexcept;
    void Capsule(f32 cx, f32 cy, f32 hw, f32 hh, f32 thick, Color c) noexcept;
    void Cross(f32 cx, f32 cy, f32 half, f32 thick, Color c) noexcept;
    void Text(f32 x, f32 y, std::string_view s, Color c, f32 scale = 1.0f) noexcept;

    [[nodiscard]] static char* Append(char* o, char* e, std::string_view s) noexcept;
    [[nodiscard]] static char* FmtI64(char* o, char* e, i64 v) noexcept;
    [[nodiscard]] static char* FmtF2(char* o, char* e, f32 v) noexcept;

    [[nodiscard]] static constexpr Color Unpack(u32 rgba) noexcept {
        constexpr f32 k = 1.0f / 255.0f;
        return {
            static_cast<f32>((rgba >> 24U) & 0xFFU) * k,
            static_cast<f32>((rgba >> 16U) & 0xFFU) * k,
            static_cast<f32>((rgba >>  8U) & 0xFFU) * k,
            static_cast<f32>((rgba >>  0U) & 0xFFU) * k,
        };
    }

private:
    RenderList* m_list{nullptr};
    u16 m_depth{kDebugDepth};
    u8  m_pass{kDebugWorldPass};
    u8  m_view{kDebugWorldView};
};
static_assert(sizeof(DebugDraw) == 16);

// Ring buffer of transient/persistent shapes.
class alignas(16) ShapePool {
public:
    constexpr void Init(std::span<ShapeCmd, kMaxShapes> storage) noexcept {
        m_pool = storage.data();
        Clear();
    }

    constexpr void Clear() noexcept {
        m_head = 0U;
        m_count = 0U;
        m_tick = 0U;
    }

    constexpr void SetTick(u32 t) noexcept { m_tick = t; }

    void Line(f32 x0, f32 y0, f32 x1, f32 y1, Color c, f32 thick = 0.05f, u32 life = 0U) noexcept;
    void Box(Rect r, Color c, f32 thick = 0.05f, bool solid = false, u32 life = 0U) noexcept;
    void Arrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 head, Color c, f32 thick = 0.05f, u32 life = 0U) noexcept;
    void Circle(f32 cx, f32 cy, f32 r, Color c, u8 segs = 16U, f32 thick = 0.05f, u32 life = 0U) noexcept;
    void Capsule(f32 cx, f32 cy, f32 hw, f32 hh, Color c, f32 thick = 0.05f, u32 life = 0U) noexcept;
    void TextWorld(f32 x, f32 y, std::string_view s, Color c, f32 scale = 0.05f, u32 life = 0U) noexcept;
    void TextHud(f32 x, f32 y, std::string_view s, Color c, f32 scale = 1.0f, u32 life = 0U) noexcept;

    void Emit(u32 tick, DebugDraw& world, DebugDraw& hud) noexcept;

    [[nodiscard]] constexpr u32 Count() const noexcept { return m_count; }

private:
    [[nodiscard]] ShapeCmd* Push(ShapeCmd::Kind k, f32 x0, f32 y0, Color c, f32 thick, u32 life) noexcept;

    ShapeCmd* m_pool{nullptr};
    u32 m_head{0U};
    u32 m_count{0U};
    u32 m_tick{0U};
};
static_assert(sizeof(ShapePool) == 24);

} // namespace en
