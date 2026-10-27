#include "physics/Narrowphase2D.h"

namespace en {

namespace {

[[nodiscard]] constexpr Vec2 ClosestPointOnSegment(Vec2 pt, Vec2 a, Vec2 b) noexcept {
    const Vec2 ab = b - a;
    const f32 lenSq = ab.LengthSq();
    return (lenSq > 1e-8f) ? (a + ab * std::clamp((pt - a).Dot(ab) / lenSq, 0.0f, 1.0f)) : a;
}

constexpr void ClosestPointsSegmentSegment(
    Vec2 p1, Vec2 q1, Vec2 p2, Vec2 q2, Vec2& outC1, Vec2& outC2) noexcept {
    const Vec2 d1 = q1 - p1;
    const Vec2 d2 = q2 - p2;
    const Vec2 r = p1 - p2;
    const f32 a = d1.LengthSq();
    const f32 e = d2.LengthSq();
    if (a <= 1e-8f) {
        outC1 = p1;
        outC2 = ClosestPointOnSegment(p1, p2, q2);
        return;
    }
    if (e <= 1e-8f) {
        outC1 = ClosestPointOnSegment(p2, p1, q1);
        outC2 = p2;
        return;
    }

    const f32 b = d1.Dot(d2);
    const f32 c = d1.Dot(r);
    const f32 f = d2.Dot(r);
    const f32 denom = a * e - b * b;

    f32 s = (denom > 1e-8f) ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    f32 t = (b * s + f) / e;
    if (t < 0.0f) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
    } else if (t > 1.0f) {
        t = 1.0f;
        s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
    outC1 = p1 + d1 * s;
    outC2 = p2 + d2 * t;
}

constexpr void ExtractCapsuleWorldSegment(
    Vec2 center, f32 halfW, f32 halfH, const WorldTransform2D& xf, Vec2& outP0, Vec2& outP1) noexcept {
    const f32 segHalf = std::max(0.0f, halfH - halfW);
    const Vec2 axisY{-xf.sinRot, xf.cosRot};
    outP0 = center - axisY * segHalf;
    outP1 = center + axisY * segHalf;
}

[[nodiscard]] constexpr Vec2 ClosestPointOnBox(
    Vec2 pt, Vec2 boxCenter, f32 halfW, f32 halfH, f32 cosB, f32 sinB) noexcept {
    const Vec2 rel = pt - boxCenter;
    const f32 lx = std::clamp(rel.x * cosB + rel.y * sinB, -halfW, halfW);
    const f32 ly = std::clamp(-rel.x * sinB + rel.y * cosB, -halfH, halfH);
    return {boxCenter.x + (lx * cosB - ly * sinB), boxCenter.y + (lx * sinB + ly * cosB)};
}

constexpr void SetSingleContact(
    ContactManifold2D& m, f32 nx, f32 ny, f32 px, f32 py, f32 sep) noexcept {
    m.normalX = nx;
    m.normalY = ny;
    m.pointCount = 1U;
    m.pointX[0] = px;
    m.pointY[0] = py;
    m.separation[0] = sep;
}

[[nodiscard]] bool CollideBoxVsSegment(
    Vec2 boxCenter, f32 halfW, f32 halfH, f32 cosA, f32 sinA,
    Vec2 p0, Vec2 p1, ContactManifold2D& outManifold, f32 speculativeMargin) noexcept {
    const Vec2 edge = p1 - p0;
    const f32 len = edge.Length();
    if (len <= 1e-6f) {
        if (!Narrowphase2D::CollideCircleObb(
                p0.x, p0.y, 0.0f, boxCenter.x, boxCenter.y, halfW, halfH, cosA, sinA, outManifold, speculativeMargin)) {
            return false;
        }
        outManifold.normalX = -outManifold.normalX;
        outManifold.normalY = -outManifold.normalY;
        return true;
    }
    const Vec2 mid = 0.5f * (p0 + p1);
    return Narrowphase2D::CollideObbObb(
        boxCenter.x, boxCenter.y, halfW, halfH, cosA, sinA,
        mid.x, mid.y, 0.5f * len, 1e-3f, edge.x / len, edge.y / len,
        outManifold, speculativeMargin);
}

} // namespace

RayHit2D Narrowphase2D::RaycastCircle(
    f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist,
    f32 centerX, f32 centerY, f32 radius) noexcept {
    const f32 mx = originX - centerX;
    const f32 my = originY - centerY;
    const f32 c = (mx * mx + my * my) - radius * radius;
    if (c <= 0.0f) {
        return {0.0f, 0.0f, originX, originY, -dirX, -dirY, kNullEntity, TileMaterialFlags::Empty, ShapeKind2D::Circle, 1U, 1U};
    }

    const f32 b = mx * dirX + my * dirY;
    const f32 disc = b * b - c;
    if (b > 0.0f || disc < 0.0f) {
        return {};
    }

    const f32 t = -b - std::sqrt(disc);
    if (t < 0.0f || t > maxDist) {
        return {};
    }

    const f32 px = originX + dirX * t;
    const f32 py = originY + dirY * t;
    const f32 invR = (radius > 1e-6f) ? (1.0f / radius) : 1.0f;
    return {
        (maxDist > 1e-8f) ? (t / maxDist) : 0.0f,
        t, px, py,
        (px - centerX) * invR,
        (py - centerY) * invR,
        kNullEntity, TileMaterialFlags::Empty, ShapeKind2D::Circle, 1U, 0U};
}

RayHit2D Narrowphase2D::RaycastSegment(
    f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist,
    Vec2 p0, Vec2 p1) noexcept {
    const Vec2 orig{originX, originY};
    const Vec2 r{dirX * maxDist, dirY * maxDist};
    const Vec2 s = p1 - p0;
    const f32 rCrossS = r.Cross(s);
    if (std::abs(rCrossS) <= 1e-8f) {
        return {};
    }

    const Vec2 qp = p0 - orig;
    const f32 t = qp.Cross(s) / rCrossS;
    const f32 u = qp.Cross(r) / rCrossS;
    if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) {
        return {};
    }

