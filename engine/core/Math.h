#pragma once

#include "core/Types.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace en {

inline constexpr f32 kPi = std::numbers::pi_v<f32>;
inline constexpr f32 kTwoPi = 2.0f * kPi;
inline constexpr f32 kHalfPi = 0.5f * kPi;
inline constexpr f32 kDegToRad = kPi / 180.0f;
inline constexpr f32 kRadToDeg = 180.0f / kPi;

struct Vec2 {
    f32 x{0.0f};
    f32 y{0.0f};

    [[nodiscard]] constexpr Vec2 operator+(Vec2 r) const noexcept { return {x + r.x, y + r.y}; }
    [[nodiscard]] constexpr Vec2 operator-(Vec2 r) const noexcept { return {x - r.x, y - r.y}; }
    [[nodiscard]] constexpr Vec2 operator-() const noexcept { return {-x, -y}; }
    [[nodiscard]] constexpr Vec2 operator*(f32 s) const noexcept { return {x * s, y * s}; }
    [[nodiscard]] constexpr Vec2 operator/(f32 s) const noexcept { return {x / s, y / s}; }
    constexpr Vec2& operator+=(Vec2 r) noexcept { x += r.x; y += r.y; return *this; }
    constexpr Vec2& operator-=(Vec2 r) noexcept { x -= r.x; y -= r.y; return *this; }
    constexpr Vec2& operator*=(f32 s) noexcept { x *= s; y *= s; return *this; }

    [[nodiscard]] constexpr f32 Dot(Vec2 r) const noexcept { return x * r.x + y * r.y; }
    [[nodiscard]] constexpr f32 Cross(Vec2 r) const noexcept { return x * r.y - y * r.x; }
    [[nodiscard]] constexpr f32 LengthSq() const noexcept { return x * x + y * y; }
    [[nodiscard]] f32 Length() const noexcept { return std::sqrt(LengthSq()); }
    [[nodiscard]] constexpr Vec2 PerpCCW() const noexcept { return {-y, x}; }

    [[nodiscard]] Vec2 Normalized() const noexcept {
        const f32 lenSq = LengthSq();
        if (lenSq <= 1e-12f) return {0.0f, 0.0f};
        const f32 inv = 1.0f / std::sqrt(lenSq);
        return {x * inv, y * inv};
    }

    [[nodiscard]] static constexpr Vec2 Lerp(Vec2 a, Vec2 b, f32 t) noexcept {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }
    [[nodiscard]] constexpr bool operator==(const Vec2&) const noexcept = default;
};

struct IVec2 {
    i32 x{0};
    i32 y{0};
    [[nodiscard]] constexpr bool operator==(const IVec2&) const noexcept = default;
};

struct Vec3 {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 z{0.0f};
};

struct alignas(16) Vec4 {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 z{0.0f};
    f32 w{0.0f};
};

struct alignas(16) Rect {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 w{0.0f};
    f32 h{0.0f};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return w <= 0.0f || h <= 0.0f; }
    [[nodiscard]] constexpr f32 Right() const noexcept { return x + w; }
    [[nodiscard]] constexpr f32 Bottom() const noexcept { return y + h; }
    [[nodiscard]] constexpr Vec2 Center() const noexcept { return {x + w * 0.5f, y + h * 0.5f}; }

    [[nodiscard]] constexpr bool Contains(Vec2 p) const noexcept {
        return p.x >= x && p.x <= (x + w) && p.y >= y && p.y <= (y + h);
    }

    [[nodiscard]] constexpr bool Overlaps(const Rect& o) const noexcept {
        return x < o.Right() && Right() > o.x && y < o.Bottom() && Bottom() > o.y;
    }

    [[nodiscard]] static constexpr Rect FromCenterHalf(Vec2 c, Vec2 half) noexcept {
        return {c.x - half.x, c.y - half.y, half.x * 2.0f, half.y * 2.0f};
    }
};
static_assert(sizeof(Rect) == 16);

struct Circle {
    Vec2 center{};
    f32 radius{0.0f};

    [[nodiscard]] constexpr bool Contains(Vec2 p) const noexcept {
        return (p - center).LengthSq() <= radius * radius;
    }
    [[nodiscard]] constexpr bool Overlaps(const Circle& o) const noexcept {
        const f32 r = radius + o.radius;
        return (o.center - center).LengthSq() <= r * r;
    }
};

struct Capsule2D {
    Vec2 a{};
    Vec2 b{};
    f32 radius{0.0f};

    [[nodiscard]] constexpr f32 DistanceSqToPoint(Vec2 p) const noexcept {
        const Vec2 ab = b - a;
        const f32 lenSq = ab.LengthSq();
        f32 t = 0.0f;
        if (lenSq > 1e-8f) {
            t = std::clamp((p - a).Dot(ab) / lenSq, 0.0f, 1.0f);
        }
        const Vec2 closest = a + ab * t;
        return (p - closest).LengthSq();
    }

