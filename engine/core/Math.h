#pragma once

#include "core/Types.h"

#include <algorithm>
#include <cmath>

namespace en {

inline constexpr f32 kPi       = 3.14159265358979323846f;
inline constexpr f32 kTwoPi    = 2.0f * kPi;
inline constexpr f32 kHalfPi   = 0.5f * kPi;
inline constexpr f32 kInvTwoPi = 1.0f / kTwoPi;
inline constexpr f32 kDegToRad = kPi / 180.0f;
inline constexpr f32 kRadToDeg = 180.0f / kPi;

[[nodiscard]] constexpr f32 Lerp(f32 a, f32 b, f32 t) noexcept {
    return a + (b - a) * t;
}

[[nodiscard]] constexpr f32 Approach(f32 cur, f32 target, f32 delta) noexcept {
    if (delta <= 0.0f) return cur;
    return (cur < target) ? std::min(cur + delta, target) : std::max(cur - delta, target);
}

[[nodiscard]] constexpr f32 SmoothStep(f32 edge0, f32 edge1, f32 x) noexcept {
    if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
    const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] constexpr f32 WrapAngle(f32 rad) noexcept {
    return rad - std::floor((rad + kPi) * kInvTwoPi) * kTwoPi;
}

// Evaluates sine and cosine simultaneously on [-pi/2, pi/2] via an 11th-order minimax polynomial.
constexpr void FastSinCos(f32 rad, f32& outCos, f32& outSin) noexcept {
    const f32 w = WrapAngle(rad);
    f32 sx = w;
    if (sx > kHalfPi) sx = kPi - sx;
    else if (sx < -kHalfPi) sx = -kPi - sx;
    const f32 cx = kHalfPi - std::abs(w);

    const auto poly = [](f32 t) constexpr noexcept -> f32 {
        const f32 t2 = t * t;
        return t * (1.0f + t2 * (-0.166666666f + t2 * (0.008333333f + t2 * (-0.0001984127f +
                   t2 * (0.000002755732f - t2 * 0.0000000250521f)))));
    };

    outSin = poly(sx);
    outCos = poly(cx);
}

// Implicit critically damped 2nd-order spring step (unconditionally stable for dt > 0).
constexpr void StepSpring(f32& pos, f32& vel, f32 target, f32 omega, f32 dt) noexcept {
    if (dt <= 0.0f || omega <= 0.0f) return;
    const f32 f = 1.0f + 2.0f * dt * omega;
    const f32 oo = omega * omega;
    const f32 hoo = dt * oo;
    const f32 hhoo = dt * hoo;
    const f32 detInv = 1.0f / (f + hhoo);
    const f32 nextPos = (f * pos + dt * vel + hhoo * target) * detInv;
    vel = (vel + hoo * (target - pos)) * detInv;
    pos = nextPos;
}

struct Vec2 {
    f32 x{0.0f};
    f32 y{0.0f};

    [[nodiscard]] constexpr Vec2 operator+(Vec2 r) const noexcept { return {x + r.x, y + r.y}; }
    [[nodiscard]] constexpr Vec2 operator-(Vec2 r) const noexcept { return {x - r.x, y - r.y}; }
    [[nodiscard]] constexpr Vec2 operator-() const noexcept { return {-x, -y}; }
    [[nodiscard]] constexpr Vec2 operator*(f32 s) const noexcept { return {x * s, y * s}; }

    [[nodiscard]] constexpr Vec2 operator/(f32 s) const noexcept {
        return (std::abs(s) <= 1e-12f) ? Vec2{} : (*this * (1.0f / s));
    }

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
        return (lenSq <= 1e-12f) ? Vec2{} : (*this * (1.0f / std::sqrt(lenSq)));
    }

    [[nodiscard]] static constexpr Vec2 Lerp(Vec2 a, Vec2 b, f32 t) noexcept {
        return {en::Lerp(a.x, b.x, t), en::Lerp(a.y, b.y, t)};
    }

    [[nodiscard]] constexpr bool operator==(const Vec2&) const noexcept = default;
};
static_assert(sizeof(Vec2) == 8 && alignof(Vec2) == 4);

