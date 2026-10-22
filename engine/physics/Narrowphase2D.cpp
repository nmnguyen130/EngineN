#include "physics/Narrowphase2D.h"

namespace en {

RayHit2D Narrowphase2D::RaycastCircle(
    f32 originX,
    f32 originY,
    f32 dirX,
    f32 dirY,
    f32 maxDist,
    f32 centerX,
    f32 centerY,
    f32 radius) noexcept {
    RayHit2D hit{};
    const f32 mx = originX - centerX;
    const f32 my = originY - centerY;
    const f32 c = (mx * mx + my * my) - radius * radius;
    if (c <= 0.0f) {
        hit.fraction = 0.0f;
        hit.distance = 0.0f;
        hit.pointX = originX;
        hit.pointY = originY;
        hit.normalX = -dirX;
        hit.normalY = -dirY;
        hit.shapeKind = ShapeKind2D::Circle;
        hit.hitValid = 1U;
        hit.startSolid = 1U;
        return hit;
    }

    const f32 b = mx * dirX + my * dirY;
    if (b > 0.0f) return hit;

    const f32 disc = b * b - c;
    if (disc < 0.0f) return hit;

    const f32 t = -b - std::sqrt(disc);
    if (t < 0.0f || t > maxDist) return hit;

    hit.distance = t;
    hit.fraction = (maxDist > 1e-8f) ? (t / maxDist) : 0.0f;
    hit.pointX = originX + dirX * t;
    hit.pointY = originY + dirY * t;
    const f32 invR = (radius > 1e-6f) ? (1.0f / radius) : 1.0f;
    hit.normalX = (hit.pointX - centerX) * invR;
    hit.normalY = (hit.pointY - centerY) * invR;
    hit.shapeKind = ShapeKind2D::Circle;
    hit.hitValid = 1U;
    return hit;
}

RayHit2D Narrowphase2D::RaycastSegment(
    f32 originX,
    f32 originY,
    f32 dirX,
    f32 dirY,
    f32 maxDist,
    Vec2 p0,
    Vec2 p1) noexcept {
    RayHit2D hit{};
    const Vec2 orig{originX, originY};
    const Vec2 r{dirX * maxDist, dirY * maxDist};
    const Vec2 s = p1 - p0;
    const f32 rCrossS = r.Cross(s);
    if (std::abs(rCrossS) <= 1e-8f) return hit;

    const Vec2 qp = p0 - orig;
    const f32 t = qp.Cross(s) / rCrossS;
    const f32 u = qp.Cross(r) / rCrossS;
    if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) return hit;

    Vec2 n = s.PerpCCW().Normalized();
    if (n.Dot({dirX, dirY}) > 0.0f) {
        n = -n;
    }

    hit.fraction = t;
    hit.distance = t * maxDist;
    hit.pointX = originX + r.x * t;
    hit.pointY = originY + r.y * t;
    hit.normalX = n.x;
    hit.normalY = n.y;
    hit.shapeKind = ShapeKind2D::Segment;
    hit.hitValid = 1U;
    return hit;
}

