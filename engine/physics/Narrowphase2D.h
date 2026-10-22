#pragma once

#include "physics/PhysicsTypes.h"

namespace en {

// Stateless 2D narrowphase collision detection, 2-point Sutherland-Hodgman manifold clipping, speculative contacts, and continuous TOI queries.
// Pure functional utility with zero dynamic state; core intersection routines are constexpr for compile-time verification.
// Invariant: contact normals always point from Entity A toward Entity B, and separation <= 0 indicates overlap while separation > 0 indicates a speculative gap.
class Narrowphase2D {
public:
    [[nodiscard]] static constexpr u16 CombineFriction(u16 fA_q16, u16 fB_q16) noexcept {
        return static_cast<u16>((static_cast<u32>(fA_q16) + static_cast<u32>(fB_q16)) >> 1U);
    }

    [[nodiscard]] static constexpr u16 CombineRestitution(u16 rA_q16, u16 rB_q16) noexcept {
        return std::max(rA_q16, rB_q16);
    }

    [[nodiscard]] static constexpr bool ClipRaySlabAxis(
        f32 origin,
        f32 dir,
        f32 minB,
        f32 maxB,
        f32& ioTMin,
        f32& ioTMax,
        f32& outNormalSign) noexcept {
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
        f32 originX,
        f32 originY,
        f32 dirX,
        f32 dirY,
        f32 maxDist,
        f32 minX,
        f32 minY,
        f32 maxX,
        f32 maxY) noexcept {
        RayHit2D hit{};
        if (originX >= minX && originX <= maxX && originY >= minY && originY <= maxY) {
            hit.fraction = 0.0f;
            hit.distance = 0.0f;
            hit.pointX = originX;
            hit.pointY = originY;
            hit.normalX = -dirX;
            hit.normalY = -dirY;
            hit.hitValid = 1U;
            hit.startSolid = 1U;
            return hit;
        }

        f32 tMin = -1e-5f;
        f32 tMax = maxDist;
        f32 nxSign = 0.0f;
        f32 nySign = 0.0f;
        if (!ClipRaySlabAxis(originX, dirX, minX, maxX, tMin, tMax, nxSign)) return hit;
        const f32 tAfterX = tMin;
        if (!ClipRaySlabAxis(originY, dirY, minY, maxY, tMin, tMax, nySign)) return hit;
        if (tMin < 0.0f || tMin > maxDist) return hit;

        hit.distance = tMin;
        hit.fraction = (maxDist > 1e-8f) ? (tMin / maxDist) : 0.0f;
        hit.pointX = originX + dirX * tMin;
        hit.pointY = originY + dirY * tMin;
        if (tMin > tAfterX || nySign != 0.0f) {
            hit.normalX = 0.0f;
            hit.normalY = nySign;
        } else {
            hit.normalX = nxSign;
            hit.normalY = 0.0f;
        }
        hit.hitValid = 1U;
        return hit;
    }

    [[nodiscard]] static RayHit2D RaycastCircle(
        f32 originX,
        f32 originY,
        f32 dirX,
        f32 dirY,
        f32 maxDist,
        f32 centerX,
        f32 centerY,
        f32 radius) noexcept;

    [[nodiscard]] static RayHit2D RaycastSegment(
        f32 originX,
        f32 originY,
        f32 dirX,
        f32 dirY,
        f32 maxDist,
        Vec2 p0,
        Vec2 p1) noexcept;