    [[nodiscard]] constexpr bool Contains(Vec2 p) const noexcept {
        return DistanceSqToPoint(p) <= radius * radius;
    }
};

// Branchless minimax polynomial sine/cosine pair accurate to ~1e-5 across [-pi, pi].
constexpr void FastSinCos(f32 rad, f32& outCos, f32& outSin) noexcept {
    constexpr f32 kInvTwoPi = 1.0f / kTwoPi;
    const f32 turns = std::floor(rad * kInvTwoPi + 0.5f);
    f32 x = rad - turns * kTwoPi;
    f32 signCos = 1.0f;
    if (x > kHalfPi) {
        x = kPi - x;
        signCos = -1.0f;
    } else if (x < -kHalfPi) {
        x = -kPi - x;
        signCos = -1.0f;
    }
    const f32 x2 = x * x;
    outSin = x * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * 0.00019841f)));
    outCos = signCos * (1.0f - x2 * (0.5f - x2 * (0.04166667f - x2 * 0.00138889f)));
}

// Oriented 2D bounding box with Separating Axis Theorem (SAT) overlap and MTV solver.
struct OBB2D {
    Vec2 center{};
    Vec2 halfExtents{};
    f32 cosRot{1.0f};
    f32 sinRot{0.0f};

    [[nodiscard]] constexpr Vec2 AxisX() const noexcept { return {cosRot, sinRot}; }
    [[nodiscard]] constexpr Vec2 AxisY() const noexcept { return {-sinRot, cosRot}; }

    [[nodiscard]] constexpr bool Contains(Vec2 worldPt) const noexcept {
        const Vec2 d = worldPt - center;
        const f32 localX = d.x * cosRot + d.y * sinRot;
        const f32 localY = -d.x * sinRot + d.y * cosRot;
        return std::abs(localX) <= halfExtents.x && std::abs(localY) <= halfExtents.y;
    }

    [[nodiscard]] constexpr bool OverlapsSAT(const OBB2D& other, Vec2* outMtv = nullptr) const noexcept {
        const std::array<Vec2, 4> axes = {AxisX(), AxisY(), other.AxisX(), other.AxisY()};
        const Vec2 d = other.center - center;
        f32 minOverlap = 1e30f;
        Vec2 bestAxis{1.0f, 0.0f};

        for (const Vec2 axis : axes) {
            const f32 rA = halfExtents.x * std::abs(axis.Dot(AxisX())) +
                           halfExtents.y * std::abs(axis.Dot(AxisY()));
            const f32 rB = other.halfExtents.x * std::abs(axis.Dot(other.AxisX())) +
                           other.halfExtents.y * std::abs(axis.Dot(other.AxisY()));
            const f32 dist = std::abs(d.Dot(axis));
            const f32 overlap = (rA + rB) - dist;
            if (overlap <= 0.0f) return false;
            if (overlap < minOverlap) {
                minOverlap = overlap;
                bestAxis = (d.Dot(axis) < 0.0f) ? -axis : axis;
            }
        }
        if (outMtv) *outMtv = bestAxis * minOverlap;
        return true;
    }
};

struct alignas(8) Mat3x2 {
    f32 m00{1.0f}, m01{0.0f};
    f32 m10{0.0f}, m11{1.0f};
    f32 tx{0.0f}, ty{0.0f};

    [[nodiscard]] static constexpr Mat3x2 Identity() noexcept { return {}; }

    [[nodiscard]] static constexpr Mat3x2 FromTRS(Vec2 pos, f32 rotRad, Vec2 scale) noexcept {
        f32 c = 1.0f, s = 0.0f;
        FastSinCos(rotRad, c, s);
        return {c * scale.x, s * scale.x, -s * scale.y, c * scale.y, pos.x, pos.y};
    }

    [[nodiscard]] constexpr f32 Determinant() const noexcept {
        return m00 * m11 - m01 * m10;
    }

    [[nodiscard]] constexpr Mat3x2 Inverse() const noexcept {
        const f32 det = Determinant();
        if (std::abs(det) <= 1e-8f) return Identity();
        const f32 invDet = 1.0f / det;
        const f32 i00 = m11 * invDet;
        const f32 i01 = -m01 * invDet;
        const f32 i10 = -m10 * invDet;
        const f32 i11 = m00 * invDet;
        return {
            i00, i01,
            i10, i11,
            -(i00 * tx + i10 * ty),
            -(i01 * tx + i11 * ty),
        };
    }

    [[nodiscard]] constexpr Vec2 TransformPoint(Vec2 p) const noexcept {
        return {m00 * p.x + m10 * p.y + tx, m01 * p.x + m11 * p.y + ty};
    }

    [[nodiscard]] constexpr Vec2 TransformVector(Vec2 v) const noexcept {
        return {m00 * v.x + m10 * v.y, m01 * v.x + m11 * v.y};
    }