SweepHit2D Narrowphase2D::SweepAabbVsSegment(
    f32 centerAX,
    f32 centerAY,
    f32 halfAW,
    f32 halfAH,
    f32 deltaX,
    f32 deltaY,
    Vec2 p0,
    Vec2 p1,
    bool oneSided) noexcept {
    SweepHit2D out{};
    const Vec2 edge = p1 - p0;
    const f32 edgeLenSq = edge.LengthSq();
    if (edgeLenSq <= 1e-8f) return out;

    // Outward normal pointing CCW from p0 -> p1
    Vec2 n = edge.PerpCCW() * (1.0f / std::sqrt(edgeLenSq));
    const Vec2 centerStart{centerAX, centerAY};
    const Vec2 delta{deltaX, deltaY};

    // Flip normal toward the approaching AABB unless the segment is strictly one-sided
    if (!oneSided && (centerStart - p0).Dot(n) < 0.0f) {
        n = -n;
    }

    // Support distance of the AABB along normal n
    const f32 supportRadius = halfAW * std::abs(n.x) + halfAH * std::abs(n.y);
    const f32 distStart = (centerStart - p0).Dot(n) - supportRadius;
    const f32 vn = delta.Dot(n);

    // Reject if moving away from the plane
    if (vn >= -1e-7f) return out;

    constexpr f32 kSkin = 2e-4f;
    f32 t = 0.0f;
    if (distStart > kSkin) {
        t = distStart / (-vn);
        if (t > 1.0f) return out;
    } else if (distStart < -0.25f) {
        // Already well behind the segment
        return out;
    }

    const Vec2 centerAtHit = centerStart + delta * t;
    // Project support point onto the segment to verify we hit between [p0, p1]
    const Vec2 supportPt{
        centerAtHit.x - ((n.x >= 0.0f) ? halfAW : -halfAW),
        centerAtHit.y - ((n.y >= 0.0f) ? halfAH : -halfAH)};
    const f32 projU = (supportPt - p0).Dot(edge) / edgeLenSq;
    // Allow a small margin around [0, 1] scaled by box width so the box footprint smoothly rides the segment
    const f32 uMargin = halfAW / std::sqrt(edgeLenSq);
    if (projU < -uMargin || projU > 1.0f + uMargin) {
        return out;
    }

    out.toi = std::max(0.0f, t);
    out.pointX = centerAtHit.x;
    out.pointY = centerAtHit.y;
    out.normalX = n.x;
    out.normalY = n.y;
    out.penetration = (distStart < 0.0f) ? -distStart : 0.0f;
    out.startPenetrating = (distStart < -kSkin) ? 1U : 0U;
    out.hitValid = 1U;
    out.isOneWay = oneSided ? 1U : 0U;
    return out;
}

bool Narrowphase2D::CollideCircleCircle(
    f32 centerAX,
    f32 centerAY,
    f32 radiusA,
    f32 centerBX,
    f32 centerBY,
    f32 radiusB,
    ContactManifold2D& outManifold,
    f32 speculativeMargin) noexcept {
    const f32 dx = centerBX - centerAX;
    const f32 dy = centerBY - centerAY;
    const f32 distSq = dx * dx + dy * dy;
    const f32 sumR = radiusA + radiusB;
    const f32 maxReach = sumR + speculativeMargin;
    if (distSq > maxReach * maxReach) return false;

    const f32 dist = std::sqrt(distSq);
    if (dist > 1e-6f) {
        const f32 invDist = 1.0f / dist;
        outManifold.normalX = dx * invDist;
        outManifold.normalY = dy * invDist;
    } else {
        outManifold.normalX = 0.0f;
        outManifold.normalY = 1.0f;
    }

    outManifold.pointCount = 1U;
    outManifold.pointX[0] = centerAX + outManifold.normalX * radiusA;
    outManifold.pointY[0] = centerAY + outManifold.normalY * radiusA;
    outManifold.separation[0] = dist - sumR;
    return true;
}

bool Narrowphase2D::CollideCircleAabb(
    f32 circleX,
    f32 circleY,
    f32 radius,
    f32 boxCenterX,
    f32 boxCenterY,
    f32 halfW,
    f32 halfH,
    ContactManifold2D& outManifold,
    f32 speculativeMargin) noexcept {
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
        outManifold.pointCount = 1U;
        if (minX < minY) {
            outManifold.normalX = (dxLeft < dxRight) ? 1.0f : -1.0f;
            outManifold.normalY = 0.0f;
            outManifold.pointX[0] = boxCenterX + ((dxLeft < dxRight) ? -halfW : halfW);
            outManifold.pointY[0] = circleY;
            outManifold.separation[0] = -(minX + radius);
        } else {
            outManifold.normalX = 0.0f;
            outManifold.normalY = (dyDown < dyUp) ? 1.0f : -1.0f;
            outManifold.pointX[0] = circleX;
            outManifold.pointY[0] = boxCenterY + ((dyDown < dyUp) ? -halfH : halfH);
            outManifold.separation[0] = -(minY + radius);
        }
        return true;
    }

    const f32 dx = (boxCenterX + closestX) - circleX;
    const f32 dy = (boxCenterY + closestY) - circleY;
    const f32 distSq = dx * dx + dy * dy;
    const f32 maxReach = radius + speculativeMargin;
    if (distSq > maxReach * maxReach) return false;

    const f32 dist = std::sqrt(distSq);
    if (dist > 1e-6f) {
        const f32 invDist = 1.0f / dist;
        outManifold.normalX = dx * invDist;
        outManifold.normalY = dy * invDist;
    } else {
        outManifold.normalX = 0.0f;
        outManifold.normalY = 1.0f;
    }
    outManifold.pointCount = 1U;
    outManifold.pointX[0] = boxCenterX + closestX;
    outManifold.pointY[0] = boxCenterY + closestY;
    outManifold.separation[0] = dist - radius;
    return true;
}