[[nodiscard]] constexpr Vec2 operator*(f32 s, Vec2 v) noexcept { return v * s; }

struct IVec2 {
    i32 x{0};
    i32 y{0};

    [[nodiscard]] constexpr IVec2 operator+(IVec2 r) const noexcept { return {x + r.x, y + r.y}; }
    [[nodiscard]] constexpr IVec2 operator-(IVec2 r) const noexcept { return {x - r.x, y - r.y}; }
    [[nodiscard]] constexpr IVec2 operator*(i32 s) const noexcept { return {x * s, y * s}; }
    [[nodiscard]] constexpr auto operator<=>(const IVec2&) const noexcept = default;
};
static_assert(sizeof(IVec2) == 8 && alignof(IVec2) == 4);

struct Rect {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 w{0.0f};
    f32 h{0.0f};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return w <= 0.0f || h <= 0.0f; }
    [[nodiscard]] constexpr f32 Right() const noexcept { return x + w; }
    [[nodiscard]] constexpr f32 Bottom() const noexcept { return y + h; }
    [[nodiscard]] constexpr Vec2 Center() const noexcept { return {x + w * 0.5f, y + h * 0.5f}; }
    [[nodiscard]] constexpr Vec2 HalfExtents() const noexcept { return {w * 0.5f, h * 0.5f}; }

    [[nodiscard]] constexpr bool Contains(Vec2 p) const noexcept {
        return !IsEmpty() && p.x >= x && p.x <= Right() && p.y >= y && p.y <= Bottom();
    }

    [[nodiscard]] constexpr bool Overlaps(const Rect& o) const noexcept {
        return !IsEmpty() && !o.IsEmpty() &&
               x < o.Right() && Right() > o.x && y < o.Bottom() && Bottom() > o.y;
    }

    [[nodiscard]] constexpr Rect Intersection(const Rect& o) const noexcept {
        const f32 nx = std::max(x, o.x);
        const f32 ny = std::max(y, o.y);
        const f32 nr = std::min(Right(), o.Right());
        const f32 nb = std::min(Bottom(), o.Bottom());
        return (nr <= nx || nb <= ny) ? Rect{} : Rect{nx, ny, nr - nx, nb - ny};
    }

    [[nodiscard]] static constexpr Rect FromCenterHalf(Vec2 center, Vec2 half) noexcept {
        return (half.x <= 0.0f || half.y <= 0.0f)
            ? Rect{}
            : Rect{center.x - half.x, center.y - half.y, half.x * 2.0f, half.y * 2.0f};
    }

    [[nodiscard]] constexpr bool operator==(const Rect&) const noexcept = default;
};
static_assert(sizeof(Rect) == 16 && alignof(Rect) == 4);

// Swept 2D AABB collision result for continuous Metroidvania platforming and dash kinematics.
// Value type; time in [0, 1] indicates normalized impact fraction along movement delta.
struct SweepHit {
    f32  time{1.0f};
    Vec2 normal{};
    u8   hit{0};
    u8   pad[3]{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hit != 0U; }
};
static_assert(sizeof(SweepHit) == 16);