    [[nodiscard]] static constexpr SweepHit2D SweepAabbVsAabb(
        f32 centerAX,
        f32 centerAY,
        f32 halfAW,
        f32 halfAH,
        f32 deltaX,
        f32 deltaY,
        f32 minBX,
        f32 minBY,
        f32 maxBX,
        f32 maxBY) noexcept {
        constexpr f32 kSkin = 2e-4f;
        const f32 expMinX = minBX - halfAW;
        const f32 expMinY = minBY - halfAH;
        const f32 expMaxX = maxBX + halfAW;
        const f32 expMaxY = maxBY + halfAH;

        SweepHit2D out{};
        if (centerAX > expMinX + kSkin && centerAX < expMaxX - kSkin &&
            centerAY > expMinY + kSkin && centerAY < expMaxY - kSkin) {
            const f32 dxLeft = centerAX - expMinX;
            const f32 dxRight = expMaxX - centerAX;
            const f32 dyDown = centerAY - expMinY;
            const f32 dyUp = expMaxY - centerAY;
            const f32 minXPen = std::min(dxLeft, dxRight);
            const f32 minYPen = std::min(dyDown, dyUp);
            out.toi = 0.0f;
            out.pointX = centerAX;
            out.pointY = centerAY;
            out.startPenetrating = 1U;
            out.hitValid = 1U;
            if (minXPen < minYPen) {
                out.penetration = minXPen;
                out.normalX = (dxLeft < dxRight) ? -1.0f : 1.0f;
                out.normalY = 0.0f;
            } else {
                out.penetration = minYPen;
                out.normalX = 0.0f;
                out.normalY = (dyDown < dyUp) ? -1.0f : 1.0f;
            }
            return out;
        }

        f32 tMin = -0.25f;
        f32 tMax = 1.0f;
        f32 nxSign = 0.0f;
        f32 nySign = 0.0f;
        if (!ClipRaySlabAxis(centerAX, deltaX, expMinX, expMaxX, tMin, tMax, nxSign)) return out;
        const f32 tAfterX = tMin;
        if (!ClipRaySlabAxis(centerAY, deltaY, expMinY, expMaxY, tMin, tMax, nySign)) return out;
        if (tMax < 0.0f || tMin > 1.0f) return out;

        const f32 moveLen = std::abs(deltaX) + std::abs(deltaY);
        const f32 minAllowedT = (moveLen > 1e-6f) ? (-2.0f * kSkin / moveLen) : -1e-3f;
        if (tMin < minAllowedT) return out;

        if (tMin > tAfterX && nySign != 0.0f) {
            out.normalX = 0.0f;
            out.normalY = nySign;
        } else if (nxSign != 0.0f) {
            out.normalX = nxSign;
            out.normalY = 0.0f;
        } else if (nySign != 0.0f) {
            out.normalX = 0.0f;
            out.normalY = nySign;
        } else {
            return out;
        }

        if ((deltaX * out.normalX + deltaY * out.normalY) >= -1e-7f) {
            return out;
        }

        out.toi = std::max(0.0f, tMin);
        out.pointX = centerAX + deltaX * out.toi;
        out.pointY = centerAY + deltaY * out.toi;
        out.hitValid = 1U;
        return out;
    }

    // Sweeps an AABB (or circle approximated by its bounding radius) against an arbitrary 2D line segment p0 -> p1.
    [[nodiscard]] static SweepHit2D SweepAabbVsSegment(
        f32 centerAX,
        f32 centerAY,
        f32 halfAW,
        f32 halfAH,
        f32 deltaX,
        f32 deltaY,
        Vec2 p0,
        Vec2 p1,
        bool oneSided = false) noexcept;

