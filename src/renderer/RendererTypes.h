#pragma once

#include "core/Types.h"
#include <algorithm>
#include <cmath>

namespace EngineN {

/**
 * Normalized 32-bit float RGBA color with premultiplied alpha support.
 */
struct Color {
    f32 r{0.0f};
    f32 g{0.0f};
    f32 b{0.0f};
    f32 a{1.0f};

    constexpr Color() = default;
    constexpr Color(f32 inR, f32 inG, f32 inB, f32 inA = 1.0f) noexcept
        : r(inR), g(inG), b(inB), a(inA) {}

    [[nodiscard]] static constexpr Color FromRGBA8(u8 inR, u8 inG, u8 inB, u8 inA = 255) noexcept {
        constexpr f32 kInv255 = 1.0f / 255.0f;
        return {
            static_cast<f32>(inR) * kInv255,
            static_cast<f32>(inG) * kInv255,
            static_cast<f32>(inB) * kInv255,
            static_cast<f32>(inA) * kInv255
        };
    }

    [[nodiscard]] constexpr u32 ToPremultipliedRGBA8() const noexcept {
        const f32 clampedA = std::clamp(a, 0.0f, 1.0f);
        const u32 pr = static_cast<u32>(std::clamp(r * clampedA, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 pg = static_cast<u32>(std::clamp(g * clampedA, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 pb = static_cast<u32>(std::clamp(b * clampedA, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 pa = static_cast<u32>(clampedA * 255.0f + 0.5f);
        return (pa << 24) | (pb << 16) | (pg << 8) | pr;
    }

    [[nodiscard]] constexpr u32 ToStraightRGBA8() const noexcept {
        const u32 pr = static_cast<u32>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 pg = static_cast<u32>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 pb = static_cast<u32>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 pa = static_cast<u32>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        return (pa << 24) | (pb << 16) | (pg << 8) | pr;
    }
};

namespace Colors {
    inline constexpr Color White{1.0f, 1.0f, 1.0f, 1.0f};
    inline constexpr Color Black{0.0f, 0.0f, 0.0f, 1.0f};
    inline constexpr Color Transparent{0.0f, 0.0f, 0.0f, 0.0f};
    inline constexpr Color Red{1.0f, 0.0f, 0.0f, 1.0f};
    inline constexpr Color Green{0.0f, 1.0f, 0.0f, 1.0f};
    inline constexpr Color Blue{0.0f, 0.0f, 1.0f, 1.0f};
    inline constexpr Color Yellow{1.0f, 1.0f, 0.0f, 1.0f};
}

/**
 * 2D floating-point rectangle.
 */
struct FRect {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 w{0.0f};
    f32 h{0.0f};

    constexpr FRect() = default;
    constexpr FRect(f32 inX, f32 inY, f32 inW, f32 inH) noexcept
        : x(inX), y(inY), w(inW), h(inH) {}

    [[nodiscard]] constexpr bool Contains(f32 px, f32 py) const noexcept {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }

    [[nodiscard]] constexpr bool Intersects(const FRect& o) const noexcept {
        return x < (o.x + o.w) && (x + w) > o.x && y < (o.y + o.h) && (y + h) > o.y;
    }
};

/**
 * Generational texture handle to safely invalidate recycled GPU descriptors.
 */
struct TextureHandle {
    u16 slot{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
    [[nodiscard]] static constexpr TextureHandle Invalid() noexcept { return {0, 0}; }
    [[nodiscard]] static constexpr TextureHandle White() noexcept { return {1, 1}; }

    constexpr auto operator<=>(const TextureHandle&) const = default;
};

enum class RenderPass : u8 {
    Scene      = 0,
    Distortion = 1,
    Lights     = 2,
    Bloom      = 3,
    Composite  = 4,
    Ui         = 5,
    DevTools   = 6,
    Count      = 7
};

enum class BlendMode : u8 {
    Premultiplied = 0,
    Additive      = 1,
    Multiply      = 2,
    Opaque        = 3,
    Max           = 4,
    Count         = 5
};

enum class PresentMode : u8 {
    VSync     = 0, // FIFO
    Immediate = 1, // Immediate uncapped
    Mailbox   = 2  // Lowest latency uncapped without tearing
};

enum class ScaleMode : u8 {
    Fit        = 0, // Letterbox preserving aspect ratio
    IntegerFit = 1, // Pixel-perfect integer ratio letterbox
    Cover      = 2  // Fill screen cropping excess
};

enum class PipelineKind : u16 {
    Sprite    = 0,
    SDFShape  = 1,
    MSDFText  = 2,
    Composite = 3,
    Count     = 4
};

/**
 * Passive 2D view transformation consumed by the presentation layer.
 */
struct ViewTransform2D {
    f32 viewProjection[16]{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    f32 viewport[4]{0.0f, 0.0f, 1280.0f, 720.0f};
    f32 zoom{1.0f};
    f32 rotation{0.0f};
    FRect visibleFrustumWorldBounds{0.0f, 0.0f, 1280.0f, 720.0f};
    bool screenSpaceBypass{false};
};

} // namespace EngineN