    Vec2 n = s.PerpCCW().Normalized();
    if (n.Dot({dirX, dirY}) > 0.0f) {
        n = -n;
    }
    return {
        t, t * maxDist,
        originX + r.x * t, originY + r.y * t,
        n.x, n.y,
        kNullEntity, TileMaterialFlags::Empty, ShapeKind2D::Segment, 1U, 0U};
}

SweepHit2D Narrowphase2D::SweepAabbVsSegment(
    f32 centerAX, f32 centerAY, f32 halfAW, f32 halfAH,
    f32 deltaX, f32 deltaY,
    Vec2 p0, Vec2 p1, bool oneSided) noexcept {
    const Vec2 edge = p1 - p0;
    const f32 edgeLenSq = edge.LengthSq();
    if (edgeLenSq <= 1e-8f) {
        return {};
    }

    const f32 invEdgeLen = 1.0f / std::sqrt(edgeLenSq);
    Vec2 n = edge.PerpCCW() * invEdgeLen;
    const Vec2 centerStart{centerAX, centerAY};
    const Vec2 delta{deltaX, deltaY};

    if (!oneSided && (centerStart - p0).Dot(n) < 0.0f) {
        n = -n;
    }

    const f32 supportRadius = halfAW * std::abs(n.x) + halfAH * std::abs(n.y);
    const f32 distStart = (centerStart - p0).Dot(n) - supportRadius;
    const f32 vn = delta.Dot(n);
    if (vn >= -1e-7f) {
        return {};
    }

    constexpr f32 kSkin = 2e-4f;
    f32 t = 0.0f;
    if (distStart > kSkin) {
        t = distStart / (-vn);
        if (t > 1.0f) {
            return {};
        }
    } else if (distStart < -0.25f) {
        return {};
    }

    const Vec2 centerAtHit = centerStart + delta * t;
    const Vec2 supportPt{
        centerAtHit.x - ((n.x >= 0.0f) ? halfAW : -halfAW),
        centerAtHit.y - ((n.y >= 0.0f) ? halfAH : -halfAH)};
    const f32 projU = (supportPt - p0).Dot(edge) / edgeLenSq;
    const f32 uMargin = (halfAW * std::abs(n.y) + halfAH * std::abs(n.x)) * invEdgeLen;
    if (projU < -uMargin || projU > 1.0f + uMargin) {
        return {};
    }

    return {
        std::max(0.0f, t),
        centerAtHit.x, centerAtHit.y,
        n.x, n.y,
        (distStart < 0.0f) ? -distStart : 0.0f,
        kNullEntity, TileMaterialFlags::Empty,
        static_cast<u8>((distStart < -kSkin) ? 1U : 0U),
        1U,
        static_cast<u8>(oneSided ? 1U : 0U)};
}

