#pragma once

#include "core/Types.h"
#include "core/Constants.h"
#include "core/Error.h"
#include "core/Result.h"
#include "math/Vec2.h"

#include <cmath>
#include <algorithm>
#include <optional>
#include <span>
#include <cstring>
#include <source_location>

namespace EngineN {

// Enumerations

enum class BlendMode : u8 {
    Opaque = 0,         ///< Overwrites destination completely (no blending)
    Alpha = 1,          ///< Traditional straight alpha blending
    Premultiplied = 2,  ///< Canonical premultiplied alpha (recommended for 2D VFX)
    Additive = 3,       ///< Color addition (glow, laser, fire)
    Multiply = 4        ///< Color multiplication (shadows, tints)
};

enum class TextureFilter : u8 {
    Nearest = 0,        ///< Pixel-perfect point sampling (crisp pixel art)
    Linear = 1          ///< Bilinear filtering (smooth vector/HD art)
};

enum class PresentationMode : u8 {
    Disabled = 0,       ///< 1:1 window size, no virtual scaling
    Stretch = 1,        ///< Stretched to fill window ignoring aspect ratio
    Letterbox = 2,      ///< Preserves aspect ratio with black bars
    Overscan = 3,       ///< Preserves aspect ratio, crops overflow
    IntegerScale = 4    ///< Integer multiples of virtual canvas (crisp pixel art)
};

enum class VSyncMode : u8 {
    Off = 0,
    On = 1,
    Adaptive = 2        ///< Tear-free when at refresh rate, tears below to prevent hitching
};

enum class RenderPass : u8 {
    Background = 0,     ///< Parallax background scenery, far skies
    World = 1,          ///< Level tiles, props, platforms, entities, hazards
    Overlay = 2,        ///< Foreground parallax silhouettes, weather VFX, screen-tint
    Ui = 3              ///< Virtual canvas screen-space HUD, dialogs, cursor
};

enum class SpriteFlip : u8 {
    None = 0,
    FlipH = 1,          ///< Flip horizontally along X axis
    FlipV = 2,          ///< Flip vertically along Y axis
    FlipHV = 3          ///< Flip both horizontally and vertically
};

enum class RenderErrc : i32 {
    Success = 0,
    DeviceCreationFailed = 1,
    ContextInitFailed = 2,
    TextureCreationFailed = 3,
    TextureNotFound = 4,
    TextureLimitReached = 5,
    FrameOrderViolation = 6,
    QueueCapacityExceeded = 7,
    InvalidConfiguration = 8,
    BackendError = 9
};

inline EngineError MakeRenderError(
    RenderErrc code,
    std::string_view msg,
    std::source_location loc = std::source_location::current()) noexcept
{
    return EngineError(msg, static_cast<i32>(code), loc);
}

namespace Detail {
inline constexpr u32 ChannelToByte(f32 v) noexcept {
    if (std::isnan(v) || v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<u32>(v * 255.0f + 0.5f);
}
} // namespace Detail

/// Canonical 32-bit floating-point RGBA representation with [0.0, 1.0] channels.
struct Color {
    f32 r = 1.0f;
    f32 g = 1.0f;
    f32 b = 1.0f;
    f32 a = 1.0f;

    constexpr Color() = default;
    constexpr Color(f32 inR, f32 inG, f32 inB, f32 inA = 1.0f) noexcept
        : r(inR), g(inG), b(inB), a(inA) {}

    static constexpr Color FromRGBA8(u8 r8, u8 g8, u8 b8, u8 a8 = 255) noexcept {
        constexpr f32 inv = 1.0f / 255.0f;
        return {
            static_cast<f32>(r8) * inv,
            static_cast<f32>(g8) * inv,
            static_cast<f32>(b8) * inv,
            static_cast<f32>(a8) * inv
        };
    }

    static constexpr Color FromHex(u32 hexRGBA) noexcept {
        const u8 r8 = static_cast<u8>((hexRGBA >> 24) & 0xFF);
        const u8 g8 = static_cast<u8>((hexRGBA >> 16) & 0xFF);
        const u8 b8 = static_cast<u8>((hexRGBA >> 8) & 0xFF);
        const u8 a8 = static_cast<u8>(hexRGBA & 0xFF);
        return FromRGBA8(r8, g8, b8, a8);
    }

