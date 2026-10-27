#pragma once

#include "physics/PhysicsTypes.h"

namespace en {

// Stateless 2D narrowphase collision detection, Sutherland-Hodgman manifold clipping, and continuous TOI queries.
// Non-owning pure math utility operating on world-space transforms and quantized local bounds.
// Invariant: contact normals always point from entityA toward entityB; separation < 0 indicates overlap.
class Narrowphase2D {
public:
    [[nodiscard]] static constexpr u16 CombineFriction(u16 fA_q16, u16 fB_q16) noexcept {
        return static_cast<u16>((static_cast<u32>(fA_q16) + static_cast<u32>(fB_q16)) >> 1U);
    }

    [[nodiscard]] static constexpr u16 CombineRestitution(u16 rA_q16, u16 rB_q16) noexcept {
        return std::max(rA_q16, rB_q16);
    }

    [[nodiscard]] static constexpr bool ClipRaySlabAxis(
        f32 origin, f32 dir, f32 minB, f32 maxB,
        f32& ioTMin, f32& ioTMax, f32& outNormalSign) noexcept {
        outNormalSign = 0.0f;
        if (dir >= -1e-8f && dir <= 1e-8f) {
            return origin >= minB && origin <= maxB;
        }
        const f32 invD = 1.0f / dir;
        f32 t1 = (minB - origin) * invD;
        f32 t2 = (maxB - origin) * invD;
        f32 sign = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            sign = 1.0f;
        }
        if (t1 > ioTMin) {
            ioTMin = t1;
            outNormalSign = sign;
        }
        if (t2 < ioTMax) {
            ioTMax = t2;
        }
        return ioTMin <= ioTMax;
    }

    [[nodiscard]] static constexpr RayHit2D RaycastAabb(
        f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist,
        f32 minX, f32 minY, f32 maxX, f32 maxY) noexcept {
        if (originX >= minX && originX <= maxX && originY >= minY && originY <= maxY) {
            return {0.0f, 0.0f, originX, originY, -dirX, -dirY, kNullEntity, TileMaterialFlags::Empty, ShapeKind2D::Aabb, 1U, 1U};
        }

        f32 tMin = -1e-5f;
        f32 tMax = maxDist;
        f32 nxSign = 0.0f;
        f32 nySign = 0.0f;
        if (!ClipRaySlabAxis(originX, dirX, minX, maxX, tMin, tMax, nxSign)) {
            return {};
        }
        const f32 tAfterX = tMin;
        if (!ClipRaySlabAxis(originY, dirY, minY, maxY, tMin, tMax, nySign) || tMin < 0.0f || tMin > maxDist) {
            return {};
        }

        const bool useY = (tMin > tAfterX || nySign != 0.0f);
        return {
            (maxDist > 1e-8f) ? (tMin / maxDist) : 0.0f,
            tMin,
            originX + dirX * tMin,
            originY + dirY * tMin,
            useY ? 0.0f : nxSign,
            useY ? nySign : 0.0f,
            kNullEntity,
            TileMaterialFlags::Empty,
            ShapeKind2D::Aabb,
            1U,
            0U};
    }

    [[nodiscard]] static RayHit2D RaycastCircle(
        f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist,
        f32 centerX, f32 centerY, f32 radius) noexcept;

    [[nodiscard]] static RayHit2D RaycastSegment(
        f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist,
        Vec2 p0, Vec2 p1) noexcept;

    [[nodiscard]] static constexpr SweepHit2D SweepAabbVsAabb(
        f32 centerAX, f32 centerAY, f32 halfAW, f32 halfAH,
        f32 deltaX, f32 deltaY,
        f32 minBX, f32 minBY, f32 maxBX, f32 maxBY) noexcept {
        constexpr f32 kSkin = 2e-4f;
        const f32 expMinX = minBX - halfAW;
        const f32 expMinY = minBY - halfAH;
        const f32 expMaxX = maxBX + halfAW;
        const f32 expMaxY = maxBY + halfAH;

        if (centerAX > expMinX + kSkin && centerAX < expMaxX - kSkin &&
            centerAY > expMinY + kSkin && centerAY < expMaxY - kSkin) {
            const f32 dxLeft = centerAX - expMinX;
            const f32 dxRight = expMaxX - centerAX;
            const f32 dyDown = centerAY - expMinY;
            const f32 dyUp = expMaxY - centerAY;
            const f32 minXPen = std::min(dxLeft, dxRight);
            const f32 minYPen = std::min(dyDown, dyUp);
            if (minXPen < minYPen) {
                return {0.0f, centerAX, centerAY, (dxLeft < dxRight) ? -1.0f : 1.0f, 0.0f, minXPen, kNullEntity, TileMaterialFlags::Empty, 1U, 1U, 0U};
            }
            return {0.0f, centerAX, centerAY, 0.0f, (dyDown < dyUp) ? -1.0f : 1.0f, minYPen, kNullEntity, TileMaterialFlags::Empty, 1U, 1U, 0U};
        }

        f32 tMin = -0.25f;
        f32 tMax = 1.0f;
        f32 nxSign = 0.0f;
        f32 nySign = 0.0f;
        if (!ClipRaySlabAxis(centerAX, deltaX, expMinX, expMaxX, tMin, tMax, nxSign)) {
            return {};
        }
        const f32 tAfterX = tMin;
        if (!ClipRaySlabAxis(centerAY, deltaY, expMinY, expMaxY, tMin, tMax, nySign) || tMax < 0.0f || tMin > 1.0f) {
            return {};
        }

        const f32 moveLen = std::abs(deltaX) + std::abs(deltaY);
        const f32 minAllowedT = (moveLen > 1e-6f) ? (-2.0f * kSkin / moveLen) : -1e-3f;
        if (tMin < minAllowedT) {
            return {};
        }

        f32 nx = 0.0f;
        f32 ny = 0.0f;
        if (tMin > tAfterX && nySign != 0.0f) {
            ny = nySign;
        } else if (nxSign != 0.0f) {
            nx = nxSign;
        } else if (nySign != 0.0f) {
            ny = nySign;
        } else {
            return {};
        }

        if ((deltaX * nx + deltaY * ny) >= -1e-7f) {
            return {};
        }

        const f32 toi = std::max(0.0f, tMin);
        return {toi, centerAX + deltaX * toi, centerAY + deltaY * toi, nx, ny, 0.0f, kNullEntity, TileMaterialFlags::Empty, 0U, 1U, 0U};
    }

