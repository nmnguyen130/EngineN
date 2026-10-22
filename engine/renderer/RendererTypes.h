#pragma once

#include "core/Math.h"

namespace en {

struct TextureTag {};
using TextureHandle = Handle<TextureTag>;

inline constexpr TextureHandle kWhiteTexture{0, 1};
inline constexpr TextureHandle kBuiltinFontTexture{1, 1};
inline constexpr TextureHandle kBuiltinSoftLightTexture{2, 1};

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

// 8-byte pixel viewport extent decoupled from platform/Window.h via structural template conversion.
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

// 32-byte texture and offscreen render-target creation descriptor with zero compiler padding.
struct alignas(8) TextureDesc {
    std::span<const u8> pixels{};
    u32 width{0};
    u32 height{0};
    u32 pitch{0};
    TextureFormat format{TextureFormat::RGBA8};
    TextureFilter filter{TextureFilter::Nearest};
    bool renderTarget{false};
    bool premultipliedAlpha{true};
};
static_assert(sizeof(TextureDesc) == 32);

// 32-byte (2 per cache line, zero padding) GPU vertex layout binary-identical to SDL_Vertex.
struct alignas(16) Vertex2D {
    f32 x{0.0f};
    f32 y{0.0f};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 u{0.0f};
    f32 v{0.0f};
};
static_assert(sizeof(Vertex2D) == 32);

// 80-byte (5x16B, zero padding) sprite/primitive quad submission command supporting arbitrary pivot, Y-sort depth bias, and hit-flash.
struct alignas(16) QuadCmd {
    Vec2 center{0.0f, 0.0f};
    Vec2 size{1.0f, 1.0f};
    Vec2 pivot{0.5f, 0.5f};
    f32 cosRot{1.0f};
    f32 sinRot{0.0f};
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    TextureHandle texture{kWhiteTexture};
    u16 depth{32768};
    i16 depthBias{0};
    u8 passId{0};
    u8 layer{10};
    u8 viewSlot{0};
    BlendMode blend{BlendMode::Alpha};
    bool flipX{false};
    bool flipY{false};
    bool pixelSnap{false};
    u8 flash8{0};

    constexpr QuadCmd& SetRotation(f32 angleRad) noexcept {
        FastSinCos(angleRad, cosRot, sinRot);
        return *this;
    }

    constexpr QuadCmd& SetHitFlash(f32 ratio) noexcept {
        flash8 = static_cast<u8>(std::clamp(ratio, 0.0f, 1.0f) * 255.0f + 0.5f);
        return *this;
    }
};
static_assert(sizeof(QuadCmd) == 80);

// 64-bit radix sort key: [Pass:4][Layer:8][Depth:16][Blend:2][View:4][TextureIdx:14][QuadIdx:16].
// Separates kSortStateMask (includes Layer and Depth for back-to-front ordering) from kBatchStateMask
// (Pass, Blend, View, TextureIdx) so sorted Y-depth sprites on the same atlas coalesce into 1 DrawBatch.
struct SortKey {
    static constexpr u64 kSortStateMask  = 0xFFFFFFFFFFFF0000ULL;
    static constexpr u64 kBatchStateMask = 0xF000000FFFFF0000ULL;

    [[nodiscard]] static constexpr u64 Pack(const QuadCmd& cmd, u16 quadIdx) noexcept {
        const i32 biasedDepth = std::clamp(
            static_cast<i32>(cmd.depth) + static_cast<i32>(cmd.depthBias), 0, 65535);
        const u64 pass = static_cast<u64>(cmd.passId & 0x0FU) << 60U;
        const u64 layer = static_cast<u64>(cmd.layer) << 52U;
        const u64 depth = static_cast<u64>(static_cast<u16>(biasedDepth)) << 36U;
        const u64 blend = static_cast<u64>(static_cast<u8>(cmd.blend) & 0x03U) << 34U;
        const u64 view = static_cast<u64>(cmd.viewSlot & 0x0FU) << 30U;
        const u64 tex = static_cast<u64>(cmd.texture.index & 0x3FFFU) << 16U;
        return pass | layer | depth | blend | view | tex | static_cast<u64>(quadIdx);
    }