bool Narrowphase2D::CollideCircleCircle(
    f32 centerAX, f32 centerAY, f32 radiusA,
    f32 centerBX, f32 centerBY, f32 radiusB,
    ContactManifold2D& outManifold, f32 speculativeMargin) noexcept {
    const f32 dx = centerBX - centerAX;
    const f32 dy = centerBY - centerAY;
    const f32 distSq = dx * dx + dy * dy;
    const f32 sumR = radiusA + radiusB;
    const f32 maxReach = sumR + speculativeMargin;
    if (distSq > maxReach * maxReach) {
        return false;
    }

    const f32 dist = std::sqrt(distSq);
    const f32 nx = (dist > 1e-6f) ? (dx / dist) : 0.0f;
    const f32 ny = (dist > 1e-6f) ? (dy / dist) : 1.0f;
    SetSingleContact(outManifold, nx, ny, centerAX + nx * radiusA, centerAY + ny * radiusA, dist - sumR);
    return true;
}

bool Narrowphase2D::CollideCircleAabb(
    f32 circleX, f32 circleY, f32 radius,
    f32 boxCenterX, f32 boxCenterY, f32 halfW, f32 halfH,
    ContactManifold2D& outManifold, f32 speculativeMargin) noexcept {
    const f32 relX = circleX - boxCenterX;
    const f32 relY = circleY - boxCenterY;
    const f32 closestX = std::clamp(relX, -halfW, halfW);
    const f32 closestY = std::clamp(relY, -halfH, halfH);

    if (relX == closestX && relY == closestY) {
        const f32 dxLeft = relX + halfW;
        const f32 dxRight = halfW - relX;
        const f32 dyDown = relY + halfH;
        const f32 dyUp = halfH - relY;
        const f32 minX = std::min(dxLeft, dxRight);
        const f32 minY = std::min(dyDown, dyUp);
        if (minX < minY) {
            const f32 sign = (dxLeft < dxRight) ? 1.0f : -1.0f;
            SetSingleContact(outManifold, sign, 0.0f, boxCenterX - sign * halfW, circleY, -(minX + radius));
        } else {
            const f32 sign = (dyDown < dyUp) ? 1.0f : -1.0f;
            SetSingleContact(outManifold, 0.0f, sign, circleX, boxCenterY - sign * halfH, -(minY + radius));
        }
        return true;
    }

    const f32 worldClosestX = boxCenterX + closestX;
    const f32 worldClosestY = boxCenterY + closestY;
    const f32 dx = worldClosestX - circleX;
    const f32 dy = worldClosestY - circleY;
    const f32 distSq = dx * dx + dy * dy;
    const f32 maxReach = radius + speculativeMargin;
    if (distSq > maxReach * maxReach) {
        return false;
    }

    const f32 dist = std::sqrt(distSq);
    const f32 nx = (dist > 1e-6f) ? (dx / dist) : 0.0f;
    const f32 ny = (dist > 1e-6f) ? (dy / dist) : 1.0f;
    SetSingleContact(outManifold, nx, ny, worldClosestX, worldClosestY, dist - radius);
    return true;
}

bool Narrowphase2D::CollideCircleObb(
    f32 circleX, f32 circleY, f32 radius,
    f32 boxCenterX, f32 boxCenterY, f32 halfW, f32 halfH,
    f32 cosB, f32 sinB,
    ContactManifold2D& outManifold, f32 speculativeMargin) noexcept {
    const f32 relX = circleX - boxCenterX;
    const f32 relY = circleY - boxCenterY;
    const f32 localX = relX * cosB + relY * sinB;
    const f32 localY = -relX * sinB + relY * cosB;

    if (!CollideCircleAabb(localX, localY, radius, 0.0f, 0.0f, halfW, halfH, outManifold, speculativeMargin)) {
        return false;
    }

    const f32 nx = outManifold.normalX;
    const f32 ny = outManifold.normalY;
    const f32 px = outManifold.pointX[0];
    const f32 py = outManifold.pointY[0];
    outManifold.normalX = nx * cosB - ny * sinB;
    outManifold.normalY = nx * sinB + ny * cosB;
    outManifold.pointX[0] = boxCenterX + (px * cosB - py * sinB);
    outManifold.pointY[0] = boxCenterY + (px * sinB + py * cosB);
    return true;
}

