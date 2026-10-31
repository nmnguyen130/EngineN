#pragma once

#include "core/Math.h"

namespace en {

struct TextureTag {};
using TextureHandle = Handle<TextureTag>;
inline constexpr TextureHandle kWhiteTexture{0U, 1U};

enum class BlendMode : u8     { Alpha = 0, Additive = 1, Multiply = 2, Opaque = 3 };
enum class PresentMode : u8   { Immediate = 0, VSync = 1, AdaptiveVSync = 2 };
enum class TextureFilter : u8 { Nearest = 0, Linear = 1, PixelArt = 2 };
enum class TextureFormat : u8 { RGBA8 = 0, BGRA8 = 1 };

enum class RendererErrc : u8 {
    None                = 0,
    DeviceInitFailed    = 1,
    PoolExhausted       = 2,
    InvalidDimension    = 3,
    InvalidHandle       = 4,
    TextureCreateFailed = 5,
    TextureUpdateFailed = 6,
};

struct Viewport {
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

struct TextureDesc {
    std::span<const u8> pixels{};
    u32                 width{1};
    u32                 height{1};
    u32                 pitch{0};
    TextureFilter       filter{TextureFilter::Nearest};
    TextureFormat       format{TextureFormat::RGBA8};
    bool                renderTarget{false};
};
static_assert(sizeof(TextureDesc) == 32);

// Contiguous 32-byte vertex layout matching SDL_RenderGeometryRaw.
struct Vertex2D {
    f32 x{0.0f}, y{0.0f};
    f32 r{1.0f}, g{1.0f}, b{1.0f}, a{1.0f};
    f32 u{0.0f}, v{0.0f};
};
static_assert(sizeof(Vertex2D) == 32);

struct QuadCmd {
    Vec2          center{};
    Vec2          size{1.0f, 1.0f};
    Vec2          pivot{0.5f, 0.5f};
    f32           cosRot{1.0f};
    f32           sinRot{0.0f};
    Rect          uv{0.0f, 0.0f, 1.0f, 1.0f};
    Color         color{1.0f, 1.0f, 1.0f, 1.0f};
    TextureHandle texture{kWhiteTexture};
    u16           depth{32768U};
    u8            pass{0};
    u8            layer{10};
    u8            view{0};
    BlendMode     blend{BlendMode::Alpha};
    bool          flipX{false};
    bool          flipY{false};
    bool          pixelSnap{false};

    constexpr QuadCmd& Rot(f32 rad) noexcept {
        FastSinCos(rad, cosRot, sinRot);
        return *this;
    }
};
static_assert(sizeof(QuadCmd) == 80);

// 64-bit sort key: [61..60]Pass [59..52]Layer [51..36]Depth [35..34]Blend [32..30]View [29..16]TexIdx [15..0]QuadIdx
struct SortKey {
    static constexpr u64 kBatchStateMask = 0xF000000FFFFF0000ULL;

    [[nodiscard]] static constexpr u64 Pack(const QuadCmd& c, u16 texIdx, u16 quadIdx) noexcept {
        return (static_cast<u64>(c.pass & 0x03U) << 60ULL) |
               (static_cast<u64>(c.layer) << 52ULL) |
               (static_cast<u64>(c.depth) << 36ULL) |
               (static_cast<u64>(static_cast<u8>(c.blend) & 0x03U) << 34ULL) |
               (static_cast<u64>(c.view & 0x07U) << 30ULL) |
               (static_cast<u64>(texIdx & 0x3FFFU) << 16ULL) |
               static_cast<u64>(quadIdx);
    }

    [[nodiscard]] static constexpr u16 Idx(u64 k) noexcept  { return static_cast<u16>(k & 0xFFFFULL); }
    [[nodiscard]] static constexpr u8  View(u64 k) noexcept { return static_cast<u8>((k >> 30ULL) & 0x07ULL); }
    [[nodiscard]] static constexpr BlendMode Blend(u64 k) noexcept {
        return static_cast<BlendMode>((k >> 34ULL) & 0x03ULL);
    }
    [[nodiscard]] static constexpr u8  Pass(u64 k) noexcept { return static_cast<u8>((k >> 60ULL) & 0x03ULL); }