    // Quantizes a continuous 2D world Y coordinate into a 16-bit depth key [0..65535] for top-down Y-sorting.
    [[nodiscard]] static constexpr u16 EncodeYDepth(
        f32 worldY, f32 minY = -500.0f, f32 maxY = 500.0f, i32 zBias = 0) noexcept {
        if (maxY <= minY) return 32768U;
        const f32 t = std::clamp((worldY - minY) / (maxY - minY), 0.0f, 1.0f);
        const i32 base = static_cast<i32>(t * 65535.0f + 0.5f) + zBias;
        return static_cast<u16>(std::clamp(base, 0, 65535));
    }

    [[nodiscard]] static constexpr u16 QuadIndex(u64 key) noexcept { return static_cast<u16>(key & 0xFFFFU); }
    [[nodiscard]] static constexpr u16 TextureIndex(u64 key) noexcept { return static_cast<u16>((key >> 16U) & 0x3FFFU); }
    [[nodiscard]] static constexpr u8 ViewSlot(u64 key) noexcept { return static_cast<u8>((key >> 30U) & 0x0FU); }
    [[nodiscard]] static constexpr BlendMode Blend(u64 key) noexcept { return static_cast<BlendMode>((key >> 34U) & 0x03U); }
    [[nodiscard]] static constexpr u16 Depth(u64 key) noexcept { return static_cast<u16>((key >> 36U) & 0xFFFFU); }
    [[nodiscard]] static constexpr u8 Layer(u64 key) noexcept { return static_cast<u8>((key >> 52U) & 0xFFU); }
    [[nodiscard]] static constexpr u8 PassId(u64 key) noexcept { return static_cast<u8>((key >> 60U) & 0x0FU); }
};

// 64-byte (1 L1 cache line exact, zero padding) pre-transformed quad with pre-summed corner offset vectors.
// Value type; stores d0 = hx + hy, d1 = hx - hy, and packed secondary RGBA8 gradient color c1Rgba8.
struct alignas(64) PreparedQuad {
    static constexpr u16 kGradActiveBit     = 0x8000U;
    static constexpr u16 kGradHorizontalBit = 0x4000U;
    static constexpr u16 kTexIndexMask      = 0x3FFFU;

    Color color{};
    f32 cx{0.0f};
    f32 cy{0.0f};
    f32 d0x{0.5f};
    f32 d0y{0.5f};
    f32 d1x{0.5f};
    f32 d1y{-0.5f};
    f32 u0{0.0f};
    f32 v0{0.0f};
    f32 u1{1.0f};
    f32 v1{1.0f};
    TextureHandle texture{kWhiteTexture};
    u32 c1Rgba8{0};
};
static_assert(sizeof(PreparedQuad) == 64);

// 16-byte (4 per cache line, zero padding) coalesced draw call range sharing identical pipeline state.
struct alignas(16) DrawBatch {
    u32 firstQuad{0};
    u32 quadCount{0};
    TextureHandle texture{kWhiteTexture};
    u8 passId{0};
    u8 viewSlot{0};
    BlendMode blend{BlendMode::Alpha};
    u8 layer{0};
};
static_assert(sizeof(DrawBatch) == 16);

// 2-byte Q8.8 fixed-point pixels-per-meter scale allowing Camera2D to fit in 48 bytes with zero padding.
struct alignas(2) UnitScaleQ8 {
    u16 raw{256};

    constexpr UnitScaleQ8() noexcept = default;
    constexpr UnitScaleQ8(f32 v) noexcept
        : raw(static_cast<u16>(std::clamp(v * 256.0f + 0.5f, 1.0f, 65535.0f))) {}

    constexpr UnitScaleQ8& operator=(f32 v) noexcept {
        raw = static_cast<u16>(std::clamp(v * 256.0f + 0.5f, 1.0f, 65535.0f));
        return *this;
    }

    [[nodiscard]] constexpr operator f32() const noexcept {
        return static_cast<f32>(raw) * (1.0f / 256.0f);
    }

