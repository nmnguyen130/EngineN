#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <concepts>
#include <span>
#include <type_traits>

namespace en::renderer {

enum class RendererErrc : u8 {
    DeviceInitFailed,
    TextureCreateFailed,
    TextureUpdateFailed,
    InvalidHandle,
    PoolExhausted,
    InvalidDimension
};

// Lightweight 2D pixel extent that converts implicitly from platform::Extent2i or {w, h}
// so renderer headers remain decoupled from platform headers.
struct alignas(8) ViewportExtent {
    i32 w{0};
    i32 h{0};

    constexpr ViewportExtent() noexcept = default;
    constexpr ViewportExtent(i32 width, i32 height) noexcept : w(width), h(height) {}

    template <typename T>
        requires (!std::same_as<std::remove_cvref_t<T>, ViewportExtent> &&
                  requires(const T& e) { e.w; e.h; })
    constexpr ViewportExtent(const T& extent) noexcept
        : w(static_cast<i32>(extent.w))
        , h(static_cast<i32>(extent.h)) {}

    [[nodiscard]] constexpr bool IsValid() const noexcept { return w > 0 && h > 0; }
    constexpr bool operator==(const ViewportExtent&) const noexcept = default;
};
static_assert(sizeof(ViewportExtent) == 8 && alignof(ViewportExtent) == 8);

// Generational handle for GPU textures managed by Renderer (Asset Sink).
// Slot 0 (generation 1) is permanently bound to the built-in 1x1 opaque white texture.
struct alignas(4) TextureHandle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
    constexpr bool operator==(const TextureHandle&) const noexcept = default;
};
static_assert(sizeof(TextureHandle) == 4 && alignof(TextureHandle) == 4);

inline constexpr TextureHandle kWhiteTexture{0, 1};

enum class TextureFormat : u8 { RGBA8 = 0, BGRA8 = 1 };
enum class TextureFilter : u8 { Nearest = 0, Linear = 1 };
enum class BlendMode : u8 { AlphaPMA = 0, Additive = 1, Multiply = 2, Opaque = 3 };
enum class PresentMode : u8 { VSync = 0, Immediate = 1, Adaptive = 2 };

struct TextureDesc {
    u16 width{0};
    u16 height{0};
    TextureFormat format{TextureFormat::RGBA8};
    TextureFilter filter{TextureFilter::Nearest};
    std::span<const u8> pixels{};
    u32 pitch{0};
};

// Premultiplied linear float color matching SDL_FColor binary layout (16 bytes, 4-byte aligned).
struct Color {
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
        return {
            static_cast<f32>(r) * kInv * fa,
            static_cast<f32>(g) * kInv * fa,
            static_cast<f32>(b) * kInv * fa,
            fa
        };
    }

    [[nodiscard]] static constexpr Color FromRGBA8(u32 rgba8) noexcept {
        return FromRGBA8(
            static_cast<u8>((rgba8 >> 24u) & 0xFFu),
            static_cast<u8>((rgba8 >> 16u) & 0xFFu),
            static_cast<u8>((rgba8 >> 8u) & 0xFFu),
            static_cast<u8>(rgba8 & 0xFFu));
    }

    constexpr bool operator==(const Color&) const noexcept = default;
};
static_assert(sizeof(Color) == 16 && alignof(Color) == 4);

namespace Colors {
    inline constexpr Color White       = Color::Premultiplied(1.0f, 1.0f, 1.0f, 1.0f);
    inline constexpr Color Black       = Color::Premultiplied(0.0f, 0.0f, 0.0f, 1.0f);
    inline constexpr Color Transparent = Color::Premultiplied(0.0f, 0.0f, 0.0f, 0.0f);
    inline constexpr Color Red         = Color::Premultiplied(1.0f, 0.0f, 0.0f, 1.0f);
    inline constexpr Color Green       = Color::Premultiplied(0.0f, 1.0f, 0.0f, 1.0f);
    inline constexpr Color Blue        = Color::Premultiplied(0.0f, 0.0f, 1.0f, 1.0f);
} // namespace Colors

struct FRect {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 w{0.0f};
    f32 h{0.0f};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return w <= 0.0f || h <= 0.0f; }
    constexpr bool operator==(const FRect&) const noexcept = default;
};
static_assert(sizeof(FRect) == 16 && alignof(FRect) == 4);