    [[nodiscard]] constexpr u32 ToPackedRGBA8() const noexcept {
        const u32 rb = Detail::ChannelToByte(r);
        const u32 gb = Detail::ChannelToByte(g);
        const u32 bb = Detail::ChannelToByte(b);
        const u32 ab = Detail::ChannelToByte(a);
        return rb | (gb << 8) | (bb << 16) | (ab << 24);
    }

    [[nodiscard]] constexpr u32 ToHex() const noexcept {
        return (Detail::ChannelToByte(r) << 24) |
               (Detail::ChannelToByte(g) << 16) |
               (Detail::ChannelToByte(b) << 8)  |
               Detail::ChannelToByte(a);
    }

    [[nodiscard]] constexpr Color Premultiplied() const noexcept {
        return { r * a, g * a, b * a, a };
    }

    [[nodiscard]] constexpr Color Unpremultiplied() const noexcept {
        if (a <= 1e-6f) return { 0.0f, 0.0f, 0.0f, 0.0f };
        return { r / a, g / a, b / a, a };
    }

    constexpr Color operator*(const Color& rhs) const noexcept {
        return { r * rhs.r, g * rhs.g, b * rhs.b, a * rhs.a };
    }

    constexpr Color operator*(f32 scalar) const noexcept {
        return { r * scalar, g * scalar, b * scalar, a * scalar };
    }

    constexpr bool operator==(const Color& other) const noexcept = default;
};

inline constexpr Color Lerp(const Color& c1, const Color& c2, f32 t) noexcept {
    return {
        c1.r + (c2.r - c1.r) * t,
        c1.g + (c2.g - c1.g) * t,
        c1.b + (c2.b - c1.b) * t,
        c1.a + (c2.a - c1.a) * t
    };
}

namespace Colors {
    inline constexpr Color Transparent{0.0f, 0.0f, 0.0f, 0.0f};
    inline constexpr Color White{1.0f, 1.0f, 1.0f, 1.0f};
    inline constexpr Color Black{0.0f, 0.0f, 0.0f, 1.0f};
    inline constexpr Color Red{1.0f, 0.0f, 0.0f, 1.0f};
    inline constexpr Color Green{0.0f, 1.0f, 0.0f, 1.0f};
    inline constexpr Color Blue{0.0f, 0.0f, 1.0f, 1.0f};
    inline constexpr Color Yellow{1.0f, 1.0f, 0.0f, 1.0f};
    inline constexpr Color Cyan{0.0f, 1.0f, 1.0f, 1.0f};
    inline constexpr Color Magenta{1.0f, 0.0f, 1.0f, 1.0f};
    inline constexpr Color Gray{0.5f, 0.5f, 0.5f, 1.0f};
}

// Geometric Rectangles (Half-Open Intervals [min, max))

struct Rect {
    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;

    constexpr Rect() = default;
    constexpr Rect(i32 inX, i32 inY, i32 inW, i32 inH) noexcept
        : x(inX), y(inY), w(inW), h(inH) {}

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return w <= 0 || h <= 0; }

    [[nodiscard]] constexpr bool Contains(i32 px, i32 py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }

    [[nodiscard]] constexpr bool Overlaps(const Rect& other) const noexcept {
        return x < other.x + other.w && x + w > other.x &&
               y < other.y + other.h && y + h > other.y;
    }

    [[nodiscard]] constexpr Rect Intersection(const Rect& other) const noexcept {
        const i32 nx = std::max(x, other.x);
        const i32 ny = std::max(y, other.y);
        const i32 mx = std::min(x + w, other.x + other.w);
        const i32 my = std::min(y + h, other.y + other.h);
        if (mx <= nx || my <= ny) return {0, 0, 0, 0};
        return {nx, ny, mx - nx, my - ny};
    }

    constexpr bool operator==(const Rect&) const noexcept = default;
};

struct FRect {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;

    constexpr FRect() = default;
    constexpr FRect(f32 inX, f32 inY, f32 inW, f32 inH) noexcept
        : x(inX), y(inY), w(inW), h(inH) {}

    [[nodiscard]] constexpr bool IsEmpty() const noexcept {
        return !(w > 0.0f && h > 0.0f);
    }

