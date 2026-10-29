#pragma once

#include "renderer/RendererTypes.h"

namespace en {

// FNV-1a path id (slash-normalized, case-insensitive). 0 = invalid.
struct alignas(8) AssetId {
    u64 value{0};

    constexpr AssetId() noexcept = default;
    constexpr explicit AssetId(u64 raw) noexcept : value(raw) {}
    constexpr AssetId(StringId sid) noexcept : value(sid.value) {}

    [[nodiscard]] constexpr operator StringId() const noexcept { return StringId{value}; }
    [[nodiscard]] static constexpr AssetId Hash(std::string_view str) noexcept {
        return AssetId{StringId::Hash(str).value};
    }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0ULL; }
    [[nodiscard]] constexpr auto operator<=>(const AssetId&) const noexcept = default;
};
static_assert(sizeof(AssetId) == 8);

inline namespace literals {
[[nodiscard]] consteval AssetId operator""_aid(const char* str, usize len) noexcept {
    return AssetId::Hash(std::string_view{str, len});
}
} // namespace literals

inline constexpr usize kMaxAssetScopes = 16U;

enum class AssetErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    ScopeExhausted,
    PoolExhausted,
};

enum class AssetKind : u8 {
    None = 0,
    Texture = 1,
    Atlas = 2,
    Blob = 3,
    Audio = 4,
};

struct SpriteTag {};
struct TextureAssetTag {};
struct AtlasTag {};
struct BlobTag {};
struct AudioTag {};

using SpriteHandle       = Handle<SpriteTag>;
using TextureAssetHandle = Handle<TextureAssetTag>;
using AtlasHandle        = Handle<AtlasTag>;
using BlobHandle         = Handle<BlobTag>;
using AudioHandle        = Handle<AudioTag>;

struct alignas(8) SpriteFrame {
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    f32 width{1.0f};
    f32 height{1.0f};
    Vec2 pivot{0.5f, 0.5f};
    TextureHandle gpuTexture{kWhiteTexture};
    u16 generation{0};
    u16 pad{0};
};
static_assert(sizeof(SpriteFrame) == 40);

// Free: width stores nextFree.
struct alignas(16) TextureRecord {
    const u8* pixels{nullptr};
    TextureHandle gpu{kWhiteTexture};
    u16 width{1};
    u16 height{1};

    [[nodiscard]] constexpr std::span<const u8> PixelBytes() const noexcept {
        if (!pixels || width == 0U || height == 0U) return {};
        const usize n = static_cast<usize>(width) * static_cast<usize>(height) * 4U;
        return {pixels, n};
    }
};
static_assert(sizeof(TextureRecord) == 16);

// Free: firstSprite stores nextFree.
struct alignas(8) AtlasRecord {
    u16 firstSprite{0};
    u16 spriteCount{0};
    u16 texMasterSlot{0};
    u16 reserved{0};
};
static_assert(sizeof(AtlasRecord) == 8);

// Free: size stores nextFree.
struct alignas(16) BlobRecord {
    const u8* data{nullptr};
    u32 size{0};
    u32 reserved{0};

    [[nodiscard]] constexpr std::span<const u8> Bytes() const noexcept {
        return (data && size > 0U) ? std::span<const u8>{data, size} : std::span<const u8>{};
    }
};
static_assert(sizeof(BlobRecord) == 16);

// Free: sampleRate stores nextFree.
struct alignas(16) AudioRecord {
    const f32* pcm{nullptr};
    u32 frameCount{0};
    u16 sampleRate{48000};
    u8 channels{2};
    bool looping{false};

    [[nodiscard]] constexpr u32 TotalSamples() const noexcept {
        return frameCount * static_cast<u32>(channels);
    }
    [[nodiscard]] constexpr std::span<const f32> Samples() const noexcept {
        return (pcm && frameCount > 0U) ? std::span<const f32>{pcm, TotalSamples()}
                                        : std::span<const f32>{};
    }
};
static_assert(sizeof(AudioRecord) == 16);

// Free: assetId==0, payloadAndKind = nextFree.
// Live: low 13 = payload index, high 3 = AssetKind.
struct alignas(16) SlotMeta {
    u64 assetId{0};
    u16 scopeMask{0};
    u16 lastUsedFrame{0};
    u16 generation{0};
    u16 payloadAndKind{0};

    [[nodiscard]] constexpr u16 Payload() const noexcept {
        return static_cast<u16>(payloadAndKind & 0x1FFFU);
    }
    [[nodiscard]] constexpr AssetKind Kind() const noexcept {
        return static_cast<AssetKind>((payloadAndKind >> 13U) & 0x7U);
    }
    constexpr void SetPayload(u16 slot) noexcept {
        payloadAndKind = static_cast<u16>((payloadAndKind & 0xE000U) | (slot & 0x1FFFU));
    }
    constexpr void SetKind(AssetKind k) noexcept {
        payloadAndKind = static_cast<u16>((payloadAndKind & 0x1FFFU) |
                                          (static_cast<u16>(k) << 13U));
    }
    constexpr void Set(u16 slot, AssetKind k) noexcept {
        payloadAndKind = static_cast<u16>((slot & 0x1FFFU) | (static_cast<u16>(k) << 13U));
    }
};
static_assert(sizeof(SlotMeta) == 16);

} // namespace en