bool Narrowphase2D::CollideObbObb(
    f32 centerAX, f32 centerAY, f32 halfAW, f32 halfAH, f32 cosA, f32 sinA,
    f32 centerBX, f32 centerBY, f32 halfBW, f32 halfBH, f32 cosB, f32 sinB,
    ContactManifold2D& outManifold, f32 speculativeMargin) noexcept {
    const Vec2 axesA[2] = {{cosA, sinA}, {-sinA, cosA}};
    const Vec2 axesB[2] = {{cosB, sinB}, {-sinB, cosB}};
    const f32 extA[2] = {halfAW, halfAH};
    const f32 extB[2] = {halfBW, halfBH};
    const Vec2 cA{centerAX, centerAY};
    const Vec2 cB{centerBX, centerBY};
    const Vec2 d = cB - cA;

    f32 maxSep = -1e30f;
    Vec2 bestNormal{1.0f, 0.0f};
    u8 bestOwner = 0U;
    u8 bestAxisIdx = 0U;

    const auto testAxis = [&](Vec2 axis, u8 owner, u8 axisIdx) noexcept -> bool {
        const f32 projD = d.Dot(axis);
        const Vec2 n = (projD < 0.0f) ? -axis : axis;
        const f32 rA = extA[0] * std::abs(n.Dot(axesA[0])) + extA[1] * std::abs(n.Dot(axesA[1]));
        const f32 rB = extB[0] * std::abs(n.Dot(axesB[0])) + extB[1] * std::abs(n.Dot(axesB[1]));
        const f32 sep = std::abs(projD) - (rA + rB);
        if (sep > speculativeMargin) {
            return false;
        }
        if (sep > maxSep) {
            maxSep = sep;
            bestNormal = n;
            bestOwner = owner;
            bestAxisIdx = axisIdx;
        }
        return true;
    };

    if (!testAxis(axesA[0], 0U, 0U) || !testAxis(axesA[1], 0U, 1U) ||
        !testAxis(axesB[0], 1U, 0U) || !testAxis(axesB[1], 1U, 1U)) {
        return false;
    }

    const Vec2 refCenter = (bestOwner == 0U) ? cA : cB;
    const Vec2 incCenter = (bestOwner == 0U) ? cB : cA;
    const Vec2* refAxes = (bestOwner == 0U) ? axesA : axesB;
    const Vec2* incAxes = (bestOwner == 0U) ? axesB : axesA;
    const f32* refExt = (bestOwner == 0U) ? extA : extB;
    const f32* incExt = (bestOwner == 0U) ? extB : extA;

    const Vec2 refNormal = (bestOwner == 0U) ? bestNormal : -bestNormal;
    const u8 sideIdx = static_cast<u8>(1U - bestAxisIdx);
    const Vec2 sideAxis = refAxes[sideIdx];
    const f32 refFrontOffset = refCenter.Dot(refNormal) + refExt[bestAxisIdx];
    const f32 sideExtent = refExt[sideIdx];
    const f32 refSideCenter = refCenter.Dot(sideAxis);

    const u8 incAxisIdx = (std::abs(incAxes[0].Dot(refNormal)) > std::abs(incAxes[1].Dot(refNormal))) ? 0U : 1U;
    const u8 incSideIdx = static_cast<u8>(1U - incAxisIdx);
    const f32 incSign = (incAxes[incAxisIdx].Dot(refNormal) > 0.0f) ? -1.0f : 1.0f;

    const Vec2 incFaceCenter = incCenter + incAxes[incAxisIdx] * (incSign * incExt[incAxisIdx]);
    const Vec2 incHalfEdge = incAxes[incSideIdx] * incExt[incSideIdx];
    Vec2 clipPts[2] = {incFaceCenter - incHalfEdge, incFaceCenter + incHalfEdge};

    const auto clipSegmentToPlane = [](Vec2 inPts[2], Vec2 planeNormal, f32 maxPlaneDist) noexcept -> u8 {
        Vec2 outPts[2]{};
        u8 count = 0U;
        const f32 d0 = inPts[0].Dot(planeNormal) - maxPlaneDist;
        const f32 d1 = inPts[1].Dot(planeNormal) - maxPlaneDist;
        if (d0 <= 0.0f) outPts[count++] = inPts[0];
        if (d1 <= 0.0f) outPts[count++] = inPts[1];
        if (d0 * d1 < 0.0f && count < 2U) {
            outPts[count++] = inPts[0] + (inPts[1] - inPts[0]) * (d0 / (d0 - d1));
        }
        inPts[0] = outPts[0];
        if (count > 1U) inPts[1] = outPts[1];
        return count;
    };

    if (clipSegmentToPlane(clipPts,  sideAxis,  refSideCenter + sideExtent) < 2U ||
        clipSegmentToPlane(clipPts, -sideAxis, -refSideCenter + sideExtent) < 1U) {
        SetSingleContact(outManifold, bestNormal.x, bestNormal.y, 0.5f * (centerAX + centerBX), 0.5f * (centerAY + centerBY), maxSep);
        return true;
    }

    outManifold.normalX = bestNormal.x;
    outManifold.normalY = bestNormal.y;
    u8 validCount = 0U;
    for (u8 i = 0U; i < 2U; ++i) {
        const f32 sep = clipPts[i].Dot(refNormal) - refFrontOffset;
        if (sep <= speculativeMargin) {
            outManifold.pointX[validCount] = clipPts[i].x;
            outManifold.pointY[validCount] = clipPts[i].y;
            outManifold.separation[validCount] = sep;
            ++validCount;
        }
    }

    if (validCount == 0U) {
        SetSingleContact(outManifold, bestNormal.x, bestNormal.y, clipPts[0].x, clipPts[0].y, maxSep);
    } else {
        outManifold.pointCount = validCount;
    }
    return true;
}

