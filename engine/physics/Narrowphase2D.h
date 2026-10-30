#pragma once

#include "physics/PhysicsTypes.h"

namespace en {

// Pure 2D narrowphase. Normals A→B; separation < 0 = overlap.
class Narrowphase2D {
public:
    [[nodiscard]] static constexpr u16 CombineFriction(u16 a, u16 b) noexcept {
        return static_cast<u16>((static_cast<u32>(a) + static_cast<u32>(b)) >> 1U);
    }
    [[nodiscard]] static constexpr u16 CombineRestitution(u16 a, u16 b) noexcept {
        return std::max(a, b);
    }

    [[nodiscard]] static constexpr bool ClipSlab(
        f32 origin, f32 dir, f32 minB, f32 maxB,
        f32& tMin, f32& tMax, f32& nSign) noexcept {
        nSign = 0.0f;
        if (dir >= -1e-8f && dir <= 1e-8f) return origin >= minB && origin <= maxB;
        const f32 inv = 1.0f / dir;
        f32 t1 = (minB - origin) * inv;
        f32 t2 = (maxB - origin) * inv;
        f32 s = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            s = 1.0f;
        }
        if (t1 > tMin) {
            tMin = t1;
            nSign = s;
        }
        if (t2 < tMax) tMax = t2;
        return tMin <= tMax;
    }

    [[nodiscard]] static constexpr RayHit2D RaycastAabb(
        f32 ox, f32 oy, f32 dx, f32 dy, f32 maxDist,
        f32 minX, f32 minY, f32 maxX, f32 maxY) noexcept {
        if (ox >= minX && ox <= maxX && oy >= minY && oy <= maxY) {
            return {0.0f, 0.0f, ox, oy, -dx, -dy, kNullEntity,
                    TileMaterialFlags::Empty, ShapeKind2D::Aabb, 1U, 1U};
        }
        f32 tMin = -1e-5f, tMax = maxDist, nx = 0.0f, ny = 0.0f;
        if (!ClipSlab(ox, dx, minX, maxX, tMin, tMax, nx)) return {};
        const f32 tX = tMin;
        if (!ClipSlab(oy, dy, minY, maxY, tMin, tMax, ny) || tMin < 0.0f || tMin > maxDist) {
            return {};
        }
        const bool useY = (tMin > tX || ny != 0.0f);
        return {(maxDist > 1e-8f) ? tMin / maxDist : 0.0f, tMin,
                ox + dx * tMin, oy + dy * tMin,
                useY ? 0.0f : nx, useY ? ny : 0.0f,
                kNullEntity, TileMaterialFlags::Empty, ShapeKind2D::Aabb, 1U, 0U};
    }

    [[nodiscard]] static RayHit2D RaycastCircle(
        f32 ox, f32 oy, f32 dx, f32 dy, f32 maxDist,
        f32 cx, f32 cy, f32 r) noexcept;

    [[nodiscard]] static RayHit2D RaycastSegment(
        f32 ox, f32 oy, f32 dx, f32 dy, f32 maxDist,
        Vec2 p0, Vec2 p1) noexcept;