    [[nodiscard]] static SweepHit2D SweepAabbVsSegment(
        f32 centerAX, f32 centerAY, f32 halfAW, f32 halfAH,
        f32 deltaX, f32 deltaY,
        Vec2 p0, Vec2 p1, bool oneSided = false) noexcept;

    [[nodiscard]] static constexpr bool CollideAabbAabb(
        f32 centerAX, f32 centerAY, f32 halfAW, f32 halfAH,
        f32 centerBX, f32 centerBY, f32 halfBW, f32 halfBH,
        ContactManifold2D& outManifold, f32 speculativeMargin = 0.0f) noexcept {
        const f32 dx = centerBX - centerAX;
        const f32 dy = centerBY - centerAY;
        const f32 sepX = std::abs(dx) - (halfAW + halfBW);
        const f32 sepY = std::abs(dy) - (halfAH + halfBH);
        if (sepX > speculativeMargin || sepY > speculativeMargin) {
            return false;
        }

        if (sepX > sepY) {
            outManifold.normalX = (dx < 0.0f) ? -1.0f : 1.0f;
            outManifold.normalY = 0.0f;
            const f32 cx = centerAX + outManifold.normalX * halfAW;
            const f32 cMinY = std::max(centerAY - halfAH, centerBY - halfBH);
            const f32 cMaxY = std::min(centerAY + halfAH, centerBY + halfBH);
            outManifold.pointCount = (cMaxY - cMinY > 1e-4f) ? 2U : 1U;
            outManifold.pointX[0] = cx;
            outManifold.pointX[1] = cx;
            outManifold.pointY[0] = (outManifold.pointCount == 2U) ? cMinY : 0.5f * (cMinY + cMaxY);
            outManifold.pointY[1] = cMaxY;
            outManifold.separation[0] = sepX;
            outManifold.separation[1] = sepX;
        } else {
            outManifold.normalX = 0.0f;
            outManifold.normalY = (dy < 0.0f) ? -1.0f : 1.0f;
            const f32 cy = centerAY + outManifold.normalY * halfAH;
            const f32 cMinX = std::max(centerAX - halfAW, centerBX - halfBW);
            const f32 cMaxX = std::min(centerAX + halfAW, centerBX + halfBW);
            outManifold.pointCount = (cMaxX - cMinX > 1e-4f) ? 2U : 1U;
            outManifold.pointX[0] = (outManifold.pointCount == 2U) ? cMinX : 0.5f * (cMinX + cMaxX);
            outManifold.pointX[1] = cMaxX;
            outManifold.pointY[0] = cy;
            outManifold.pointY[1] = cy;
            outManifold.separation[0] = sepY;
            outManifold.separation[1] = sepY;
        }
        return true;
    }

    [[nodiscard]] static bool CollideCircleCircle(
        f32 centerAX, f32 centerAY, f32 radiusA,
        f32 centerBX, f32 centerBY, f32 radiusB,
        ContactManifold2D& outManifold, f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideCircleAabb(
        f32 circleX, f32 circleY, f32 radius,
        f32 boxCenterX, f32 boxCenterY, f32 halfW, f32 halfH,
        ContactManifold2D& outManifold, f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideCircleObb(
        f32 circleX, f32 circleY, f32 radius,
        f32 boxCenterX, f32 boxCenterY, f32 halfW, f32 halfH,
        f32 cosB, f32 sinB,
        ContactManifold2D& outManifold, f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideObbObb(
        f32 centerAX, f32 centerAY, f32 halfAW, f32 halfAH, f32 cosA, f32 sinA,
        f32 centerBX, f32 centerBY, f32 halfBW, f32 halfBH, f32 cosB, f32 sinB,
        ContactManifold2D& outManifold, f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideShapes(
        EntityId entityA, const Collider2D& colA, const WorldTransform2D& xfA,
        EntityId entityB, const Collider2D& colB, const WorldTransform2D& xfB,
        ContactManifold2D& outManifold, f32 speculativeMargin = 0.0f) noexcept;
};

} // namespace en
