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

enum class TextureFormat : u8 {
    RGBA8 = 0,
    BGRA8 = 1,
};

enum class TextureFilter : u8 {
    Nearest = 0,
    Linear = 1,
    PixelArt = 2,
};

enum class BlendMode : u8 {
    Alpha = 0,
    Additive = 1,
    Multiply = 2,
    Opaque = 3,
};

enum class PresentMode : u8 {
    Immediate = 0,
    VSync = 1,
    AdaptiveVSync = 2,
};

// Pixel viewport dimensions decoupled from platform/Window.h via structural conversion.
// Value type; converts implicitly from any {.w, .h} extent such as Extent2i.
// Invariant: IsValid() holds iff w > 0 && h > 0.
struct alignas(8) ViewportExtent {
    i32 w{1280};
    i32 h{720};

    constexpr ViewportExtent() noexcept = default;
    constexpr ViewportExtent(i32 width, i32 height) noexcept : w(width), h(height) {}

    template <typename ExtentT>
        requires (!std::same_as<std::remove_cvref_t<ExtentT>, ViewportExtent>) &&
                 requires(const ExtentT& e) {
                     { e.w } -> std::convertible_to<i32>;
                     { e.h } -> std::convertible_to<i32>;
                 }
    constexpr ViewportExtent(const ExtentT& ext) noexcept
        : w(static_cast<i32>(ext.w)), h(static_cast<i32>(ext.h)) {}

    [[nodiscard]] constexpr bool IsValid() const noexcept { return w > 0 && h > 0; }
    [[nodiscard]] constexpr f32 AspectRatio() const noexcept {
        return h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 1.0f;
    }
    [[nodiscard]] constexpr bool operator==(const ViewportExtent&) const noexcept = default;
};
static_assert(sizeof(ViewportExtent) == 8);

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

// GPU vertex layout binary-compatible with SDL_Vertex.
struct alignas(16) Vertex2D {
    f32 x{0.0f};
    f32 y{0.0f};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
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
    u8 passId{0};
    u8 layer{10};
    u8 viewSlot{0};
    BlendMode blend{BlendMode::Alpha};
    bool flipX{false};
    bool flipY{false};
    bool pixelSnap{false};

    constexpr QuadCmd& SetRotation(f32 angleRad) noexcept {
        FastSinCos(angleRad, cosRot, sinRot);
        return *this;
    }
};
static_assert(sizeof(QuadCmd) == 80);

// 64-bit sort key: [Pass:2][Layer:8][Depth:16][Blend:2][View:3][TextureIdx:14][QuadIdx:16].
// Lower 16 bits hold unique QuadIdx so in-place std::sort is strictly deterministic.
// kBatchStateMask coalesces sorted sprites across different Layer/Depth on the same atlas.
struct SortKey {
    static constexpr u64 kBatchStateMask = 0xF000000FFFFF0000ULL;

    [[nodiscard]] static constexpr u64 Pack(const QuadCmd& cmd, u16 texIdx, u16 quadIdx) noexcept {
        const u64 pass = static_cast<u64>(cmd.passId & 0x03U) << 60U;
        const u64 layer = static_cast<u64>(cmd.layer) << 52U;
        const u64 depth = static_cast<u64>(cmd.depth) << 36U;
        const u64 blend = static_cast<u64>(static_cast<u8>(cmd.blend) & 0x03U) << 34U;
        const u64 view = static_cast<u64>(cmd.viewSlot & 0x07U) << 30U;
        const u64 tex = static_cast<u64>(texIdx & 0x3FFFU) << 16U;
        return pass | layer | depth | blend | view | tex | static_cast<u64>(quadIdx);
    }

    [[nodiscard]] static constexpr u16 EncodeYDepth(
        f32 worldY, f32 minY = -500.0f, f32 maxY = 500.0f, i32 zBias = 0) noexcept {
        if (maxY <= minY) return 32768U;
        const f32 t = std::clamp((worldY - minY) / (maxY - minY), 0.0f, 1.0f);
        const i32 base = static_cast<i32>(t * 65535.0f + 0.5f) + zBias;
        return static_cast<u16>(std::clamp(base, 0, 65535));
    }