    [[nodiscard]] static constexpr SweepHit2D SweepAabbVsAabb(
        f32 cx, f32 cy, f32 hw, f32 hh,
        f32 dx, f32 dy,
        f32 minX, f32 minY, f32 maxX, f32 maxY) noexcept {
        constexpr f32 kSkin = 2e-4f;
        const f32 eMinX = minX - hw, eMinY = minY - hh;
        const f32 eMaxX = maxX + hw, eMaxY = maxY + hh;

        if (cx > eMinX + kSkin && cx < eMaxX - kSkin &&
            cy > eMinY + kSkin && cy < eMaxY - kSkin) {
            const f32 dl = cx - eMinX, dr = eMaxX - cx;
            const f32 dd = cy - eMinY, du = eMaxY - cy;
            const f32 px = std::min(dl, dr), py = std::min(dd, du);
            if (px < py) {
                return {0.0f, cx, cy, (dl < dr) ? -1.0f : 1.0f, 0.0f, px,
                        kNullEntity, TileMaterialFlags::Empty, 1U, 1U, 0U};
            }
            return {0.0f, cx, cy, 0.0f, (dd < du) ? -1.0f : 1.0f, py,
                    kNullEntity, TileMaterialFlags::Empty, 1U, 1U, 0U};
        }

        f32 tMin = -0.25f, tMax = 1.0f, nx = 0.0f, ny = 0.0f;
        if (!ClipSlab(cx, dx, eMinX, eMaxX, tMin, tMax, nx)) return {};
        const f32 tX = tMin;
        if (!ClipSlab(cy, dy, eMinY, eMaxY, tMin, tMax, ny) || tMax < 0.0f || tMin > 1.0f) {
            return {};
        }
        const f32 move = std::abs(dx) + std::abs(dy);
        const f32 minT = (move > 1e-6f) ? (-2.0f * kSkin / move) : -1e-3f;
        if (tMin < minT) return {};

        f32 nX = 0.0f, nY = 0.0f;
        if (tMin > tX || ny != 0.0f) nY = ny;
        else nX = nx;
        if (nX == 0.0f && nY == 0.0f) {
            const f32 mx = std::min(cx - eMinX, eMaxX - cx);
            const f32 my = std::min(cy - eMinY, eMaxY - cy);
            if (mx < my) nX = (cx < 0.5f * (eMinX + eMaxX)) ? -1.0f : 1.0f;
            else nY = (cy < 0.5f * (eMinY + eMaxY)) ? -1.0f : 1.0f;
        }
        const f32 t = std::clamp(tMin, 0.0f, 1.0f);
        return {t, cx + dx * t, cy + dy * t, nX, nY, 0.0f,
                kNullEntity, TileMaterialFlags::Empty, 0U, 1U, 0U};
    }

    [[nodiscard]] static SweepHit2D SweepAabbVsSegment(
        f32 cx, f32 cy, f32 hw, f32 hh,
        f32 dx, f32 dy,
        Vec2 p0, Vec2 p1, bool oneWay) noexcept;

    [[nodiscard]] static constexpr bool CollideAabbAabb(
        f32 ax, f32 ay, f32 ahw, f32 ahh,
        f32 bx, f32 by, f32 bhw, f32 bhh,
        ContactManifold2D& m, f32 margin = 0.0f) noexcept {
        const f32 dx = bx - ax, dy = by - ay;
        const f32 px = (ahw + bhw + margin) - std::abs(dx);
        const f32 py = (ahh + bhh + margin) - std::abs(dy);
        if (px < 0.0f || py < 0.0f) return false;
        m.pointCount = 1U;
        if (px < py) {
            m.normalX = (dx < 0.0f) ? -1.0f : 1.0f;
            m.normalY = 0.0f;
            m.separation[0] = -px;
            m.pointX[0] = ax + m.normalX * ahw;
            m.pointY[0] = ay;
        } else {
            m.normalX = 0.0f;
            m.normalY = (dy < 0.0f) ? -1.0f : 1.0f;
            m.separation[0] = -py;
            m.pointX[0] = ax;
            m.pointY[0] = ay + m.normalY * ahh;
        }
        return true;
    }

    [[nodiscard]] static bool CollideCircleAabb(
        f32 cx, f32 cy, f32 r,
        f32 bx, f32 by, f32 bhw, f32 bhh,
        ContactManifold2D& m, f32 margin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideCircleCircle(
        f32 ax, f32 ay, f32 ar,
        f32 bx, f32 by, f32 br,
        ContactManifold2D& m, f32 margin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideObbObb(
        f32 ax, f32 ay, f32 ahw, f32 ahh, f32 ac, f32 as,
        f32 bx, f32 by, f32 bhw, f32 bhh, f32 bc, f32 bs,
        ContactManifold2D& m, f32 margin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideShapes(
        EntityId idA, const Collider2D& colA, const WorldTransform2D& xfA,
        EntityId idB, const Collider2D& colB, const WorldTransform2D& xfB,
        ContactManifold2D& out, f32 margin = 0.0f) noexcept;
};

} // namespace en