    [[nodiscard]] static constexpr bool CollideAabbAabb(
        f32 centerAX,
        f32 centerAY,
        f32 halfAW,
        f32 halfAH,
        f32 centerBX,
        f32 centerBY,
        f32 halfBW,
        f32 halfBH,
        ContactManifold2D& outManifold,
        f32 speculativeMargin = 0.0f) noexcept {
        const f32 dx = centerBX - centerAX;
        const f32 dy = centerBY - centerAY;
        const f32 sumW = halfAW + halfBW;
        const f32 sumH = halfAH + halfBH;
        const f32 sepX = std::abs(dx) - sumW;
        const f32 sepY = std::abs(dy) - sumH;

        if (sepX > speculativeMargin || sepY > speculativeMargin) {
            return false;
        }

        if (sepX > sepY) {
            outManifold.normalX = (dx < 0.0f) ? -1.0f : 1.0f;
            outManifold.normalY = 0.0f;
            const f32 contactX = centerAX + outManifold.normalX * halfAW;
            const f32 clipMinY = std::max(centerAY - halfAH, centerBY - halfBH);
            const f32 clipMaxY = std::min(centerAY + halfAH, centerBY + halfBH);
            if (clipMaxY - clipMinY > 1e-4f) {
                outManifold.pointCount = 2U;
                outManifold.pointX[0] = contactX;
                outManifold.pointY[0] = clipMinY;
                outManifold.separation[0] = sepX;
                outManifold.pointX[1] = contactX;
                outManifold.pointY[1] = clipMaxY;
                outManifold.separation[1] = sepX;
            } else {
                outManifold.pointCount = 1U;
                outManifold.pointX[0] = contactX;
                outManifold.pointY[0] = 0.5f * (clipMinY + clipMaxY);
                outManifold.separation[0] = sepX;
            }
        } else {
            outManifold.normalX = 0.0f;
            outManifold.normalY = (dy < 0.0f) ? -1.0f : 1.0f;
            const f32 contactY = centerAY + outManifold.normalY * halfAH;
            const f32 clipMinX = std::max(centerAX - halfAW, centerBX - halfBW);
            const f32 clipMaxX = std::min(centerAX + halfAW, centerBX + halfBW);
            if (clipMaxX - clipMinX > 1e-4f) {
                outManifold.pointCount = 2U;
                outManifold.pointX[0] = clipMinX;
                outManifold.pointY[0] = contactY;
                outManifold.separation[0] = sepY;
                outManifold.pointX[1] = clipMaxX;
                outManifold.pointY[1] = contactY;
                outManifold.separation[1] = sepY;
            } else {
                outManifold.pointCount = 1U;
                outManifold.pointX[0] = 0.5f * (clipMinX + clipMaxX);
                outManifold.pointY[0] = contactY;
                outManifold.separation[0] = sepY;
            }
        }
        return true;
    }

    [[nodiscard]] static bool CollideCircleCircle(
        f32 centerAX,
        f32 centerAY,
        f32 radiusA,
        f32 centerBX,
        f32 centerBY,
        f32 radiusB,
        ContactManifold2D& outManifold,
        f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideCircleAabb(
        f32 circleX,
        f32 circleY,
        f32 radius,
        f32 boxCenterX,
        f32 boxCenterY,
        f32 halfW,
        f32 halfH,
        ContactManifold2D& outManifold,
        f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideCircleSegment(
        f32 circleX,
        f32 circleY,
        f32 radius,
        Vec2 p0,
        Vec2 p1,
        ContactManifold2D& outManifold,
        f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideAabbSegment(
        f32 boxCenterX,
        f32 boxCenterY,
        f32 halfW,
        f32 halfH,
        Vec2 p0,
        Vec2 p1,
        ContactManifold2D& outManifold,
        f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideObbObb(
        f32 centerAX,
        f32 centerAY,
        f32 halfAW,
        f32 halfAH,
        f32 cosA,
        f32 sinA,
        f32 centerBX,
        f32 centerBY,
        f32 halfBW,
        f32 halfBH,
        f32 cosB,
        f32 sinB,
        ContactManifold2D& outManifold,
        f32 speculativeMargin = 0.0f) noexcept;

    [[nodiscard]] static bool CollideShapes(
        EntityId entityA,
        const Collider2D& colA,
        const WorldTransform2D& xfA,
        EntityId entityB,
        const Collider2D& colB,
        const WorldTransform2D& xfB,
        ContactManifold2D& outManifold,
        f32 speculativeMargin = 0.0f) noexcept;
};

} // namespace en