    [[nodiscard]] static constexpr u16 QuadIndex(u64 key) noexcept { return static_cast<u16>(key & 0xFFFFU); }
    [[nodiscard]] static constexpr u8 ViewSlot(u64 key) noexcept { return static_cast<u8>((key >> 30U) & 0x07U); }
    [[nodiscard]] static constexpr BlendMode Blend(u64 key) noexcept { return static_cast<BlendMode>((key >> 34U) & 0x03U); }
    [[nodiscard]] static constexpr u8 PassId(u64 key) noexcept { return static_cast<u8>((key >> 60U) & 0x03U); }
};

// Pre-transformed quad storing corner offset vectors (d0 = hx + hy, d1 = hx - hy).
struct alignas(4) PreparedQuad {
    Color color{};
    f32 cx{0.0f};
    f32 cy{0.0f};
    f32 d0x{0.0f};
    f32 d0y{0.0f};
    f32 d1x{0.0f};
    f32 d1y{0.0f};
    f32 u0{0.0f};
    f32 v0{0.0f};
    f32 u1{0.0f};
    f32 v1{0.0f};
    TextureHandle texture{};
};
static_assert(sizeof(PreparedQuad) == 60);

struct alignas(4) DrawBatch {
    TextureHandle texture{kWhiteTexture};
    u16 firstQuad{0};
    u16 quadCount{0};
    u8 passId{0};
    u8 viewSlot{0};
    BlendMode blend{BlendMode::Alpha};
};
static_assert(sizeof(DrawBatch) == 12);

// 2D view camera with normalized viewport and scissor clip rect.
struct alignas(4) Camera2D {
    Rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
    Rect scissor{0.0f, 0.0f, 1.0f, 1.0f};
    f32 worldCenterX{0.0f};
    f32 worldCenterY{0.0f};
    f32 zoom{1.0f};
    f32 pixelsPerMeter{1.0f};
    bool screenSpace{false};
    bool yUp{false};

    [[nodiscard]] constexpr f32 EffectiveScale() const noexcept {
        return std::max(0.001f, zoom * pixelsPerMeter);
    }

    [[nodiscard]] constexpr Vec2 WorldToViewport(Vec2 worldPos, ViewportExtent vp) const noexcept {
        if (screenSpace) return worldPos * zoom;
        const f32 vpW = viewport.w * static_cast<f32>(vp.w);
        const f32 vpH = viewport.h * static_cast<f32>(vp.h);
        const f32 s = EffectiveScale();
        const f32 dx = (worldPos.x - worldCenterX) * s;
        const f32 dy = (worldPos.y - worldCenterY) * s;
        return {vpW * 0.5f + dx, vpH * 0.5f + (yUp ? -dy : dy)};
    }

    [[nodiscard]] constexpr Vec2 ViewportToWorld(Vec2 vpLocalPos, ViewportExtent vp) const noexcept {
        if (screenSpace) {
            return vpLocalPos * (zoom > 1e-5f ? (1.0f / zoom) : 1.0f);
        }
        const f32 vpW = viewport.w * static_cast<f32>(vp.w);
        const f32 vpH = viewport.h * static_cast<f32>(vp.h);
        const f32 invS = 1.0f / EffectiveScale();
        const f32 dx = (vpLocalPos.x - vpW * 0.5f) * invS;
        const f32 dy = (vpLocalPos.y - vpH * 0.5f) * invS;
        return {worldCenterX + dx, worldCenterY + (yUp ? -dy : dy)};
    }
};
static_assert(sizeof(Camera2D) == 52);

struct alignas(4) RenderPassDesc {
    Color clearColor{0.05f, 0.06f, 0.09f, 1.0f};
    u16 targetTexIndex{0};
    bool clear{true};
    bool enabled{true};
};
static_assert(sizeof(RenderPassDesc) == 20);

struct alignas(4) RenderStats {
    u32 submittedQuads{0};
    u32 culledQuads{0};
    u32 droppedQuads{0};
    u32 drawBatches{0};
};
static_assert(sizeof(RenderStats) == 16);

struct alignas(16) NineSliceDesc {
    Rect outerRect{};
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 border{8.0f};
    f32 uvBorder{0.25f};
    TextureHandle texture{kWhiteTexture};
    u16 depth{32768};
    BlendMode blend{BlendMode::Alpha};
    bool drawCenter{true};
};
static_assert(sizeof(NineSliceDesc) == 64);

} // namespace en
