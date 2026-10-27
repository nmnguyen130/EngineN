#pragma once

#include "renderer/RendererTypes.h"

namespace en {

// Case-insensitive, slash-normalized FNV-1a asset identifier interoperable with StringId.
// Value type; value == 0 represents an unassigned asset identifier.
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

    [[nodiscard]] constexpr Vec2 Pivot() const noexcept { return pivot; }
};
static_assert(sizeof(SpriteFrame) == 40);

// Texture record (16B); while unoccupied on the freelist, width stores nextFree.
struct alignas(16) TextureRecord {
    const u8* pixels{nullptr};
    TextureHandle gpu{kWhiteTexture};
    u16 width{1};
    u16 height{1};

    [[nodiscard]] constexpr std::span<const u8> PixelBytes() const noexcept {
        const usize count = static_cast<usize>(width) * static_cast<usize>(height) * 4U;
        return pixels ? std::span<const u8>(pixels, count) : std::span<const u8>{};
    }
};
static_assert(sizeof(TextureRecord) == 16);

// Sprite atlas record (8B); while unoccupied on the freelist, firstSprite stores nextFree.
struct alignas(8) AtlasRecord {
    u16 firstSprite{0};
    u16 spriteCount{0};
    u16 texMasterSlot{0};
    u16 reserved{0};
};
static_assert(sizeof(AtlasRecord) == 8);

// Binary blob record (16B); while unoccupied on the freelist, size stores nextFree.
struct alignas(16) BlobRecord {
    const u8* data{nullptr};
    u32 size{0};
    u32 reserved{0};

    [[nodiscard]] constexpr std::span<const u8> Bytes() const noexcept {
        return data ? std::span<const u8>(data, size) : std::span<const u8>{};
    }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return data != nullptr && size > 0U; }
};
static_assert(sizeof(BlobRecord) == 16);

// Decoded PCM audio clip record (16B); while unoccupied on the freelist, sampleRate stores nextFree.
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
        return pcm ? std::span<const f32>(pcm, TotalSamples()) : std::span<const f32>{};
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept { return pcm != nullptr && frameCount > 0U; }
};
static_assert(sizeof(AudioRecord) == 16);

// Master asset slot metadata (16B, 4 slots per cache line).
// When unoccupied on the freelist, assetId == 0 and payloadSlot stores nextFree (0 == end of list).
struct alignas(16) SlotMeta {
    u64 assetId{0};
    u16 scopeMask{0};
    u16 lastUsedFrame{0};
    u16 generation{0};
    u16 payloadSlot : 13 {0};
    u16 kind        : 3  {0};

    [[nodiscard]] constexpr AssetKind Kind() const noexcept {
        return static_cast<AssetKind>(kind);
    }
};
static_assert(sizeof(SlotMeta) == 16);

} // namespace en
