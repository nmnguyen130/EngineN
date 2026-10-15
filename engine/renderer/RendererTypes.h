#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <span>

namespace en::renderer {

enum class RendererErrc : u8 {
    DeviceInitFailed,
    TextureCreateFailed,
    TextureUpdateFailed,
    InvalidHandle,
    PoolExhausted,
    InvalidDimension
};

// Generational handle for GPU textures managed by Renderer (Asset Sink).
// Slot 0 (generation 1) is permanently bound to the built-in 1x1 opaque white texture.
struct TextureHandle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
    constexpr bool operator==(const TextureHandle&) const noexcept = default;
};

inline constexpr TextureHandle kWhiteTexture{0, 1};

enum class TextureFormat : u8 { RGBA8, BGRA8 };
enum class TextureFilter : u8 { Nearest, Linear };
enum class BlendMode : u8 { AlphaPMA = 0, Additive = 1, Multiply = 2, Opaque = 3 };
enum class PresentMode : u8 { VSync, Immediate, Adaptive };

// Descriptor passed by Asset Manager or procedural generators into Renderer::CreateTexture.
struct TextureDesc {
    u16 width{0};
    u16 height{0};
    TextureFormat format{TextureFormat::RGBA8};
    TextureFilter filter{TextureFilter::Nearest};
    std::span<const u8> pixels{};
    u32 pitch{0};
};

// Premultiplied linear float color matching SDL_FColor binary layout (16 bytes).
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

    constexpr bool operator==(const Color&) const noexcept = default;
};
static_assert(sizeof(Color) == 16);

namespace Colors {
    inline constexpr Color White       = Color::Premultiplied(1.0f, 1.0f, 1.0f, 1.0f);
    inline constexpr Color Black       = Color::Premultiplied(0.0f, 0.0f, 0.0f, 1.0f);
    inline constexpr Color Transparent = Color::Premultiplied(0.0f, 0.0f, 0.0f, 0.0f);
    inline constexpr Color Red         = Color::Premultiplied(1.0f, 0.0f, 0.0f, 1.0f);
    inline constexpr Color Green       = Color::Premultiplied(0.0f, 1.0f, 0.0f, 1.0f);
    inline constexpr Color Blue        = Color::Premultiplied(0.0f, 0.0f, 1.0f, 1.0f);
}

struct FRect {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 w{0.0f};
    f32 h{0.0f};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return w <= 0.0f || h <= 0.0f; }
    constexpr bool operator==(const FRect&) const noexcept = default;
};

// Binary-compatible with SDL_Vertex (32 bytes = 2 vertices per 64-byte cache line).
struct Vertex2D {
    f32 x{0.0f};
    f32 y{0.0f};
    Color color{};
    f32 u{0.0f};
    f32 v{0.0f};
};
static_assert(sizeof(Vertex2D) == 32);

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
        return (static_cast<u64>(cmd.passId & 0x0Fu) << 60) |
               (static_cast<u64>(cmd.layer) << 52) |
               (static_cast<u64>(cmd.depth) << 36) |
               (static_cast<u64>(static_cast<u8>(cmd.blend) & 0x03u) << 34) |
               (static_cast<u64>(cmd.viewSlot & 0x0Fu) << 30) |
               (static_cast<u64>(cmd.texture.index & 0x3FFFu) << 16) |
               static_cast<u64>(quadIdx);
    }

    [[nodiscard]] static constexpr u16 QuadIndex(u64 key) noexcept { return static_cast<u16>(key & 0xFFFFu); }
    [[nodiscard]] static constexpr u16 TextureIndex(u64 key) noexcept { return static_cast<u16>((key >> 16) & 0x3FFFu); }
    [[nodiscard]] static constexpr u8 ViewSlot(u64 key) noexcept { return static_cast<u8>((key >> 30) & 0x0Fu); }
    [[nodiscard]] static constexpr BlendMode Blend(u64 key) noexcept { return static_cast<BlendMode>((key >> 34) & 0x03u); }
    [[nodiscard]] static constexpr u8 PassId(u64 key) noexcept { return static_cast<u8>((key >> 60) & 0x0Fu); }
};

struct DrawBatch {
    u32 firstQuad{0};
    u32 quadCount{0};
    TextureHandle texture{kWhiteTexture};
    u8 passId{0};
    u8 viewSlot{0};
    BlendMode blend{BlendMode::AlphaPMA};
};

struct Camera2D {
    f32 worldCenterX{0.0f};
    f32 worldCenterY{0.0f};
    f32 zoom{1.0f};
    FRect viewport{};
    FRect scissor{};
    bool screenSpace{false};
};

struct RenderPassDesc {
    Color clearColor{Colors::Black};
    bool clear{false};
    bool enabled{true};
};

struct RenderStats {
    u32 submittedQuads{0};
    u32 culledQuads{0};
    u32 droppedQuads{0};
    u32 drawBatches{0};
    u32 radixPassesRun{0};
    bool monotonicFastPath{false};
};

} // namespace en::renderer