// Binary-compatible with SDL_Vertex (32 bytes = 2 vertices per 64-byte L1 cache line).
struct alignas(16) Vertex2D {
    f32 x{0.0f};
    f32 y{0.0f};
    Color color{};
    f32 u{0.0f};
    f32 v{0.0f};
};
static_assert(sizeof(Vertex2D) == 32 && alignof(Vertex2D) == 16);

struct QuadCmd {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 w{0.0f};
    f32 h{0.0f};
    f32 cosRot{1.0f};
    f32 sinRot{0.0f};
    FRect uv{0.0f, 0.0f, 1.0f, 1.0f};
    Color color{Colors::White};
    TextureHandle texture{kWhiteTexture};
    u16 depth{32768};
    u8 passId{0};
    u8 layer{0};
    u8 viewSlot{0};
    BlendMode blend{BlendMode::AlphaPMA};
    bool flipX{false};
    bool flipY{false};
};

// 64-bit self-contained sort key embedding the 16-bit quad index in bits [0..15].
// Bit layout: [Pass:4][Layer:8][Depth:16][Blend:2][View:4][TextureIdx:14][QuadIdx:16]
struct SortKey {
    static constexpr u64 kBatchStateMask =
        (0x0FULL << 60) |
        (0x03ULL << 34) |
        (0x0FULL << 30) |
        (0x3FFFULL << 16);

    [[nodiscard]] static constexpr u64 Pack(const QuadCmd& cmd, u16 quadIdx) noexcept {
        const u16 texIdx = cmd.texture.IsValid() ? (cmd.texture.index & 0x3FFFu) : 0u;
        return (static_cast<u64>(cmd.passId & 0x0Fu) << 60) |
               (static_cast<u64>(cmd.layer) << 52) |
               (static_cast<u64>(cmd.depth) << 36) |
               (static_cast<u64>(static_cast<u8>(cmd.blend) & 0x03u) << 34) |
               (static_cast<u64>(cmd.viewSlot & 0x0Fu) << 30) |
               (static_cast<u64>(texIdx) << 16) |
               static_cast<u64>(quadIdx);
    }

    [[nodiscard]] static constexpr u16 QuadIndex(u64 key) noexcept { return static_cast<u16>(key & 0xFFFFu); }
    [[nodiscard]] static constexpr u16 TextureIndex(u64 key) noexcept { return static_cast<u16>((key >> 16) & 0x3FFFu); }
    [[nodiscard]] static constexpr u8 ViewSlot(u64 key) noexcept { return static_cast<u8>((key >> 30) & 0x0Fu); }
    [[nodiscard]] static constexpr BlendMode Blend(u64 key) noexcept { return static_cast<BlendMode>((key >> 34) & 0x03u); }
    [[nodiscard]] static constexpr u8 PassId(u64 key) noexcept { return static_cast<u8>((key >> 60) & 0x0Fu); }
};

struct alignas(16) DrawBatch {
    u32 firstQuad{0};
    u32 quadCount{0};
    TextureHandle texture{kWhiteTexture};
    u8 passId{0};
    u8 viewSlot{0};
    BlendMode blend{BlendMode::AlphaPMA};
    u8 reserved{0};

    constexpr bool operator==(const DrawBatch&) const noexcept = default;
};
static_assert(sizeof(DrawBatch) == 16 && alignof(DrawBatch) == 16);

struct alignas(16) Camera2D {
    f32 worldCenterX{0.0f};
    f32 worldCenterY{0.0f};
    f32 zoom{1.0f};
    FRect viewport{};
    FRect scissor{};
    bool screenSpace{false};
    u8 reserved[3]{0, 0, 0};

    constexpr bool operator==(const Camera2D&) const noexcept = default;
};
static_assert(sizeof(Camera2D) == 48 && alignof(Camera2D) == 16);

struct RenderPassDesc {
    Color clearColor{Colors::Black};
    bool clear{false};
    bool enabled{true};
    u16 reserved{0};

    constexpr bool operator==(const RenderPassDesc&) const noexcept = default;
};
static_assert(sizeof(RenderPassDesc) == 20 && alignof(RenderPassDesc) == 4);

struct RenderStats {
    u32 submittedQuads{0};
    u32 culledQuads{0};
    u32 droppedQuads{0};
    u32 drawBatches{0};
    u32 radixPassesRun{0};
    bool monotonicFastPath{false};
    u8 reserved[3]{0, 0, 0};

    constexpr bool operator==(const RenderStats&) const noexcept = default;
};
static_assert(sizeof(RenderStats) == 24 && alignof(RenderStats) == 4);

} // namespace en::renderer