    [[nodiscard]] constexpr bool Contains(Vec2 p) const noexcept {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }

    [[nodiscard]] constexpr bool Overlaps(const FRect& other) const noexcept {
        return x < other.x + other.w && x + w > other.x &&
               y < other.y + other.h && y + h > other.y;
    }

    [[nodiscard]] constexpr FRect Intersection(const FRect& other) const noexcept {
        const f32 nx = std::max(x, other.x);
        const f32 ny = std::max(y, other.y);
        const f32 mx = std::min(x + w, other.x + other.w);
        const f32 my = std::min(y + h, other.y + other.h);
        if (mx <= nx || my <= ny) return {0.0f, 0.0f, 0.0f, 0.0f};
        return {nx, ny, mx - nx, my - ny};
    }

    [[nodiscard]] constexpr FRect Union(const FRect& other) const noexcept {
        if (IsEmpty()) return other;
        if (other.IsEmpty()) return *this;
        const f32 nx = std::min(x, other.x);
        const f32 ny = std::min(y, other.y);
        const f32 mx = std::max(x + w, other.x + other.w);
        const f32 my = std::max(y + h, other.y + other.h);
        return {nx, ny, mx - nx, my - ny};
    }

    [[nodiscard]] constexpr FRect Expanded(f32 amount) const noexcept {
        const f32 nw = w + 2.0f * amount;
        const f32 nh = h + 2.0f * amount;
        if (nw <= 0.0f || nh <= 0.0f) {
            return {x + w * 0.5f, y + h * 0.5f, 0.0f, 0.0f};
        }
        return {x - amount, y - amount, nw, nh};
    }

    [[nodiscard]] constexpr Vec2 Center() const noexcept {
        return {x + w * 0.5f, y + h * 0.5f};
    }

    [[nodiscard]] constexpr Vec2 Min() const noexcept { return {x, y}; }
    [[nodiscard]] constexpr Vec2 Max() const noexcept { return {x + w, y + h}; }

    constexpr bool operator==(const FRect&) const noexcept = default;
};

// 2D Affine Transformation Matrix (2x3)
// Column-vector convention: [x', y', 1]^T = M * [x, y, 1]^T
struct Mat2D {
    f32 a  = 1.0f;
    f32 b  = 0.0f;
    f32 c  = 0.0f;
    f32 d  = 1.0f;
    f32 tx = 0.0f;
    f32 ty = 0.0f;

    static constexpr Mat2D Identity() noexcept { return {}; }

    static constexpr Mat2D Translation(f32 x, f32 y) noexcept {
        return {1.0f, 0.0f, 0.0f, 1.0f, x, y};
    }

    static constexpr Mat2D Scale(f32 sx, f32 sy) noexcept {
        return {sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
    }

    static Mat2D Rotation(f32 radians) noexcept {
        const f32 cosA = std::cos(radians);
        const f32 sinA = std::sin(radians);
        return {cosA, sinA, -sinA, cosA, 0.0f, 0.0f};
    }

    /// Multiply two matrices (A * B applies B first, then A).
    constexpr Mat2D operator*(const Mat2D& rhs) const noexcept {
        return {
            a * rhs.a + c * rhs.b,
            b * rhs.a + d * rhs.b,
            a * rhs.c + c * rhs.d,
            b * rhs.c + d * rhs.d,
            a * rhs.tx + c * rhs.ty + tx,
            b * rhs.tx + d * rhs.ty + ty
        };
    }

    [[nodiscard]] constexpr Vec2 TransformPoint(Vec2 p) const noexcept {
        return {
            a * p.x + c * p.y + tx,
            b * p.x + d * p.y + ty
        };
    }

    [[nodiscard]] constexpr Vec2 TransformVector(Vec2 v) const noexcept {
        return {
            a * v.x + c * v.y,
            b * v.x + d * v.y
        };
    }

    [[nodiscard]] constexpr f32 Determinant() const noexcept {
        return a * d - b * c;
    }

    [[nodiscard]] std::optional<Mat2D> TryInverse() const noexcept {
        const f32 det = Determinant();
        if (std::abs(det) < 1e-9f || std::isnan(det) || std::isinf(det)) {
            return std::nullopt;
        }
        const f32 invDet = 1.0f / det;
        return Mat2D{
            d * invDet,
            -b * invDet,
            -c * invDet,
            a * invDet,
            (c * ty - d * tx) * invDet,
            (b * tx - a * ty) * invDet
        };
    }
};

// Entity 2D Transform with Sub-Frame Shortest-Arc Lerp
struct Transform2D {
    Vec2 position{0.0f, 0.0f};
    f32  rotation = 0.0f; // radians
    Vec2 scale{1.0f, 1.0f};
    Vec2 pivot{0.0f, 0.0f};