bool Narrowphase2D::CollideCircleSegment(
    f32 circleX,
    f32 circleY,
    f32 radius,
    Vec2 p0,
    Vec2 p1,
    ContactManifold2D& outManifold,
    f32 speculativeMargin) noexcept {
    const Vec2 edge = p1 - p0;
    const f32 lenSq = edge.LengthSq();
    const f32 u = (lenSq > 1e-8f)
                      ? std::clamp((Vec2{circleX, circleY} - p0).Dot(edge) / lenSq, 0.0f, 1.0f)
                      : 0.0f;
    const Vec2 closest = p0 + edge * u;
    return CollideCircleCircle(
        circleX, circleY, radius, closest.x, closest.y, 0.0f, outManifold, speculativeMargin);
}

bool Narrowphase2D::CollideAabbSegment(
    f32 boxCenterX,
    f32 boxCenterY,
    f32 halfW,
    f32 halfH,
    Vec2 p0,
    Vec2 p1,
    ContactManifold2D& outManifold,
    f32 speculativeMargin) noexcept {
    const Vec2 edge = p1 - p0;
    const f32 len = edge.Length();
    if (len <= 1e-6f) {
        return CollideCircleAabb(p0.x, p0.y, 0.0f, boxCenterX, boxCenterY, halfW, halfH, outManifold, speculativeMargin);
    }
    const f32 cosB = edge.x / len;
    const f32 sinB = edge.y / len;
    const Vec2 mid = 0.5f * (p0 + p1);
    return CollideObbObb(
        boxCenterX, boxCenterY, halfW, halfH, 1.0f, 0.0f,
        mid.x, mid.y, 0.5f * len, 1e-3f, cosB, sinB,
        outManifold, speculativeMargin);
}