    [[nodiscard]] static constexpr u16 YDepth(f32 y, f32 minY = -500.0f, f32 maxY = 500.0f, i32 bias = 0) noexcept {
        if (maxY <= minY) return 32768U;
        const f32 t = std::clamp((y - minY) / (maxY - minY), 0.0f, 1.0f);
        return static_cast<u16>(std::clamp(static_cast<i32>(t * 65535.0f + 0.5f) + bias, 0, 65535));
    }
};

// 60-byte pre-transformed quad with natural 4-byte alignment (zero padding bytes).
struct PreparedQuad {
    f32           r{1.0f}, g{1.0f}, b{1.0f}, a{1.0f};
    f32           cx{0.0f}, cy{0.0f};
    f32           d0x{0.0f}, d0y{0.0f};
    f32           d1x{0.0f}, d1y{0.0f};
    f32           u0{0.0f}, v0{0.0f}, u1{1.0f}, v1{1.0f};
    TextureHandle texture{kWhiteTexture};
};
static_assert(sizeof(PreparedQuad) == 60);

struct DrawBatch {
    TextureHandle texture{kWhiteTexture};
    u16           first{0};
    u16           count{0};
    u8            pass{0};
    u8            view{0};
    BlendMode     blend{BlendMode::Alpha};
};
static_assert(sizeof(DrawBatch) == 12);

struct Camera2D {
    Rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
    Rect scissor{0.0f, 0.0f, 1.0f, 1.0f};
    f32  cx{0.0f};
    f32  cy{0.0f};
    f32  zoom{1.0f};
    f32  pixelsPerMeter{48.0f};
    bool screenSpace{false};
    bool yUp{true};

    [[nodiscard]] constexpr f32 Scale() const noexcept {
        return std::max(0.001f, zoom * pixelsPerMeter);
    }

    [[nodiscard]] constexpr Vec2 ToView(Vec2 world, Viewport vp) const noexcept {
        if (screenSpace) return world * zoom;
        const f32 s  = Scale();
        const f32 vw = viewport.w * static_cast<f32>(vp.w);
        const f32 vh = viewport.h * static_cast<f32>(vp.h);
        return {
            vw * 0.5f + (world.x - cx) * s,
            vh * 0.5f + (world.y - cy) * s * (yUp ? -1.0f : 1.0f),
        };
    }

    [[nodiscard]] constexpr Vec2 ToWorld(Vec2 view, Viewport vp) const noexcept {
        if (screenSpace) return view * (zoom > 1e-5f ? (1.0f / zoom) : 1.0f);
        const f32 inv = 1.0f / Scale();
        const f32 vw  = viewport.w * static_cast<f32>(vp.w);
        const f32 vh  = viewport.h * static_cast<f32>(vp.h);
        return {
            cx + (view.x - vw * 0.5f) * inv,
            cy + (view.y - vh * 0.5f) * inv * (yUp ? -1.0f : 1.0f),
        };
    }
};
static_assert(sizeof(Camera2D) == 52);

struct PassDesc {
    Color clearColor{0.06f, 0.07f, 0.10f, 1.0f};
    u16   target{0};
    bool  clear{true};
    bool  enabled{true};
};
static_assert(sizeof(PassDesc) == 20);

struct RenderStats {
    u32 submitted{0};
    u32 culled{0};
    u32 dropped{0};
    u32 batches{0};
};
static_assert(sizeof(RenderStats) == 16);

struct NineSlice {
    Rect          rect{};
    Rect          uv{0.0f, 0.0f, 1.0f, 1.0f};
    Color         color{1.0f, 1.0f, 1.0f, 1.0f};
    f32           border{4.0f};
    f32           uvBorder{0.25f};
    TextureHandle texture{kWhiteTexture};
    u16           depth{32768U};
    BlendMode     blend{BlendMode::Alpha};
    bool          center{true};
};
static_assert(sizeof(NineSlice) == 64);

// 52-byte 2D lighting and post-processing configuration for ambient multiply darkness, vignette, and hit-flash overlays.
struct PostProcessConfig {
    Color ambientColor{1.0f, 1.0f, 1.0f, 0.0f};
    Color vignetteColor{0.05f, 0.0f, 0.02f, 0.0f};
    Color flashColor{1.0f, 1.0f, 1.0f, 0.0f};
    f32   vignetteThickness{0.18f};
};
static_assert(sizeof(PostProcessConfig) == 52);

} // namespace en