    [[nodiscard]] constexpr Mat3x2 operator*(const Mat3x2& r) const noexcept {
        return {
            m00 * r.m00 + m10 * r.m01,
            m01 * r.m00 + m11 * r.m01,
            m00 * r.m10 + m10 * r.m11,
            m01 * r.m10 + m11 * r.m11,
            m00 * r.tx + m10 * r.ty + tx,
            m01 * r.tx + m11 * r.ty + ty,
        };
    }
};

struct alignas(16) Color {
    f32 r{1.0f};
    f32 g{1.0f};
    f32 b{1.0f};
    f32 a{1.0f};

    [[nodiscard]] static constexpr Color Premultiplied(f32 r, f32 g, f32 b, f32 a = 1.0f) noexcept {
        return {r * a, g * a, b * a, a};
    }

    [[nodiscard]] static constexpr Color FromRGBA8(u8 r, u8 g, u8 b, u8 a = 255) noexcept {
        constexpr f32 kInv = 1.0f / 255.0f;
        const f32 fa = static_cast<f32>(a) * kInv;
        return {static_cast<f32>(r) * kInv * fa,
                static_cast<f32>(g) * kInv * fa,
                static_cast<f32>(b) * kInv * fa,
                fa};
    }

    [[nodiscard]] static constexpr Color FromHSV(f32 hDeg, f32 s, f32 v, f32 a = 1.0f) noexcept {
        const f32 c = v * s;
        const f32 hPrime = std::fmod(std::max(0.0f, hDeg), 360.0f) / 60.0f;
        const f32 x = c * (1.0f - std::abs(std::fmod(hPrime, 2.0f) - 1.0f));
        f32 r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
        if (hPrime < 1.0f) { r1 = c; g1 = x; }
        else if (hPrime < 2.0f) { r1 = x; g1 = c; }
        else if (hPrime < 3.0f) { g1 = c; b1 = x; }
        else if (hPrime < 4.0f) { g1 = x; b1 = c; }
        else if (hPrime < 5.0f) { r1 = x; b1 = c; }
        else { r1 = c; b1 = x; }
        const f32 m = v - c;
        return Premultiplied(r1 + m, g1 + m, b1 + m, a);
    }

    [[nodiscard]] constexpr u32 ToRGBA8() const noexcept {
        const auto pack = [](f32 v) constexpr noexcept -> u32 {
            const f32 clamped = std::clamp(v, 0.0f, 1.0f);
            return static_cast<u32>(clamped * 255.0f + 0.5f);
        };
        return (pack(r) << 24U) | (pack(g) << 16U) | (pack(b) << 8U) | pack(a);
    }

    [[nodiscard]] static constexpr Color Lerp(Color c0, Color c1, f32 t) noexcept {
        return {
            c0.r + (c1.r - c0.r) * t,
            c0.g + (c1.g - c0.g) * t,
            c0.b + (c1.b - c0.b) * t,
            c0.a + (c1.a - c0.a) * t,
        };
    }
};
static_assert(sizeof(Color) == 16);

// 2nd-order unconditionally stable implicit critically damped spring integrator.
constexpr void StepSpring(f32& pos, f32& vel, f32 target, f32 omega, f32 dt) noexcept {
    if (dt <= 0.0f) return;
    const f32 f = 1.0f + 2.0f * dt * omega;
    const f32 oo = omega * omega;
    const f32 hoo = dt * oo;
    const f32 hhoo = dt * hoo;
    const f32 detInv = 1.0f / (f + hhoo);
    const f32 detX = f * pos + dt * vel + hhoo * target;
    const f32 detV = vel + hoo * (target - pos);
    pos = detX * detInv;
    vel = detV * detInv;
}

[[nodiscard]] constexpr f32 SmoothStep(f32 edge0, f32 edge1, f32 x) noexcept {
    if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
    const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] constexpr f32 SmootherStep(f32 edge0, f32 edge1, f32 x) noexcept {
    if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
    const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

[[nodiscard]] constexpr f32 EaseOutBack(f32 t) noexcept {
    constexpr f32 c1 = 1.70158f;
    constexpr f32 c3 = c1 + 1.0f;
    const f32 u = std::clamp(t, 0.0f, 1.0f) - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

[[nodiscard]] constexpr Vec2 CubicBezier2D(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, f32 t) noexcept {
    const f32 u = 1.0f - t;
    const f32 tt = t * t;
    const f32 uu = u * u;
    const f32 uuu = uu * u;
    const f32 ttt = tt * t;
    return p0 * uuu + p1 * (3.0f * uu * t) + p2 * (3.0f * u * tt) + p3 * ttt;
}

[[nodiscard]] constexpr Vec2 CatmullRom2D(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, f32 t) noexcept {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t +
            (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
            (-p0 + p1 * 3.0f - p2 * 3.0f + p3) * t3) * 0.5f;
}

} // namespace en