bool Narrowphase2D::CollideObbObb(
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
    f32 speculativeMargin) noexcept {
    const Vec2 axesA[2] = {{cosA, sinA}, {-sinA, cosA}};
    const Vec2 axesB[2] = {{cosB, sinB}, {-sinB, cosB}};
    const f32 extA[2] = {halfAW, halfAH};
    const f32 extB[2] = {halfBW, halfBH};
    const Vec2 cA{centerAX, centerAY};
    const Vec2 cB{centerBX, centerBY};
    const Vec2 d = cB - cA;

    f32 maxSep = -1e30f;
    Vec2 bestNormal{1.0f, 0.0f};
    u8 bestOwner = 0U; // 0: box A face, 1: box B face
    u8 bestAxisIdx = 0U;

    const auto testAxis = [&](Vec2 axis, u8 owner, u8 axisIdx) noexcept -> bool {
        const f32 projD = d.Dot(axis);
        const Vec2 n = (projD < 0.0f) ? -axis : axis;
        const f32 dist = std::abs(projD);
        const f32 rA = extA[0] * std::abs(n.Dot(axesA[0])) + extA[1] * std::abs(n.Dot(axesA[1]));
        const f32 rB = extB[0] * std::abs(n.Dot(axesB[0])) + extB[1] * std::abs(n.Dot(axesB[1]));
        const f32 sep = dist - (rA + rB);
        if (sep > speculativeMargin) return false;
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

    // Sutherland-Hodgman 2-point reference/incident face clipping
    const Vec2 refCenter = (bestOwner == 0U) ? cA : cB;
    const Vec2 incCenter = (bestOwner == 0U) ? cB : cA;
    const Vec2* refAxes = (bestOwner == 0U) ? axesA : axesB;
    const Vec2* incAxes = (bestOwner == 0U) ? axesB : axesA;
    const f32* refExt = (bestOwner == 0U) ? extA : extB;
    const f32* incExt = (bestOwner == 0U) ? extB : extA;

    // Outward normal of the reference face pointing toward the incident box
    const Vec2 refNormal = (bestOwner == 0U) ? bestNormal : -bestNormal;
    const u8 sideIdx = static_cast<u8>(1U - bestAxisIdx);
    const Vec2 sideAxis = refAxes[sideIdx];
    const f32 refFrontOffset = refCenter.Dot(refNormal) + refExt[bestAxisIdx];
    const f32 sideExtent = refExt[sideIdx];
    const f32 refSideCenter = refCenter.Dot(sideAxis);

    // Find the incident face most anti-parallel to refNormal
    const f32 dot0 = incAxes[0].Dot(refNormal);
    const f32 dot1 = incAxes[1].Dot(refNormal);
    const u8 incAxisIdx = (std::abs(dot0) > std::abs(dot1)) ? 0U : 1U;
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
            const f32 t = d0 / (d0 - d1);
            outPts[count++] = inPts[0] + (inPts[1] - inPts[0]) * t;
        }
        inPts[0] = outPts[0];
        if (count > 1U) inPts[1] = outPts[1];
        return count;
    };

    if (clipSegmentToPlane(clipPts,  sideAxis,  refSideCenter + sideExtent) < 2U ||
        clipSegmentToPlane(clipPts, -sideAxis, -refSideCenter + sideExtent) < 1U) {
        outManifold.normalX = bestNormal.x;
        outManifold.normalY = bestNormal.y;
        outManifold.pointCount = 1U;
        outManifold.pointX[0] = 0.5f * (centerAX + centerBX);
        outManifold.pointY[0] = 0.5f * (centerAY + centerBY);
        outManifold.separation[0] = maxSep;
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
        outManifold.pointCount = 1U;
        outManifold.pointX[0] = clipPts[0].x;
        outManifold.pointY[0] = clipPts[0].y;
        outManifold.separation[0] = maxSep;
    } else {
        outManifold.pointCount = validCount;
    }
    return true;
}

