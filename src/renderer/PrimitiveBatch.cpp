#include "renderer/PrimitiveBatch.h"
#include "renderer/RenderContext.h"
#include "renderer/DebugFont.h"

#include <cmath>
#include <algorithm>

namespace EngineN {

// Solid & Outline Rectangles

void PrimitiveBatch::FillRect(const FRect& rect, Color color) noexcept {
    if (rect.IsEmpty()) return;

    const Vec2 p0{rect.x, rect.y};
    const Vec2 p1{rect.x + rect.w, rect.y};
    const Vec2 p2{rect.x + rect.w, rect.y + rect.h};
    const Vec2 p3{rect.x, rect.y + rect.h};

    // Transform quad corners into view space
    const auto& state = m_ctx.GetState();
    Vec2 v0 = state.viewMatrix.TransformPoint(p0);
    Vec2 v1 = state.viewMatrix.TransformPoint(p1);
    Vec2 v2 = state.viewMatrix.TransformPoint(p2);
    Vec2 v3 = state.viewMatrix.TransformPoint(p3);

    if (state.pixelSnap) {
        v0.x = std::round(v0.x); v0.y = std::round(v0.y);
        v1.x = std::round(v1.x); v1.y = std::round(v1.y);
        v2.x = std::round(v2.x); v2.y = std::round(v2.y);
        v3.x = std::round(v3.x); v3.y = std::round(v3.y);
    }

    const u32 packed = (state.tint * color).Premultiplied().ToPackedRGBA8();

    // Reserve 1 quad (4 vertices, 6 indices)
    auto res = m_ctx.Queue().Reserve(4, 6);
    if (!res.isValid) return;

    // Primitives sample the center of the built-in 1x1 white texture
    res.vertices[0] = {v0.x, v0.y, 0.5f, 0.5f, packed};
    res.vertices[1] = {v1.x, v1.y, 0.5f, 0.5f, packed};
    res.vertices[2] = {v2.x, v2.y, 0.5f, 0.5f, packed};
    res.vertices[3] = {v3.x, v3.y, 0.5f, 0.5f, packed};

    res.indices[0] = 0; res.indices[1] = 1; res.indices[2] = 2;
    res.indices[3] = 0; res.indices[4] = 2; res.indices[5] = 3;

    const u64 key = SortKey::Pack(
        state.pass, state.layer, state.depth, state.blend, state.material,
        TextureHandle::White().index
    );

    m_ctx.Queue().Commit(res, key, state.scissorId, 4, 6);
}

void PrimitiveBatch::StrokeRect(const FRect& rect, f32 thickness, Color color) noexcept {
    if (rect.IsEmpty() || thickness <= 0.0f) return;

    const f32 ht = thickness * 0.5f;
    const f32 innerW = rect.w - thickness;
    const f32 innerH = rect.h - thickness;

    // Degrade to solid rect if thickness consumes inner area
    if (innerW <= 0.0f || innerH <= 0.0f) {
        FillRect({rect.x - ht, rect.y - ht, rect.w + thickness, rect.h + thickness}, color);
        return;
    }

    const auto& state = m_ctx.GetState();

    // 4 Outer corner points
    Vec2 o0 = state.viewMatrix.TransformPoint({rect.x - ht, rect.y - ht});
    Vec2 o1 = state.viewMatrix.TransformPoint({rect.x + rect.w + ht, rect.y - ht});
    Vec2 o2 = state.viewMatrix.TransformPoint({rect.x + rect.w + ht, rect.y + rect.h + ht});
    Vec2 o3 = state.viewMatrix.TransformPoint({rect.x - ht, rect.y + rect.h + ht});

    // 4 Inner corner points
    Vec2 i0 = state.viewMatrix.TransformPoint({rect.x + ht, rect.y + ht});
    Vec2 i1 = state.viewMatrix.TransformPoint({rect.x + rect.w - ht, rect.y + ht});
    Vec2 i2 = state.viewMatrix.TransformPoint({rect.x + rect.w - ht, rect.y + rect.h - ht});
    Vec2 i3 = state.viewMatrix.TransformPoint({rect.x + ht, rect.y + rect.h - ht});

    const u32 packed = (state.tint * color).Premultiplied().ToPackedRGBA8();

    // Reserve 4 perimeter border quads (8 vertices, 24 indices)
    auto res = m_ctx.Queue().Reserve(8, 24);
    if (!res.isValid) return;

    res.vertices[0] = {o0.x, o0.y, 0.5f, 0.5f, packed};
    res.vertices[1] = {o1.x, o1.y, 0.5f, 0.5f, packed};
    res.vertices[2] = {o2.x, o2.y, 0.5f, 0.5f, packed};
    res.vertices[3] = {o3.x, o3.y, 0.5f, 0.5f, packed};

    res.vertices[4] = {i0.x, i0.y, 0.5f, 0.5f, packed};
    res.vertices[5] = {i1.x, i1.y, 0.5f, 0.5f, packed};
    res.vertices[6] = {i2.x, i2.y, 0.5f, 0.5f, packed};
    res.vertices[7] = {i3.x, i3.y, 0.5f, 0.5f, packed};

    // Top quad
    res.indices[0]  = 0; res.indices[1]  = 1; res.indices[2]  = 5;
    res.indices[3]  = 0; res.indices[4]  = 5; res.indices[5]  = 4;
    // Right quad
    res.indices[6]  = 1; res.indices[7]  = 2; res.indices[8]  = 6;
    res.indices[9]  = 1; res.indices[10] = 6; res.indices[11] = 5;
    // Bottom quad
    res.indices[12] = 2; res.indices[13] = 3; res.indices[14] = 7;
    res.indices[15] = 2; res.indices[16] = 7; res.indices[17] = 6;
    // Left quad
    res.indices[18] = 3; res.indices[19] = 0; res.indices[20] = 4;
    res.indices[21] = 3; res.indices[22] = 4; res.indices[23] = 7;

    const u64 key = SortKey::Pack(
        state.pass, state.layer, state.depth, state.blend, state.material,
        TextureHandle::White().index
    );

    m_ctx.Queue().Commit(res, key, state.scissorId, 8, 24);
}

// Lines & Triangles

void PrimitiveBatch::Line(Vec2 a, Vec2 b, f32 thickness, Color color) noexcept {
    const Vec2 delta = b - a;
    const f32 len = delta.Length();
    if (len < 1e-6f || thickness <= 0.0f) return;

    // Expand line into a screen-aligned oriented rectangle
    const Vec2 normal{-delta.y / len, delta.x / len};
    const Vec2 offset = normal * (thickness * 0.5f);

    const auto& state = m_ctx.GetState();
    Vec2 v0 = state.viewMatrix.TransformPoint(a + offset);
    Vec2 v1 = state.viewMatrix.TransformPoint(b + offset);
    Vec2 v2 = state.viewMatrix.TransformPoint(b - offset);
    Vec2 v3 = state.viewMatrix.TransformPoint(a - offset);

    const u32 packed = (state.tint * color).Premultiplied().ToPackedRGBA8();

    auto res = m_ctx.Queue().Reserve(4, 6);
    if (!res.isValid) return;

    res.vertices[0] = {v0.x, v0.y, 0.5f, 0.5f, packed};
    res.vertices[1] = {v1.x, v1.y, 0.5f, 0.5f, packed};
    res.vertices[2] = {v2.x, v2.y, 0.5f, 0.5f, packed};
    res.vertices[3] = {v3.x, v3.y, 0.5f, 0.5f, packed};

    res.indices[0] = 0; res.indices[1] = 1; res.indices[2] = 2;
    res.indices[3] = 0; res.indices[4] = 2; res.indices[5] = 3;

    const u64 key = SortKey::Pack(
        state.pass, state.layer, state.depth, state.blend, state.material,
        TextureHandle::White().index
    );

    m_ctx.Queue().Commit(res, key, state.scissorId, 4, 6);
}

void PrimitiveBatch::FillTriangle(Vec2 a, Vec2 b, Vec2 c, Color color) noexcept {
    const auto& state = m_ctx.GetState();
    const Vec2 v0 = state.viewMatrix.TransformPoint(a);
    const Vec2 v1 = state.viewMatrix.TransformPoint(b);
    const Vec2 v2 = state.viewMatrix.TransformPoint(c);

    const u32 packed = (state.tint * color).Premultiplied().ToPackedRGBA8();

    auto res = m_ctx.Queue().Reserve(3, 3);
    if (!res.isValid) return;

    res.vertices[0] = {v0.x, v0.y, 0.5f, 0.5f, packed};
    res.vertices[1] = {v1.x, v1.y, 0.5f, 0.5f, packed};
    res.vertices[2] = {v2.x, v2.y, 0.5f, 0.5f, packed};

    res.indices[0] = 0; res.indices[1] = 1; res.indices[2] = 2;

    const u64 key = SortKey::Pack(
        state.pass, state.layer, state.depth, state.blend, state.material,
        TextureHandle::White().index
    );

    m_ctx.Queue().Commit(res, key, state.scissorId, 3, 3);
}

// Circles & Rings

void PrimitiveBatch::FillCircle(Vec2 center, f32 radius, Color color) noexcept {
    if (radius <= 0.0f) return;

    const auto& state = m_ctx.GetState();
    const f32 effectiveRadius = radius * state.view.zoom;
    // Adapt segment count dynamically to preserve smooth curvature: clamped to [12, 64]
    const u32 n = std::clamp(static_cast<u32>(std::ceil(kPi / std::sqrt(0.5f / std::max(1.0f, effectiveRadius)))), 12u, 64u);

    const Vec2 centerV = state.viewMatrix.TransformPoint(center);
    const u32 packed = (state.tint * color).Premultiplied().ToPackedRGBA8();

    const u32 vCount = n + 1;
    const u32 iCount = 3 * n;

    auto res = m_ctx.Queue().Reserve(vCount, iCount);
    if (!res.isValid) return;

    res.vertices[0] = {centerV.x, centerV.y, 0.5f, 0.5f, packed};

    const f32 angleStep = kTwoPi / static_cast<f32>(n);
    for (u32 i = 0; i < n; ++i) {
        const f32 angle = static_cast<f32>(i) * angleStep;
        const Vec2 p = center + Vec2{std::cos(angle) * radius, std::sin(angle) * radius};
        const Vec2 v = state.viewMatrix.TransformPoint(p);
        res.vertices[i + 1] = {v.x, v.y, 0.5f, 0.5f, packed};

        const u32 next = (i + 1 == n) ? 1 : (i + 2);
        res.indices[i * 3 + 0] = 0;
        res.indices[i * 3 + 1] = i + 1;
        res.indices[i * 3 + 2] = next;
    }

    const u64 key = SortKey::Pack(
        state.pass, state.layer, state.depth, state.blend, state.material,
        TextureHandle::White().index
    );

    m_ctx.Queue().Commit(res, key, state.scissorId, vCount, iCount);
}

void PrimitiveBatch::StrokeCircle(Vec2 center, f32 radius, f32 thickness, Color color) noexcept {
    if (radius <= 0.0f || thickness <= 0.0f) return;

    const f32 rOut = radius + thickness * 0.5f;
    const f32 rIn = radius - thickness * 0.5f;

    if (rIn <= 0.0f) {
        FillCircle(center, rOut, color);
        return;
    }

    const auto& state = m_ctx.GetState();
    const f32 effectiveRadius = rOut * state.view.zoom;
    const u32 n = std::clamp(static_cast<u32>(std::ceil(kPi / std::sqrt(0.5f / std::max(1.0f, effectiveRadius)))), 12u, 64u);

    const u32 packed = (state.tint * color).Premultiplied().ToPackedRGBA8();

    const u32 vCount = 2 * n;
    const u32 iCount = 6 * n;

    auto res = m_ctx.Queue().Reserve(vCount, iCount);
    if (!res.isValid) return;

    const f32 angleStep = kTwoPi / static_cast<f32>(n);
    for (u32 i = 0; i < n; ++i) {
        const f32 angle = static_cast<f32>(i) * angleStep;
        const f32 cosA = std::cos(angle);
        const f32 sinA = std::sin(angle);

        const Vec2 pOut = state.viewMatrix.TransformPoint(center + Vec2{cosA * rOut, sinA * rOut});
        const Vec2 pIn  = state.viewMatrix.TransformPoint(center + Vec2{cosA * rIn, sinA * rIn});

        res.vertices[i]     = {pOut.x, pOut.y, 0.5f, 0.5f, packed};
        res.vertices[i + n] = {pIn.x, pIn.y, 0.5f, 0.5f, packed};

        const u32 next = (i + 1 == n) ? 0 : (i + 1);
        res.indices[i * 6 + 0] = i;
        res.indices[i * 6 + 1] = next;
        res.indices[i * 6 + 2] = next + n;

        res.indices[i * 6 + 3] = i;
        res.indices[i * 6 + 4] = next + n;
        res.indices[i * 6 + 5] = i + n;
    }

    const u64 key = SortKey::Pack(
        state.pass, state.layer, state.depth, state.blend, state.material,
        TextureHandle::White().index
    );

    m_ctx.Queue().Commit(res, key, state.scissorId, vCount, iCount);
}

// Capsules (Hitboxes & Hurtboxes)

void PrimitiveBatch::FillCapsule(Vec2 a, Vec2 b, f32 radius, Color color) noexcept {
    const Vec2 delta = b - a;
    const f32 len = delta.Length();
    if (len < 1e-5f) {
        FillCircle(a, radius, color);
        return;
    }

    const auto& state = m_ctx.GetState();
    const Vec2 normal = Vec2{-delta.y / len, delta.x / len} * radius;

    // Central rectangular body connecting the two capsule endpoints
    const Vec2 vr0 = state.viewMatrix.TransformPoint(a + normal);
    const Vec2 vr1 = state.viewMatrix.TransformPoint(b + normal);
    const Vec2 vr2 = state.viewMatrix.TransformPoint(b - normal);
    const Vec2 vr3 = state.viewMatrix.TransformPoint(a - normal);

    const u32 packed = (state.tint * color).Premultiplied().ToPackedRGBA8();

    constexpr u32 kSemicircleSegs = 12;
    const u32 vCount = 4 + 2 * kSemicircleSegs;
    const u32 iCount = 6 + 6 * kSemicircleSegs;

    auto res = m_ctx.Queue().Reserve(vCount, iCount);
    if (!res.isValid) return;

    // Body rectangle quad
    res.vertices[0] = {vr0.x, vr0.y, 0.5f, 0.5f, packed};
    res.vertices[1] = {vr1.x, vr1.y, 0.5f, 0.5f, packed};
    res.vertices[2] = {vr2.x, vr2.y, 0.5f, 0.5f, packed};
    res.vertices[3] = {vr3.x, vr3.y, 0.5f, 0.5f, packed};

    res.indices[0] = 0; res.indices[1] = 1; res.indices[2] = 2;
    res.indices[3] = 0; res.indices[4] = 2; res.indices[5] = 3;

    u32 curV = 4;
    u32 curI = 6;

    // Semicircle cap A
    const f32 angleA = std::atan2(-delta.x, delta.y);
    for (u32 i = 0; i < kSemicircleSegs; ++i) {
        const f32 t0 = static_cast<f32>(i) / static_cast<f32>(kSemicircleSegs);
        const f32 t1 = static_cast<f32>(i + 1) / static_cast<f32>(kSemicircleSegs);
        const f32 th0 = angleA + kHalfPi + t0 * kPi;
        const f32 th1 = angleA + kHalfPi + t1 * kPi;

        const Vec2 p0 = state.viewMatrix.TransformPoint(a);
        const Vec2 p1 = state.viewMatrix.TransformPoint(a + Vec2{std::cos(th0) * radius, std::sin(th0) * radius});
        const Vec2 p2 = state.viewMatrix.TransformPoint(a + Vec2{std::cos(th1) * radius, std::sin(th1) * radius});

        res.vertices[curV + 0] = {p0.x, p0.y, 0.5f, 0.5f, packed};
        res.vertices[curV + 1] = {p1.x, p1.y, 0.5f, 0.5f, packed};
        res.vertices[curV + 2] = {p2.x, p2.y, 0.5f, 0.5f, packed};

        res.indices[curI + 0] = curV + 0;
        res.indices[curI + 1] = curV + 1;
        res.indices[curI + 2] = curV + 2;

        curV += 3;
        curI += 3;
    }

    // Semicircle cap B
    for (u32 i = 0; i < kSemicircleSegs; ++i) {
        const f32 t0 = static_cast<f32>(i) / static_cast<f32>(kSemicircleSegs);
        const f32 t1 = static_cast<f32>(i + 1) / static_cast<f32>(kSemicircleSegs);
        const f32 th0 = angleA - kHalfPi + t0 * kPi;
        const f32 th1 = angleA - kHalfPi + t1 * kPi;

        const Vec2 p0 = state.viewMatrix.TransformPoint(b);
        const Vec2 p1 = state.viewMatrix.TransformPoint(b + Vec2{std::cos(th0) * radius, std::sin(th0) * radius});
        const Vec2 p2 = state.viewMatrix.TransformPoint(b + Vec2{std::cos(th1) * radius, std::sin(th1) * radius});

        res.vertices[curV + 0] = {p0.x, p0.y, 0.5f, 0.5f, packed};
        res.vertices[curV + 1] = {p1.x, p1.y, 0.5f, 0.5f, packed};
        res.vertices[curV + 2] = {p2.x, p2.y, 0.5f, 0.5f, packed};

        res.indices[curI + 0] = curV + 0;
        res.indices[curI + 1] = curV + 1;
        res.indices[curI + 2] = curV + 2;

        curV += 3;
        curI += 3;
    }

    const u64 key = SortKey::Pack(
        state.pass, state.layer, state.depth, state.blend, state.material,
        TextureHandle::White().index
    );

    m_ctx.Queue().Commit(res, key, state.scissorId, curV, curI);
}

void PrimitiveBatch::StrokeCapsule(Vec2 a, Vec2 b, f32 radius, f32 thickness, Color color) noexcept {
    const Vec2 delta = b - a;
    const f32 len = delta.Length();
    if (len < 1e-5f) {
        StrokeCircle(a, radius, thickness, color);
        return;
    }

    const Vec2 normal = Vec2{-delta.y / len, delta.x / len} * radius;
    // Outline consists of two parallel side lines and two end arc caps
    Line(a + normal, b + normal, thickness, color);
    Line(a - normal, b - normal, thickness, color);
    StrokeCircle(a, radius, thickness, color);
    StrokeCircle(b, radius, thickness, color);
}

// Grids & Debug Helpers

void PrimitiveBatch::Grid(const FRect& bounds, f32 cellSize, f32 thickness, Color color) noexcept {
    if (bounds.IsEmpty() || cellSize <= 0.0f || thickness <= 0.0f) return;

    const u32 numV = static_cast<u32>(bounds.w / cellSize) + 1;
    const u32 numH = static_cast<u32>(bounds.h / cellSize) + 1;

    // Vertical grid lines
    for (u32 i = 0; i < numV; ++i) {
        const f32 x = bounds.x + static_cast<f32>(i) * cellSize;
        Line({x, bounds.y}, {x, bounds.y + bounds.h}, thickness, color);
    }
    // Horizontal grid lines
    for (u32 j = 0; j < numH; ++j) {
        const f32 y = bounds.y + static_cast<f32>(j) * cellSize;
        Line({bounds.x, y}, {bounds.x + bounds.w, y}, thickness, color);
    }
}

void PrimitiveBatch::DebugHitbox(const FRect& aabb, Color strokeColor, Color fillColor) noexcept {
    if (fillColor.a > 0.0f) {
        FillRect(aabb, fillColor);
    }
    StrokeRect(aabb, 1.0f, strokeColor);
}

void PrimitiveBatch::DebugRay(Vec2 origin, Vec2 direction, f32 length, Color color) noexcept {
    Line(origin, origin + direction.Normalized() * length, 1.5f, color);
}

void PrimitiveBatch::Text(Vec2 pos, std::string_view str, Color color, f32 scale) noexcept {
    if (str.empty() || scale <= 0.0f) return;

    const f32 startX = pos.x;
    f32 curX = pos.x;
    f32 curY = pos.y;
    const f32 charSpacing = 8.0f * scale;
    const f32 lineSpacing = 10.0f * scale;

    for (char c : str) {
        if (c == '\n') {
            curX = startX;
            curY += lineSpacing;
            continue;
        }

        if (c < 32 || c > 126) {
            c = '?';
        }

        const u8* glyph = kDebugFont8x8[static_cast<u8>(c) - 32];
        for (int row = 0; row < 8; ++row) {
            const u8 rowBits = glyph[row];
            if (rowBits == 0) continue;

            int col = 0;
            while (col < 8) {
                // Find contiguous run of set bits to emit as a single rectangle
                if (rowBits & (0x80 >> col)) {
                    const int runStart = col;
                    while (col < 8 && (rowBits & (0x80 >> col))) {
                        ++col;
                    }
                    const int runLen = col - runStart;
                    FillRect(
                        {curX + static_cast<f32>(runStart) * scale,
                         curY + static_cast<f32>(row) * scale,
                         static_cast<f32>(runLen) * scale,
                         scale},
                        color
                    );
                } else {
                    ++col;
                }
            }
        }

        curX += charSpacing;
    }
}

} // namespace EngineN