    [[nodiscard]] Mat2D ToMatrix() const noexcept {
        const f32 cosA = std::cos(rotation);
        const f32 sinA = std::sin(rotation);
        const f32 a = cosA * scale.x;
        const f32 b = sinA * scale.x;
        const f32 c = -sinA * scale.y;
        const f32 d = cosA * scale.y;
        const f32 tx = position.x - (a * pivot.x + c * pivot.y);
        const f32 ty = position.y - (b * pivot.x + d * pivot.y);
        return {a, b, c, d, tx, ty};
    }

    static Transform2D Lerp(const Transform2D& from, const Transform2D& to, f32 t) noexcept {
        const f32 angleDiff = std::remainder(to.rotation - from.rotation, kTwoPi);
        return {
            .position = EngineN::Lerp(from.position, to.position, t),
            .rotation = from.rotation + angleDiff * t,
            .scale = EngineN::Lerp(from.scale, to.scale, t),
            .pivot = EngineN::Lerp(from.pivot, to.pivot, t)
        };
    }
};

// Viewport & 2D Camera View Transform
struct ViewTransform2D {
    FRect viewport{0.0f, 0.0f, 1280.0f, 720.0f};
    Vec2  center{640.0f, 360.0f};
    f32   zoom = 1.0f;
    f32   rotation = 0.0f; // Camera rotation in radians
    bool  screenSpace = false;

    [[nodiscard]] Mat2D ToMatrix() const noexcept {
        if (screenSpace) {
            return Mat2D::Identity();
        }
        const f32 z = (std::abs(zoom) < 1e-6f) ? 1.0f : zoom;
        const f32 cosA = std::cos(rotation) * z;
        const f32 sinA = -std::sin(rotation) * z;
        const f32 a = cosA;
        const f32 b = sinA;
        const f32 c = -sinA;
        const f32 d = cosA;
        const Vec2 vpCenter = viewport.Center();
        const f32 tx = vpCenter.x - (a * center.x + c * center.y);
        const f32 ty = vpCenter.y - (b * center.x + d * center.y);
        return {a, b, c, d, tx, ty};
    }

    [[nodiscard]] Vec2 WorldToScreen(Vec2 worldPos) const noexcept {
        return ToMatrix().TransformPoint(worldPos);
    }

    [[nodiscard]] Vec2 ScreenToWorld(Vec2 screenPos) const noexcept {
        const auto inv = ToMatrix().TryInverse();
        if (inv) {
            return inv->TransformPoint(screenPos);
        }
        return screenPos;
    }

    [[nodiscard]] FRect VisibleWorldBounds() const noexcept {
        if (screenSpace) {
            return viewport;
        }
        const auto inv = ToMatrix().TryInverse();
        if (!inv) return viewport;
        const Vec2 p0 = inv->TransformPoint({viewport.x, viewport.y});
        const Vec2 p1 = inv->TransformPoint({viewport.x + viewport.w, viewport.y});
        const Vec2 p2 = inv->TransformPoint({viewport.x + viewport.w, viewport.y + viewport.h});
        const Vec2 p3 = inv->TransformPoint({viewport.x, viewport.y + viewport.h});
        const f32 minX = std::min({p0.x, p1.x, p2.x, p3.x});
        const f32 maxX = std::max({p0.x, p1.x, p2.x, p3.x});
        const f32 minY = std::min({p0.y, p1.y, p2.y, p3.y});
        const f32 maxY = std::max({p0.y, p1.y, p2.y, p3.y});
        return {minX, minY, maxX - minX, maxY - minY};
    }