// Computes Minkowski-expanded slab intersection of `moving` travelling by `delta` against `staticBox`.
[[nodiscard]] constexpr SweepHit SweepRect(const Rect& moving, Vec2 delta, const Rect& staticBox) noexcept {
    if (moving.IsEmpty() || staticBox.IsEmpty()) return {};

    const Rect expanded{
        staticBox.x - moving.w * 0.5f,
        staticBox.y - moving.h * 0.5f,
        staticBox.w + moving.w,
        staticBox.h + moving.h,
    };
    const Vec2 origin = moving.Center();

    f32 tNearX = -1e30f, tFarX = 1e30f;
    if (std::abs(delta.x) <= 1e-8f) {
        if (origin.x <= expanded.x || origin.x >= expanded.Right()) return {};
    } else {
        const f32 invX = 1.0f / delta.x;
        tNearX = (expanded.x - origin.x) * invX;
        tFarX  = (expanded.Right() - origin.x) * invX;
        if (tNearX > tFarX) std::swap(tNearX, tFarX);
    }

    f32 tNearY = -1e30f, tFarY = 1e30f;
    if (std::abs(delta.y) <= 1e-8f) {
        if (origin.y <= expanded.y || origin.y >= expanded.Bottom()) return {};
    } else {
        const f32 invY = 1.0f / delta.y;
        tNearY = (expanded.y - origin.y) * invY;
        tFarY  = (expanded.Bottom() - origin.y) * invY;
        if (tNearY > tFarY) std::swap(tNearY, tFarY);
    }

    const f32 tEnter = std::max(tNearX, tNearY);
    const f32 tExit  = std::min(tFarX, tFarY);
    if (tEnter > tExit || tEnter < 0.0f || tEnter >= 1.0f || tExit <= 0.0f) return {};

    Vec2 n{};
    if (tNearX > tNearY) {
        n.x = (delta.x < 0.0f) ? 1.0f : -1.0f;
    } else {
        n.y = (delta.y < 0.0f) ? 1.0f : -1.0f;
    }
    return SweepHit{.time = tEnter, .normal = n, .hit = 1U};
}

struct Mat3x2 {
    f32 m00{1.0f}, m01{0.0f};
    f32 m10{0.0f}, m11{1.0f};
    f32 tx{0.0f},  ty{0.0f};

    [[nodiscard]] static constexpr Mat3x2 Identity() noexcept { return {}; }

    [[nodiscard]] static constexpr Mat3x2 FromTRS(Vec2 pos, f32 rotRad, Vec2 scale = {1.0f, 1.0f}) noexcept {
        f32 c = 1.0f, s = 0.0f;
        FastSinCos(rotRad, c, s);
        return {c * scale.x, s * scale.x, -s * scale.y, c * scale.y, pos.x, pos.y};
    }

    [[nodiscard]] constexpr f32 Determinant() const noexcept { return m00 * m11 - m01 * m10; }

    [[nodiscard]] constexpr Mat3x2 Inverse() const noexcept {
        const f32 det = Determinant();
        if (std::abs(det) <= 1e-8f) return Identity();
        const f32 invDet = 1.0f / det;
        const f32 i00 =  m11 * invDet;
        const f32 i01 = -m01 * invDet;
        const f32 i10 = -m10 * invDet;
        const f32 i11 =  m00 * invDet;
        return {i00, i01, i10, i11, -(i00 * tx + i10 * ty), -(i01 * tx + i11 * ty)};
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
            m00 * r.tx  + m10 * r.ty + tx,
            m01 * r.tx  + m11 * r.ty + ty,
        };
    }

    [[nodiscard]] constexpr bool operator==(const Mat3x2&) const noexcept = default;
};
static_assert(sizeof(Mat3x2) == 24 && alignof(Mat3x2) == 4);

struct Color {
    f32 r{1.0f}, g{1.0f}, b{1.0f}, a{1.0f};

    [[nodiscard]] static constexpr Color Premultiplied(f32 r, f32 g, f32 b, f32 a = 1.0f) noexcept {
        return {r * a, g * a, b * a, a};
    }

    [[nodiscard]] static constexpr Color FromRGBA8(u8 r, u8 g, u8 b, u8 a = 255) noexcept {
        constexpr f32 kInv = 1.0f / 255.0f;
        return Premultiplied(r * kInv, g * kInv, b * kInv, a * kInv);
    }

    [[nodiscard]] constexpr u32 ToRGBA8() const noexcept {
        const auto pack = [](f32 v) constexpr noexcept -> u32 {
            return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return (pack(r) << 24U) | (pack(g) << 16U) | (pack(b) << 8U) | pack(a);
    }

    [[nodiscard]] static constexpr Color Lerp(Color c0, Color c1, f32 t) noexcept {
        return {
            en::Lerp(c0.r, c1.r, t),
            en::Lerp(c0.g, c1.g, t),
            en::Lerp(c0.b, c1.b, t),
            en::Lerp(c0.a, c1.a, t),
        };
    }

    [[nodiscard]] constexpr bool operator==(const Color&) const noexcept = default;
};
static_assert(sizeof(Color) == 16 && alignof(Color) == 4);

} // namespace en
