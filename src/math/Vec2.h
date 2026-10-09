#pragma once

#include <cmath>

namespace EngineN {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float inX, float inY) : x(inX), y(inY) {}

    constexpr Vec2 operator+(const Vec2& other) const { return {x + other.x, y + other.y}; }
    constexpr Vec2 operator-(const Vec2& other) const { return {x - other.x, y - other.y}; }
    constexpr Vec2 operator*(float scalar) const { return {x * scalar, y * scalar}; }
    constexpr Vec2 operator/(float scalar) const { return {x / scalar, y / scalar}; }

    Vec2& operator+=(const Vec2& other) {
        x += other.x;
        y += other.y;
        return *this;
    }

    Vec2& operator-=(const Vec2& other) {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    Vec2& operator*=(float scalar) {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    [[nodiscard]] float LengthSquared() const { return x * x + y * y; }
    [[nodiscard]] float Length() const { return std::sqrt(LengthSquared()); }

    [[nodiscard]] Vec2 Normalized() const {
        float len = Length();
        if (len > 1e-6f) {
            return {x / len, y / len};
        }
        return {0.0f, 0.0f};
    }

    static constexpr Vec2 Zero() { return {0.0f, 0.0f}; }
};

inline constexpr float Dot(const Vec2& a, const Vec2& b) {
    return a.x * b.x + a.y * b.y;
}

inline constexpr Vec2 Lerp(const Vec2& a, const Vec2& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

} // namespace EngineN