    [[nodiscard]] constexpr bool operator==(const UnitScaleQ8&) const noexcept = default;
};
static_assert(sizeof(UnitScaleQ8) == 2);

// 48-byte (3x16B, zero padding) 2D view camera with normalized viewport, scissor clip rect, and trauma shake.
struct alignas(16) Camera2D {
    f32 worldCenterX{0.0f};
    f32 worldCenterY{0.0f};
    f32 zoom{1.0f};
    UnitScaleQ8 pixelsPerMeter{1.0f};
    bool screenSpace{false};
    bool yUp{false};
    Rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
    Rect scissor{0.0f, 0.0f, 1.0f, 1.0f};

    [[nodiscard]] constexpr f32 EffectiveScale() const noexcept {
        return std::max(0.001f, zoom * static_cast<f32>(pixelsPerMeter));
    }

    // Applies deterministic quadratic camera trauma shake (amplitude = trauma^2) to worldCenterX/Y.
    constexpr void ApplyTraumaShake(f32 trauma, f32 timeSec, f32 maxWorldOffset = 0.45f) noexcept {
        const f32 t = std::clamp(trauma, 0.0f, 1.0f);
        const f32 mag = t * t * maxWorldOffset;
        if (mag <= 1e-5f) return;
        f32 cx = 1.0f, sx = 0.0f, cy = 1.0f, sy = 0.0f;
        FastSinCos(timeSec * 43.0f, cx, sx);
        FastSinCos(timeSec * 69.5f + 1.3f, cy, sy);
        worldCenterX += sx * mag;
        worldCenterY += cy * mag;
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
            const f32 invZ = zoom > 1e-5f ? (1.0f / zoom) : 1.0f;
            return vpLocalPos * invZ;
        }
        const f32 vpW = viewport.w * static_cast<f32>(vp.w);
        const f32 vpH = viewport.h * static_cast<f32>(vp.h);
        const f32 invS = 1.0f / EffectiveScale();
        const f32 dx = (vpLocalPos.x - vpW * 0.5f) * invS;
        const f32 dy = (vpLocalPos.y - vpH * 0.5f) * invS;
        return {worldCenterX + dx, worldCenterY + (yUp ? -dy : dy)};
    }
};
static_assert(sizeof(Camera2D) == 48);

// 20-byte (zero padding; 16 passes = 320B = 5 cache lines exact) render pass configuration.
// Supports offscreen render targets via targetTexIndex (0 = default window backbuffer).
struct alignas(4) RenderPassDesc {
    Color clearColor{0.05f, 0.06f, 0.09f, 1.0f};
    u16 targetTexIndex{0};
    bool clear{true};
    bool enabled{true};
};
static_assert(sizeof(RenderPassDesc) == 20);

// 20-byte (zero padding, zero duplicate counters) per-frame batcher and radix sort telemetry.
struct alignas(4) RenderStats {
    u32 submittedQuads{0};
    u32 culledQuads{0};
    u32 droppedQuads{0};
    u32 drawBatches{0};
    u16 activeClipViews{0};
    u8 radixPassesRun{0};
    bool monotonicFastPath{true};
};
static_assert(sizeof(RenderStats) == 20);

// 64-byte (1 L1 cache line exact, zero padding) 9-slice UI panel and bordered sprite descriptor.
struct alignas(64) NineSliceDesc {
    Rect outerRect{};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 borderLeft{8.0f};
    f32 borderRight{8.0f};
    f32 borderTop{8.0f};
    f32 borderBottom{8.0f};
    f32 uvBorder{0.25f};
    f32 cornerScale{1.0f};
    TextureHandle texture{kWhiteTexture};
    u16 depth{32768};
    BlendMode blend{BlendMode::Alpha};
    bool drawCenter{true};
};
static_assert(sizeof(NineSliceDesc) == 64);

struct alignas(16) ShadowOccluder2D {
    Vec2 a{};
    Vec2 b{};
};
static_assert(sizeof(ShadowOccluder2D) == 16);

} // namespace en
