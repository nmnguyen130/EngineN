#pragma once

#include "core/Math.h"

namespace en {

struct TextureTag {};
using TextureHandle = Handle<TextureTag>;

inline constexpr TextureHandle kWhiteTexture{0, 1};

enum class RendererErrc : u8 {
    None = 0,
    DeviceInitFailed,
    TextureCreateFailed,
    TextureUpdateFailed,
    InvalidHandle,
    PoolExhausted,
    InvalidDimension,
};

enum class TextureFormat : u8 { RGBA8 = 0, BGRA8 = 1 };
enum class TextureFilter : u8 { Nearest = 0, Linear = 1, PixelArt = 2 };
enum class BlendMode : u8 { Alpha = 0, Additive = 1, Multiply = 2, Opaque = 3 };
enum class PresentMode : u8 { Immediate = 0, VSync = 1, AdaptiveVSync = 2 };

// Pixel size. Converts from any {.w,.h} (e.g. platform Extent2i).
struct alignas(8) Viewport {
    i32 w{1280};
    i32 h{720};

    constexpr Viewport() noexcept = default;
    constexpr Viewport(i32 width, i32 height) noexcept : w(width), h(height) {}

    template <typename T>
        requires (!std::same_as<std::remove_cvref_t<T>, Viewport>) &&
                 requires(const T& e) {
                     { e.w } -> std::convertible_to<i32>;
                     { e.h } -> std::convertible_to<i32>;
                 }
    constexpr Viewport(const T& e) noexcept : w(static_cast<i32>(e.w)), h(static_cast<i32>(e.h)) {}

    [[nodiscard]] constexpr bool Valid() const noexcept { return w > 0 && h > 0; }
    [[nodiscard]] constexpr f32 Aspect() const noexcept {
        return h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 1.0f;
    }
    [[nodiscard]] constexpr bool operator==(const Viewport&) const noexcept = default;
};
static_assert(sizeof(Viewport) == 8);

struct alignas(8) TextureDesc {
    std::span<const u8> pixels{};
    u32 width{0};
    u32 height{0};
    u32 pitch{0};
    TextureFormat format{TextureFormat::RGBA8};
    TextureFilter filter{TextureFilter::Nearest};
    bool renderTarget{false};
};
static_assert(sizeof(TextureDesc) == 32);

// Contiguous xy / rgba / uv for SDL_RenderGeometryRaw (no Color embed — avoids alignas(16) pad).
struct alignas(4) Vertex2D {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 r{1.0f};
    f32 g{1.0f};
    f32 b{1.0f};
    f32 a{1.0f};
    f32 u{0.0f};
    f32 v{0.0f};
};
static_assert(sizeof(Vertex2D) == 32);

struct alignas(4) QuadCmd {
    Vec2 center{0.0f, 0.0f};
    Vec2 size{1.0f, 1.0f};
    Vec2 pivot{0.5f, 0.5f};
    f32 cosRot{1.0f};
    f32 sinRot{0.0f};
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    TextureHandle texture{kWhiteTexture};
    u16 depth{32768};
    u8 pass{0};
    u8 layer{10};
    u8 view{0};
    BlendMode blend{BlendMode::Alpha};
    bool flipX{false};
    bool flipY{false};
    bool pixelSnap{false};

    constexpr QuadCmd& Rot(f32 rad) noexcept {
        FastSinCos(rad, cosRot, sinRot);
        return *this;
    }
};
static_assert(sizeof(QuadCmd) == 80);

// [Pass:2@60][Layer:8@52][Depth:16@36][Blend:2@34][View:3@30][Tex:14@16][Idx:16]
// Batch mask drops Layer/Depth/Idx -> same Pass/View/Blend/Tex merge.
struct SortKey {
    static constexpr u64 kBatchStateMask = 0xF000000FFFFF0000ULL;

    [[nodiscard]] static constexpr u64 Pack(const QuadCmd& c, u16 textureIndex, u16 quadIndex) noexcept {
        return (static_cast<u64>(c.pass & 0x03U) << 60U) |
               (static_cast<u64>(c.layer) << 52U) |
               (static_cast<u64>(c.depth) << 36U) |
               (static_cast<u64>(static_cast<u8>(c.blend) & 0x03U) << 34U) |
               (static_cast<u64>(c.view & 0x07U) << 30U) |
               (static_cast<u64>(textureIndex & 0x3FFFU) << 16U) |
               static_cast<u64>(quadIndex);
    }