bool Narrowphase2D::CollideShapes(
    EntityId entityA, const Collider2D& colA, const WorldTransform2D& xfA,
    EntityId entityB, const Collider2D& colB, const WorldTransform2D& xfB,
    ContactManifold2D& outManifold, f32 speculativeMargin) noexcept {
    if (!colA.filter.ShouldCollide(colB.filter)) {
        return false;
    }

    if (static_cast<u8>(colA.shape) > static_cast<u8>(colB.shape)) {
        if (!CollideShapes(entityB, colB, xfB, entityA, colA, xfA, outManifold, speculativeMargin)) {
            return false;
        }
        outManifold.entityA = entityA;
        outManifold.entityB = entityB;
        outManifold.normalX = -outManifold.normalX;
        outManifold.normalY = -outManifold.normalY;
        return true;
    }

    const Vec2 centerA = xfA.TransformPoint({colA.localBounds.OffsetX(), colA.localBounds.OffsetY()});
    const f32 hAW = colA.localBounds.HalfW() * std::abs(xfA.scaleX);
    const f32 hAH = colA.localBounds.HalfH() * std::abs(xfA.scaleY);

    const Vec2 centerB = xfB.TransformPoint({colB.localBounds.OffsetX(), colB.localBounds.OffsetY()});
    const f32 hBW = colB.localBounds.HalfW() * std::abs(xfB.scaleX);
    const f32 hBH = colB.localBounds.HalfH() * std::abs(xfB.scaleY);

    outManifold = {
        entityA, entityB, 0.0f, 0.0f, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f},
        CombineFriction(colA.friction_q16, colB.friction_q16),
        CombineRestitution(colA.restitution_q16, colB.restitution_q16),
        0U, static_cast<u8>((colA.IsSensor() || colB.IsSensor()) ? 1U : 0U), 0U};

    const ShapeKind2D sA = colA.shape;
    const ShapeKind2D sB = colB.shape;

    const auto flipNormal = [&](bool hit) noexcept -> bool {
        if (hit) {
            outManifold.normalX = -outManifold.normalX;
            outManifold.normalY = -outManifold.normalY;
        }
        return hit;
    };

    if (sA == ShapeKind2D::Aabb) {
        if (sB == ShapeKind2D::Aabb) {
            return CollideAabbAabb(centerA.x, centerA.y, hAW, hAH, centerB.x, centerB.y, hBW, hBH, outManifold, speculativeMargin);
        }
        if (sB == ShapeKind2D::Circle) {
            return flipNormal(CollideCircleAabb(centerB.x, centerB.y, hBW, centerA.x, centerA.y, hAW, hAH, outManifold, speculativeMargin));
        }
        if (sB == ShapeKind2D::CapsuleY) {
            Vec2 pB0{}, pB1{};
            ExtractCapsuleWorldSegment(centerB, hBW, hBH, xfB, pB0, pB1);
            const Vec2 closestB = ClosestPointOnSegment(ClosestPointOnBox(centerB, centerA, hAW, hAH, 1.0f, 0.0f), pB0, pB1);
            return flipNormal(CollideCircleAabb(closestB.x, closestB.y, hBW, centerA.x, centerA.y, hAW, hAH, outManifold, speculativeMargin));
        }
        if (sB == ShapeKind2D::Obb) {
            return CollideObbObb(
                centerA.x, centerA.y, hAW, hAH, 1.0f, 0.0f,
                centerB.x, centerB.y, hBW, hBH, xfB.cosRot, xfB.sinRot,
                outManifold, speculativeMargin);
        }
        return CollideBoxVsSegment(
            centerA, hAW, hAH, 1.0f, 0.0f,
            xfB.TransformPoint(colB.localBounds.SegmentP0()),
            xfB.TransformPoint(colB.localBounds.SegmentP1()),
            outManifold, speculativeMargin);
    }

    if (sA == ShapeKind2D::Circle) {
        if (sB == ShapeKind2D::Circle) {
            return CollideCircleCircle(centerA.x, centerA.y, hAW, centerB.x, centerB.y, hBW, outManifold, speculativeMargin);
        }
        if (sB == ShapeKind2D::CapsuleY) {
            Vec2 pB0{}, pB1{};
            ExtractCapsuleWorldSegment(centerB, hBW, hBH, xfB, pB0, pB1);
            const Vec2 closestB = ClosestPointOnSegment(centerA, pB0, pB1);
            return CollideCircleCircle(centerA.x, centerA.y, hAW, closestB.x, closestB.y, hBW, outManifold, speculativeMargin);
        }
        if (sB == ShapeKind2D::Obb) {
            return CollideCircleObb(
                centerA.x, centerA.y, hAW, centerB.x, centerB.y, hBW, hBH, xfB.cosRot, xfB.sinRot, outManifold, speculativeMargin);
        }
        const Vec2 closest = ClosestPointOnSegment(
            centerA, xfB.TransformPoint(colB.localBounds.SegmentP0()), xfB.TransformPoint(colB.localBounds.SegmentP1()));
        return CollideCircleCircle(centerA.x, centerA.y, hAW, closest.x, closest.y, 0.0f, outManifold, speculativeMargin);
    }

    if (sA == ShapeKind2D::CapsuleY) {
        Vec2 pA0{}, pA1{};
        ExtractCapsuleWorldSegment(centerA, hAW, hAH, xfA, pA0, pA1);
        if (sB == ShapeKind2D::CapsuleY) {
            Vec2 pB0{}, pB1{}, cA{}, cB{};
            ExtractCapsuleWorldSegment(centerB, hBW, hBH, xfB, pB0, pB1);
            ClosestPointsSegmentSegment(pA0, pA1, pB0, pB1, cA, cB);
            return CollideCircleCircle(cA.x, cA.y, hAW, cB.x, cB.y, hBW, outManifold, speculativeMargin);
        }
        if (sB == ShapeKind2D::Obb) {
            const Vec2 closestA = ClosestPointOnSegment(ClosestPointOnBox(centerA, centerB, hBW, hBH, xfB.cosRot, xfB.sinRot), pA0, pA1);
            return CollideCircleObb(
                closestA.x, closestA.y, hAW, centerB.x, centerB.y, hBW, hBH, xfB.cosRot, xfB.sinRot, outManifold, speculativeMargin);
        }
        Vec2 cA{}, cB{};
        ClosestPointsSegmentSegment(pA0, pA1, xfB.TransformPoint(colB.localBounds.SegmentP0()), xfB.TransformPoint(colB.localBounds.SegmentP1()), cA, cB);
        return CollideCircleCircle(cA.x, cA.y, hAW, cB.x, cB.y, 0.0f, outManifold, speculativeMargin);
    }

    if (sA == ShapeKind2D::Obb) {
        if (sB == ShapeKind2D::Obb) {
            return CollideObbObb(
                centerA.x, centerA.y, hAW, hAH, xfA.cosRot, xfA.sinRot,
                centerB.x, centerB.y, hBW, hBH, xfB.cosRot, xfB.sinRot,
                outManifold, speculativeMargin);
        }
        return CollideBoxVsSegment(
            centerA, hAW, hAH, xfA.cosRot, xfA.sinRot,
            xfB.TransformPoint(colB.localBounds.SegmentP0()),
            xfB.TransformPoint(colB.localBounds.SegmentP1()),
            outManifold, speculativeMargin);
    }

    Vec2 cA{}, cB{};
    ClosestPointsSegmentSegment(
        xfA.TransformPoint(colA.localBounds.SegmentP0()), xfA.TransformPoint(colA.localBounds.SegmentP1()),
        xfB.TransformPoint(colB.localBounds.SegmentP0()), xfB.TransformPoint(colB.localBounds.SegmentP1()),
        cA, cB);
    return CollideCircleCircle(cA.x, cA.y, 1e-3f, cB.x, cB.y, 1e-3f, outManifold, speculativeMargin);
}

} // namespace en
