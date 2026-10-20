#pragma once

#include "assets/AssetTypes.h"
#include "platform/Window.h"

namespace en {

struct alignas(8) ViewportExtent {
    i32 w{1280};
    i32 h{720};

    constexpr ViewportExtent() noexcept = default;
    constexpr ViewportExtent(i32 width, i32 height) noexcept : w(width), h(height) {}
    constexpr ViewportExtent(Extent2i ext) noexcept : w(ext.w), h(ext.h) {}

    [[nodiscard]] constexpr bool IsValid() const noexcept { return w > 0 && h > 0; }
    [[nodiscard]] constexpr f32 AspectRatio() const noexcept {
        return h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 1.0f;
    }
};
static_assert(sizeof(ViewportExtent) == 8);

struct RenderTargetTag {};
using RenderTargetHandle = Handle<RenderTargetTag>;

inline constexpr GpuTextureHandle kBuiltinFontTexture{1, 1};
inline constexpr GpuTextureHandle kBuiltinSoftLightTexture{2, 1};

enum class BlendMode : u8 {
    Alpha = 0,
    Additive = 1,
    Multiply = 2,
    Opaque = 3,
};

struct alignas(16) ShaderParamBlock {
    Vec4 customUniforms{0.0f, 0.0f, 0.0f, 0.0f};
    f32 emissiveBoost{0.0f};
    f32 dissolveThreshold{0.0f};
    u8 shaderProgram{0};
    BlendMode blend{BlendMode::Alpha};
    u16 flags{0};
    u32 materialTag{0};
};
static_assert(sizeof(ShaderParamBlock) == 32);

struct alignas(16) Vertex2D {
    Color color{};
    f32 x{0.0f};
    f32 y{0.0f};
    f32 u{0.0f};
    f32 v{0.0f};
};
static_assert(sizeof(Vertex2D) == 32);

struct QuadCmd {
    Vec2 center{};
    Vec2 size{1.0f, 1.0f};
    f32 cosRot{1.0f};
    f32 sinRot{0.0f};
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    GpuTextureHandle texture{kWhiteGpuTexture};
    u16 depth{32768};
    u8 passId{0};
    u8 layer{10};
    u8 viewSlot{0};
    u8 materialId{0};
    BlendMode blend{BlendMode::Alpha};
    bool flipX{false};
    bool flipY{false};
};

// 64-bit radix sort key: [Pass:4][Layer:8][Depth:16][Blend:2][View:4][Texture:14][QuadIdx:16]
struct SortKey {
    static constexpr u64 kBatchStateMask = 0xFFFFFFFFFFFF0000ULL;

    [[nodiscard]] static constexpr u64 Pack(const QuadCmd& cmd, u16 quadIdx) noexcept {
        const u64 pass = static_cast<u64>(cmd.passId & 0x0FU) << 60U;
        const u64 layer = static_cast<u64>(cmd.layer) << 52U;
        const u64 depth = static_cast<u64>(cmd.depth) << 36U;
        const u64 blend = static_cast<u64>(static_cast<u8>(cmd.blend) & 0x03U) << 34U;
        const u64 view = static_cast<u64>(cmd.viewSlot & 0x0FU) << 30U;
        const u64 tex = static_cast<u64>(cmd.texture.index & 0x3FFFU) << 16U;
        return pass | layer | depth | blend | view | tex | static_cast<u64>(quadIdx);
    }

    [[nodiscard]] static constexpr u16 QuadIndex(u64 key) noexcept { return static_cast<u16>(key & 0xFFFFU); }
    [[nodiscard]] static constexpr u16 TextureIndex(u64 key) noexcept { return static_cast<u16>((key >> 16U) & 0x3FFFU); }
    [[nodiscard]] static constexpr u8 ViewSlot(u64 key) noexcept { return static_cast<u8>((key >> 30U) & 0x0FU); }
    [[nodiscard]] static constexpr BlendMode Blend(u64 key) noexcept { return static_cast<BlendMode>((key >> 34U) & 0x03U); }
    [[nodiscard]] static constexpr u8 PassId(u64 key) noexcept { return static_cast<u8>((key >> 60U) & 0x0FU); }
};

struct alignas(64) PreparedQuad {
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
    GpuTextureHandle texture{kWhiteGpuTexture};
    u32 flags{0};
};
static_assert(sizeof(PreparedQuad) == 64);

struct alignas(16) DrawBatch {
    u32 firstQuad{0};
    u32 quadCount{0};
    GpuTextureHandle texture{kWhiteGpuTexture};
    u8 passId{0};
    u8 viewSlot{0};
    BlendMode blend{BlendMode::Alpha};
    u8 flags{0};
};
static_assert(sizeof(DrawBatch) == 16);

struct alignas(8) Camera2D {
    Rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
    Rect scissor{0.0f, 0.0f, 1.0f, 1.0f};
    f32 worldCenterX{0.0f};
    f32 worldCenterY{0.0f};
    f32 zoom{1.0f};
    f32 pixelsPerMeter{48.0f};
    bool screenSpace{false};
    bool active{true};
    u16 viewportId{0};
};
static_assert(sizeof(Camera2D) == 64);

struct RenderPassDesc {
    Color clearColor{0.05f, 0.06f, 0.09f, 1.0f};
    RenderTargetHandle targetFbo{};
    bool clear{true};
    bool enabled{true};
};

struct RenderStats {
    u32 submittedQuads{0};
    u32 culledQuads{0};
    u32 droppedQuads{0};
    u32 drawBatches{0};
    u8 radixPassesRun{0};
    bool monotonicFastPath{false};
    u16 activeLights{0};
};
static_assert(sizeof(RenderStats) == 20);

struct NineSliceDesc {
    Rect outerRect{};
    f32 borderLeft{8.0f};
    f32 borderRight{8.0f};
    f32 borderTop{8.0f};
    f32 borderBottom{8.0f};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    GpuTextureHandle texture{kWhiteGpuTexture};
};

struct PostProcessConfig {
    Color ambientTint{0.18f, 0.20f, 0.28f, 1.0f};
    Color screenFlashColor{1.0f, 1.0f, 1.0f, 0.0f};
    f32 bloomIntensity{0.35f};
    f32 vignetteStrength{0.25f};
    f32 chromaticAberrationPx{0.0f};
    bool crtScanlines{false};
};

struct ShadowOccluder2D {
    Vec2 a{};
    Vec2 b{};
};

} // namespace en