    [[nodiscard]] static constexpr u16 YDepth(f32 y, f32 minY = -500.0f, f32 maxY = 500.0f, i32 bias = 0) noexcept {
        if (maxY <= minY) return 32768U;
        const f32 t = std::clamp((y - minY) / (maxY - minY), 0.0f, 1.0f);
        return static_cast<u16>(std::clamp(static_cast<i32>(t * 65535.0f + 0.5f) + bias, 0, 65535));
    }

    [[nodiscard]] static constexpr u16 Idx(u64 k) noexcept { return static_cast<u16>(k & 0xFFFFU); }
    [[nodiscard]] static constexpr u8 View(u64 k) noexcept { return static_cast<u8>((k >> 30U) & 0x07U); }
    [[nodiscard]] static constexpr BlendMode Blend(u64 k) noexcept {
        return static_cast<BlendMode>((k >> 34U) & 0x03U);
    }
    [[nodiscard]] static constexpr u8 Pass(u64 k) noexcept { return static_cast<u8>((k >> 60U) & 0x03U); }
};

// Center + corner offsets (d0=hx+hy, d1=hx-hy). Plain rgba avoids Color alignas(16).
struct alignas(4) PreparedQuad {
    f32 r{1.0f}, g{1.0f}, b{1.0f}, a{1.0f};
    f32 cx{0.0f}, cy{0.0f};
    f32 d0x{0.0f}, d0y{0.0f};
    f32 d1x{0.0f}, d1y{0.0f};
    f32 u0{0.0f}, v0{0.0f};
    f32 u1{0.0f}, v1{0.0f};
    TextureHandle texture{};
};
static_assert(sizeof(PreparedQuad) == 60);

struct alignas(4) DrawBatch {
    TextureHandle texture{kWhiteTexture};
    u16 first{0};
    u16 count{0};
    u8 pass{0};
    u8 view{0};
    BlendMode blend{BlendMode::Alpha};
};
static_assert(sizeof(DrawBatch) == 12);

struct alignas(4) Camera2D {
    Rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
    Rect scissor{0.0f, 0.0f, 1.0f, 1.0f};
    f32 cx{0.0f};          // world center
    f32 cy{0.0f};
    f32 zoom{1.0f};
    f32 pixelsPerMeter{1.0f};
    bool screenSpace{false};
    bool yUp{false};

    [[nodiscard]] constexpr f32 Scale() const noexcept {
        return std::max(0.001f, zoom * pixelsPerMeter);
    }

    [[nodiscard]] constexpr Vec2 ToView(Vec2 world, Viewport vp) const noexcept {
        if (screenSpace) return world * zoom;
        const f32 s = Scale();
        const f32 vw = viewport.w * static_cast<f32>(vp.w);
        const f32 vh = viewport.h * static_cast<f32>(vp.h);
        return {
            vw * 0.5f + (world.x - cx) * s,
            vh * 0.5f + (world.y - cy) * s * (yUp ? -1.0f : 1.0f),
        };
    }

    [[nodiscard]] constexpr Vec2 ToWorld(Vec2 view, Viewport vp) const noexcept {
        if (screenSpace) return view * (zoom > 1e-5f ? 1.0f / zoom : 1.0f);
        const f32 inv = 1.0f / Scale();
        const f32 vw = viewport.w * static_cast<f32>(vp.w);
        const f32 vh = viewport.h * static_cast<f32>(vp.h);
        const f32 dx = (view.x - vw * 0.5f) * inv;
        const f32 dy = (view.y - vh * 0.5f) * inv;
        return {cx + dx, cy + dy * (yUp ? -1.0f : 1.0f)};
    }
};
static_assert(sizeof(Camera2D) == 52);

struct alignas(4) PassDesc {
    Color clearColor{0.05f, 0.06f, 0.09f, 1.0f};
    u16 target{0};         // texture slot, 0 = backbuffer
    bool clear{true};
    bool enabled{true};
};
static_assert(sizeof(PassDesc) == 20);

struct alignas(4) RenderStats {
    u32 submitted{0};
    u32 culled{0};
    u32 dropped{0};
    u32 batches{0};
};
static_assert(sizeof(RenderStats) == 16);

struct alignas(16) NineSlice {
    Rect rect{};
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 border{8.0f};
    f32 uvBorder{0.25f};
    TextureHandle texture{kWhiteTexture};
    u16 depth{32768};
    BlendMode blend{BlendMode::Alpha};
    bool center{true};
};
static_assert(sizeof(NineSlice) == 64);

} // namespace en