    static ViewTransform2D Lerp(const ViewTransform2D& from, const ViewTransform2D& to, f32 t) noexcept {
        const f32 angleDiff = std::remainder(to.rotation - from.rotation, kTwoPi);
        return {
            .viewport = to.viewport,
            .center = EngineN::Lerp(from.center, to.center, t),
            .zoom = from.zoom + (to.zoom - from.zoom) * t,
            .rotation = from.rotation + angleDiff * t,
            .screenSpace = to.screenSpace
        };
    }
};

// Canonical 20-Byte QuadVertex
struct alignas(4) Vertex {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 u = 0.0f;
    f32 v = 0.0f;
    u32 rgba = 0xFFFFFFFF; // Packed 32-bit RGBA (R in byte 0, A in byte 3)
};
static_assert(sizeof(Vertex) == 20, "Vertex must be exactly 20 bytes for optimal cache throughput");

/// 64-Bit Radix Sort Key: determines draw order and state batching.
/// High bits represent coarse render stages (Painter's Algorithm);
/// low bits group identical GPU states to minimize state changes.
///
/// Bit Layout (MSB to LSB):
///  - [63:60] RenderPass  (4 bits) : Background < World < Overlay < Ui
///  - [59:52] Layer       (8 bits) : Parallax / tile / entity ordering (0..255)
///  - [51:36] Depth       (16 bits): Quantized [0.0..1.0] for Y-sorting / sub-layers
///  - [35:32] BlendMode   (4 bits) : Opaque, Alpha, Premultiplied, Additive, Multiply
///  - [31:24] Material    (8 bits) : Shader / Material ID
///  - [23: 8] TextureSlot (16 bits): Texture slot table index (batching identical textures)
///  - [ 7: 0] Reserved    (8 bits) : Unused (skipped during radix sorting passes)
struct SortKey {
    static constexpr u64 Pack(
        RenderPass pass,
        u8 layer,
        f32 depth01,
        BlendMode blend,
        u8 material,
        u16 textureSlot) noexcept
    {
        const f32 safeDepth = std::isnan(depth01) ? 0.0f : std::clamp(depth01, 0.0f, 1.0f);
        const u16 quantizedDepth = static_cast<u16>(safeDepth * 65535.0f + 0.5f);

        const u64 p = static_cast<u64>(pass) & 0x0FULL;
        const u64 l = static_cast<u64>(layer) & 0xFFULL;
        const u64 d = static_cast<u64>(quantizedDepth) & 0xFFFFULL;
        const u64 b = static_cast<u64>(blend) & 0x0FULL;
        const u64 m = static_cast<u64>(material) & 0xFFULL;
        const u64 t = static_cast<u64>(textureSlot) & 0xFFFFULL;

        return (p << 60) | (l << 52) | (d << 36) | (b << 32) | (m << 24) | (t << 8);
    }

    static constexpr RenderPass UnpackPass(u64 key) noexcept {
        return static_cast<RenderPass>((key >> 60) & 0x0F);
    }
    static constexpr u8 UnpackLayer(u64 key) noexcept {
        return static_cast<u8>((key >> 52) & 0xFF);
    }
    static constexpr u16 UnpackDepth(u64 key) noexcept {
        return static_cast<u16>((key >> 36) & 0xFFFF);
    }
    static constexpr BlendMode UnpackBlend(u64 key) noexcept {
        return static_cast<BlendMode>((key >> 32) & 0x0F);
    }
    static constexpr u8 UnpackMaterial(u64 key) noexcept {
        return static_cast<u8>((key >> 24) & 0xFF);
    }
    static constexpr u16 UnpackTextureSlot(u64 key) noexcept {
        return static_cast<u16>((key >> 8) & 0xFFFF);
    }
};

// Per-Frame Render Statistics
struct FrameStats {
    u32 drawCount = 0;       ///< Number of draw commands submitted to the queue
    u32 vertexCount = 0;     ///< Total vertices written
    u32 indexCount = 0;      ///< Total indices written
    u32 runCount = 0;        ///< Merged draw calls sent to backend
    u32 culledDraws = 0;     ///< Draws skipped by frustum culling
    u32 droppedDraws = 0;    ///< Draws dropped due to queue arena overflow
};

} // namespace EngineN
