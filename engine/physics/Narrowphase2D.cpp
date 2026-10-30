#include "physics/Narrowphase2D.h"

namespace en {

RayHit2D Narrowphase2D::RaycastCircle(
    f32 ox, f32 oy, f32 dx, f32 dy, f32 maxDist,
    f32 cx, f32 cy, f32 r) noexcept {
    const f32 fx = ox - cx, fy = oy - cy;
    const f32 a = dx * dx + dy * dy;
    if (a < 1e-12f) {
        const f32 d2 = fx * fx + fy * fy;
        if (d2 <= r * r) {
            return {0.0f, 0.0f, ox, oy, -fx, -fy, kNullEntity,
                    TileMaterialFlags::Empty, ShapeKind2D::Circle, 1U, 1U};
        }
        return {};
    }
    const f32 b = 2.0f * (fx * dx + fy * dy);
    const f32 c = fx * fx + fy * fy - r * r;
    const f32 disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return {};
    const f32 sqrtD = std::sqrt(disc);
    const f32 inv2a = 0.5f / a;
    f32 t = (-b - sqrtD) * inv2a;
    u8 startSolid = 0U;
    if (t < 0.0f) {
        t = (-b + sqrtD) * inv2a;
        if (t < 0.0f || c > 0.0f) return {};
        startSolid = 1U;
        t = 0.0f;
    }
    if (t > maxDist) return {};
    const f32 px = ox + dx * t, py = oy + dy * t;
    f32 nx = px - cx, ny = py - cy;
    const f32 len = std::sqrt(nx * nx + ny * ny);
       if (len > 1e-8f) { nx /= len; ny /= len; }
    else { nx = 0.0f; ny = 1.0f; }
    return {(maxDist > 1e-8f) ? t / maxDist : 0.0f, t, px, py, nx, ny,
            kNullEntity, TileMaterialFlags::Empty, ShapeKind2D::Circle, 1U, startSolid};
}

RayHit2D Narrowphase2D::RaycastSegment(
    f32 ox, f32 oy, f32 dx, f32 dy, f32 maxDist,
    Vec2 p0, Vec2 p1) noexcept {
    const f32 sx = p1.x - p0.x, sy = p1.y - p0.y;
    const f32 denom = dx * sy - dy * sx;
    if (std::abs(denom) < 1e-10f) return {};
    const f32 fx = p0.x - ox, fy = p0.y - oy;
    const f32 t = (fx * sy - fy * sx) / denom;
    const f32 u = (fx * dy - fy * dx) / denom;
    if (t < 0.0f || t > maxDist || u < 0.0f || u > 1.0f) return {};
    f32 nx = -sy, ny = sx;
    const f32 len = std::sqrt(nx * nx + ny * ny);
    if (len > 1e-8f) { nx /= len; ny /= len; }
    if (nx * dx + ny * dy > 0.0f) { nx = -nx; ny = -ny; }
    return {(maxDist > 1e-8f) ? t / maxDist : 0.0f, t,
            ox + dx * t, oy + dy * t, nx, ny,
            kNullEntity, TileMaterialFlags::Empty, ShapeKind2D::Segment, 1U, 0U};
}

SweepHit2D Narrowphase2D::SweepAabbVsSegment(
    f32 cx, f32 cy, f32 hw, f32 hh,
    f32 dx, f32 dy,
    Vec2 p0, Vec2 p1, bool oneWay) noexcept {
    const f32 minX = std::min(p0.x, p1.x) - hw;
    const f32 maxX = std::max(p0.x, p1.x) + hw;
    const f32 minY = std::min(p0.y, p1.y) - hh;
    const f32 maxY = std::max(p0.y, p1.y) + hh;
    SweepHit2D h = SweepAabbVsAabb(cx, cy, 0.0f, 0.0f, dx, dy, minX, minY, maxX, maxY);
    if (!h.Hit()) return {};
    if (oneWay && (dy > 0.0f || h.normalY < 0.5f)) return {};
    h.isOneWay = oneWay ? 1U : 0U;
    return h;
}

bool Narrowphase2D::CollideCircleAabb(
    f32 cx, f32 cy, f32 r,
    f32 bx, f32 by, f32 bhw, f32 bhh,
    ContactManifold2D& m, f32 margin) noexcept {
    const f32 closestX = std::clamp(cx, bx - bhw, bx + bhw);
    const f32 closestY = std::clamp(cy, by - bhh, by + bhh);
    f32 nx = cx - closestX, ny = cy - closestY;
    const f32 d2 = nx * nx + ny * ny;
    const f32 rEff = r + margin;
    if (d2 > rEff * rEff && d2 > 1e-12f) return false;
    m.pointCount = 1U;
    if (d2 < 1e-12f) {
        const f32 dl = cx - (bx - bhw), dr = (bx + bhw) - cx;
        const f32 dd = cy - (by - bhh), du = (by + bhh) - cy;
        const f32 px = std::min(dl, dr), py = std::min(dd, du);
        if (px < py) {
            m.normalX = (dl < dr) ? -1.0f : 1.0f;
            m.normalY = 0.0f;
            m.separation[0] = -(px + r);
        } else {
            m.normalX = 0.0f;
            m.normalY = (dd < du) ? -1.0f : 1.0f;
            m.separation[0] = -(py + r);
        }
        m.pointX[0] = cx - m.normalX * r;
        m.pointY[0] = cy - m.normalY * r;
        return true;
    }
    const f32 d = std::sqrt(d2);
    m.normalX = nx / d;
    m.normalY = ny / d;
    m.separation[0] = d - rEff;
    m.pointX[0] = closestX;
    m.pointY[0] = closestY;
    return true;
}

bool Narrowphase2D::CollideCircleCircle(
    f32 ax, f32 ay, f32 ar,
    f32 bx, f32 by, f32 br,
    ContactManifold2D& m, f32 margin) noexcept {
    f32 nx = bx - ax, ny = by - ay;
    const f32 d2 = nx * nx + ny * ny;
    const f32 r = ar + br + margin;
    if (d2 > r * r) return false;
    m.pointCount = 1U;
    if (d2 < 1e-12f) {
        m.normalX = 0.0f;
        m.normalY = 1.0f;
        m.separation[0] = -r;
        m.pointX[0] = ax;
        m.pointY[0] = ay;
        return true;
    }
    const f32 d = std::sqrt(d2);
    m.normalX = nx / d;
    m.normalY = ny / d;
    m.separation[0] = d - r;
    m.pointX[0] = ax + m.normalX * ar;
    m.pointY[0] = ay + m.normalY * ar;
    return true;
}

bool Narrowphase2D::CollideObbObb(
    f32 ax, f32 ay, f32 ahw, f32 ahh, f32 ac, f32 as,
    f32 bx, f32 by, f32 bhw, f32 bhh, f32 bc, f32 bs,
    ContactManifold2D& m, f32 margin) noexcept {
    const f32 dx = bx - ax, dy = by - ay;
    const Vec2 axes[4] = {{ac, as}, {-as, ac}, {bc, bs}, {-bs, bc}};
    f32 bestPen = 1e9f;
    f32 bestNx = 0.0f, bestNy = 1.0f;
    for (const Vec2& axis : axes) {
        const f32 len = std::sqrt(axis.x * axis.x + axis.y * axis.y);
        if (len < 1e-8f) continue;
        const f32 ux = axis.x / len, uy = axis.y / len;
        const f32 aProj = ahw * std::abs(ux * ac + uy * as) + ahh * std::abs(ux * (-as) + uy * ac);
        const f32 bProj = bhw * std::abs(ux * bc + uy * bs) + bhh * std::abs(ux * (-bs) + uy * bc);
        const f32 dist = std::abs(dx * ux + dy * uy);
        const f32 pen = aProj + bProj + margin - dist;
        if (pen < 0.0f) return false;
        if (pen < bestPen) {
            bestPen = pen;
            bestNx = ux;
            bestNy = uy;
            if (dx * ux + dy * uy < 0.0f) { bestNx = -bestNx; bestNy = -bestNy; }
        }
    }
    m.pointCount = 1U;
    m.normalX = bestNx;
    m.normalY = bestNy;
    m.separation[0] = -bestPen;
    m.pointX[0] = ax + bestNx * ahw;
    m.pointY[0] = ay + bestNy * ahh;
    return true;
}

bool Narrowphase2D::CollideShapes(
    EntityId idA, const Collider2D& colA, const WorldTransform2D& xfA,
    EntityId idB, const Collider2D& colB, const WorldTransform2D& xfB,
    ContactManifold2D& out, f32 margin) noexcept {
    out = {};
    out.entityA = idA;
    out.entityB = idB;
    out.friction_q16 = CombineFriction(colA.friction_q16, colB.friction_q16);
    out.restitution_q16 = CombineRestitution(colA.restitution_q16, colB.restitution_q16);
    out.isSensor = (colA.IsSensor() || colB.IsSensor()) ? 1U : 0U;

    const Vec2 cA = xfA.TransformPoint({colA.localBounds.OffsetX(), colA.localBounds.OffsetY()});
    const Vec2 cB = xfB.TransformPoint({colB.localBounds.OffsetX(), colB.localBounds.OffsetY()});
    const f32 ahw = colA.localBounds.HalfW() * std::abs(xfA.scaleX);
    const f32 ahh = colA.localBounds.HalfH() * std::abs(xfA.scaleY);
    const f32 bhw = colB.localBounds.HalfW() * std::abs(xfB.scaleX);
    const f32 bhh = colB.localBounds.HalfH() * std::abs(xfB.scaleY);
    const ShapeKind2D sA = colA.shape;
    const ShapeKind2D sB = colB.shape;
    bool ok = false;

    if (sA == ShapeKind2D::Circle && sB == ShapeKind2D::Circle) {
        ok = CollideCircleCircle(cA.x, cA.y, ahw, cB.x, cB.y, bhw, out, margin);
    } else if (sA == ShapeKind2D::Circle) {
        ok = CollideCircleAabb(cA.x, cA.y, ahw, cB.x, cB.y, bhw, bhh, out, margin);
    } else if (sB == ShapeKind2D::Circle) {
        ok = CollideCircleAabb(cB.x, cB.y, bhw, cA.x, cA.y, ahw, ahh, out, margin);
        if (ok) {
            out.normalX = -out.normalX;
            out.normalY = -out.normalY;
            std::swap(out.entityA, out.entityB);
        }
    } else if (sA == ShapeKind2D::Obb || sB == ShapeKind2D::Obb) {
        ok = CollideObbObb(cA.x, cA.y, ahw, ahh, xfA.cosRot, xfA.sinRot,
                           cB.x, cB.y, bhw, bhh, xfB.cosRot, xfB.sinRot, out, margin);
    } else {
        ok = CollideAabbAabb(cA.x, cA.y, ahw, ahh, cB.x, cB.y, bhw, bhh, out, margin);
    }
    return ok && out.pointCount > 0U;
}

} // namespace en
