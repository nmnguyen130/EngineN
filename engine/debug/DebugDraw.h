#pragma once

#include "debug/DebugTypes.h"

namespace en {

// Immediate-mode 2D debug primitive writer targeting a RenderList pass and view.
// Non-owning wrapper around RenderList; performs zero heap allocations.
// Invariant: sizeof(DebugDraw) == 16.
class DebugDraw {
public:
    constexpr DebugDraw(
        RenderList& list,
        u8 pass = kDebugWorldPass,
        u8 view = kDebugWorldView,
        u16 depth = kDebugDepth) noexcept
        : m_list(&list), m_depth(depth), m_pass(pass), m_view(view) {}

    void Rect(en::Rect r, Color c) noexcept;
    void Box(en::Rect r, f32 thick, Color c) noexcept;
    void Line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thick, Color c) noexcept;
    void Arrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 head, f32 thick, Color c) noexcept;
    void Circle(f32 cx, f32 cy, f32 r, u8 segs, f32 thick, Color c) noexcept;
    void Cross(f32 cx, f32 cy, f32 half, f32 thick, Color c) noexcept;
    void Text(f32 x, f32 y, std::string_view s, Color c, f32 scale = 1.0f) noexcept;

    [[nodiscard]] static char* Append(char* out, char* end, std::string_view s) noexcept;
    [[nodiscard]] static char* FmtI64(char* out, char* end, i64 v) noexcept;
    [[nodiscard]] static char* FmtF2(char* out, char* end, f32 v) noexcept;

private:
    RenderList* m_list{nullptr};
    u16         m_depth{kDebugDepth};
    u8          m_pass{kDebugWorldPass};
    u8          m_view{kDebugWorldView};
    u32         m_pad{0};
};
static_assert(sizeof(DebugDraw) == 16);

// Fixed-capacity ring buffer of transient and multi-tick persistent 2D debug gizmos.
// Non-owning view over Arena storage; expired shapes are compacted during Emit.
// Invariant: sizeof(ShapePool) == 24, and m_count <= kMaxGizmos.
class ShapePool {
public:
    constexpr void Init(std::span<ShapeCmd, kMaxGizmos> storage) noexcept {
        m_pool = storage.data();
        Clear();
    }

    constexpr void Clear() noexcept {
        m_head  = 0U;
        m_count = 0U;
        m_tick  = 0U;
    }

    constexpr void SetTick(u32 tick) noexcept { m_tick = tick; }

    void Line(f32 x0, f32 y0, f32 x1, f32 y1, Color c, f32 thick = 0.04f, u32 lifeTicks = 0U) noexcept;
    void Box(Rect r, Color c, f32 thick = 0.04f, bool solid = false, u32 lifeTicks = 0U) noexcept;
    void Arrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 head, Color c, f32 thick = 0.04f, u32 lifeTicks = 0U) noexcept;
    void Circle(f32 cx, f32 cy, f32 r, Color c, u8 segs = 16U, f32 thick = 0.04f, u32 lifeTicks = 0U) noexcept;
    void Cross(f32 cx, f32 cy, f32 half, Color c, f32 thick = 0.04f, u32 lifeTicks = 0U) noexcept;
    void TextWorld(f32 x, f32 y, std::string_view text, Color c, f32 scale = 0.05f, u32 lifeTicks = 0U) noexcept;
    void TextHud(f32 x, f32 y, std::string_view text, Color c, f32 scale = 1.0f, u32 lifeTicks = 0U) noexcept;

    void Emit(u32 tick, DebugDraw& world, DebugDraw& hud) noexcept;

    [[nodiscard]] constexpr u32 Count() const noexcept { return m_count; }

private:
    [[nodiscard]] ShapeCmd* Push(ShapeCmd::Kind kind, f32 x0, f32 y0, Color c, f32 thick, u32 life) noexcept;

    ShapeCmd* m_pool{nullptr};
    u32       m_head{0};
    u32       m_count{0};
    u32       m_tick{0};
    u32       m_pad{0};
};
static_assert(sizeof(ShapePool) == 24);

} // namespace en
