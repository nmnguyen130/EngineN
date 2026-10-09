#pragma once

#include "renderer/RendererTypes.h"

namespace EngineN {

class RenderContext;

/// Zero-heap-allocation 2D vector primitive drawer.
/// Primitives sample the built-in 1x1 white texture (slot 1), allowing them
/// to batch and interleave seamlessly with sprites in the same draw calls.
class PrimitiveBatch {
public:
    explicit PrimitiveBatch(RenderContext& context) noexcept
        : m_ctx(context) {}

    // Solid & Outline Rectangles
    void FillRect(const FRect& rect, Color color) noexcept;
    void StrokeRect(const FRect& rect, f32 thickness, Color color) noexcept;

    // Lines & Polygons
    void Line(Vec2 a, Vec2 b, f32 thickness, Color color) noexcept;
    void FillTriangle(Vec2 a, Vec2 b, Vec2 c, Color color) noexcept;

    // Circles & Rings
    void FillCircle(Vec2 center, f32 radius, Color color) noexcept;
    void StrokeCircle(Vec2 center, f32 radius, f32 thickness, Color color) noexcept;

    // Capsules (Hitboxes & Hurtboxes)
    void FillCapsule(Vec2 a, Vec2 b, f32 radius, Color color) noexcept;
    void StrokeCapsule(Vec2 a, Vec2 b, f32 radius, f32 thickness, Color color) noexcept;

    // Grids & Debug Helpers
    void Grid(const FRect& bounds, f32 cellSize, f32 thickness, Color color) noexcept;
    void DebugHitbox(const FRect& aabb, Color strokeColor, Color fillColor = Colors::Transparent) noexcept;
    void DebugRay(Vec2 origin, Vec2 direction, f32 length, Color color) noexcept;
    void Text(Vec2 pos, std::string_view str, Color color, f32 scale = 1.0f) noexcept;

private:
    RenderContext& m_ctx;
};

} // namespace EngineN