bool Narrowphase2D::CollideShapes(
    EntityId entityA,
    const Collider2D& colA,
    const WorldTransform2D& xfA,
    EntityId entityB,
    const Collider2D& colB,
    const WorldTransform2D& xfB,
    ContactManifold2D& outManifold,
    f32 speculativeMargin) noexcept {
    if (!colA.filter.ShouldCollide(colB.filter)) return false;

    const Vec2 centerA = xfA.TransformPoint({colA.localBounds.OffsetX(), colA.localBounds.OffsetY()});
    const f32 hAW = colA.localBounds.HalfW() * std::abs(xfA.scaleX);
    const f32 hAH = colA.localBounds.HalfH() * std::abs(xfA.scaleY);

    const Vec2 centerB = xfB.TransformPoint({colB.localBounds.OffsetX(), colB.localBounds.OffsetY()});
    const f32 hBW = colB.localBounds.HalfW() * std::abs(xfB.scaleX);
    const f32 hBH = colB.localBounds.HalfH() * std::abs(xfB.scaleY);

    outManifold = ContactManifold2D{};
    outManifold.entityA = entityA;
    outManifold.entityB = entityB;
    outManifold.friction_q16 = CombineFriction(colA.friction_q16, colB.friction_q16);
    outManifold.restitution_q16 = CombineRestitution(colA.restitution_q16, colB.restitution_q16);
    outManifold.isSensor = (colA.IsSensor() || colB.IsSensor()) ? 1U : 0U;

    const ShapeKind2D sA = colA.shape;
    const ShapeKind2D sB = colB.shape;

    if (sA == ShapeKind2D::Aabb && sB == ShapeKind2D::Aabb) {
        return CollideAabbAabb(centerA.x, centerA.y, hAW, hAH, centerB.x, centerB.y, hBW, hBH, outManifold, speculativeMargin);
    }
    if (sA == ShapeKind2D::Circle && sB == ShapeKind2D::Circle) {
        return CollideCircleCircle(centerA.x, centerA.y, hAW, centerB.x, centerB.y, hBW, outManifold, speculativeMargin);
    }
    if (sA == ShapeKind2D::Circle && sB == ShapeKind2D::Aabb) {
        return CollideCircleAabb(centerA.x, centerA.y, hAW, centerB.x, centerB.y, hBW, hBH, outManifold, speculativeMargin);
    }
    if (sA == ShapeKind2D::Aabb && sB == ShapeKind2D::Circle) {
        if (!CollideCircleAabb(centerB.x, centerB.y, hBW, centerA.x, centerA.y, hAW, hAH, outManifold, speculativeMargin)) {
            return false;
        }
        outManifold.normalX = -outManifold.normalX;
        outManifold.normalY = -outManifold.normalY;
        return true;
    }
    if (sA == ShapeKind2D::Circle && sB == ShapeKind2D::Segment) {
        const Vec2 p0 = xfB.TransformPoint(colB.localBounds.SegmentP0());
        const Vec2 p1 = xfB.TransformPoint(colB.localBounds.SegmentP1());
        return CollideCircleSegment(centerA.x, centerA.y, hAW, p0, p1, outManifold, speculativeMargin);
    }
    if (sA == ShapeKind2D::Segment && sB == ShapeKind2D::Circle) {
        const Vec2 p0 = xfA.TransformPoint(colA.localBounds.SegmentP0());
        const Vec2 p1 = xfA.TransformPoint(colA.localBounds.SegmentP1());
        if (!CollideCircleSegment(centerB.x, centerB.y, hBW, p0, p1, outManifold, speculativeMargin)) {
            return false;
        }
        outManifold.normalX = -outManifold.normalX;
        outManifold.normalY = -outManifold.normalY;
        return true;
    }
    if (sA == ShapeKind2D::Aabb && sB == ShapeKind2D::Segment) {
        const Vec2 p0 = xfB.TransformPoint(colB.localBounds.SegmentP0());
        const Vec2 p1 = xfB.TransformPoint(colB.localBounds.SegmentP1());
        return CollideAabbSegment(centerA.x, centerA.y, hAW, hAH, p0, p1, outManifold, speculativeMargin);
    }
    if (sA == ShapeKind2D::Segment && sB == ShapeKind2D::Aabb) {
        const Vec2 p0 = xfA.TransformPoint(colA.localBounds.SegmentP0());
        const Vec2 p1 = xfA.TransformPoint(colA.localBounds.SegmentP1());
        if (!CollideAabbSegment(centerB.x, centerB.y, hBW, hBH, p0, p1, outManifold, speculativeMargin)) {
            return false;
        }
        outManifold.normalX = -outManifold.normalX;
        outManifold.normalY = -outManifold.normalY;
        return true;
    }
    if (sA == ShapeKind2D::CapsuleY && sB == ShapeKind2D::Aabb) {
        const f32 radius = hAW;
        const f32 segHalf = std::max(0.0f, hAH - radius);
        const f32 clampedSegY = std::clamp(centerB.y, centerA.y - segHalf, centerA.y + segHalf);
        return CollideCircleAabb(centerA.x, clampedSegY, radius, centerB.x, centerB.y, hBW, hBH, outManifold, speculativeMargin);
    }
    if (sA == ShapeKind2D::Aabb && sB == ShapeKind2D::CapsuleY) {
        const f32 radius = hBW;
        const f32 segHalf = std::max(0.0f, hBH - radius);
        const f32 clampedSegY = std::clamp(centerA.y, centerB.y - segHalf, centerB.y + segHalf);
        if (!CollideCircleAabb(centerB.x, clampedSegY, radius, centerA.x, centerA.y, hAW, hAH, outManifold, speculativeMargin)) {
            return false;
        }
        outManifold.normalX = -outManifold.normalX;
        outManifold.normalY = -outManifold.normalY;
        return true;
    }

    return CollideObbObb(
        centerA.x, centerA.y, hAW, hAH, xfA.cosRot, xfA.sinRot,
        centerB.x, centerB.y, hBW, hBH, xfB.cosRot, xfB.sinRot,
        outManifold, speculativeMargin);
}

} // namespace en
