#pragma once

#include "core/Math.h"

namespace en {

using AssetId = StringId;

inline namespace literals {
[[nodiscard]] consteval AssetId operator""_aid(const char* str, usize len) noexcept {
    return AssetId::Hash(std::string_view{str, len});
}
} // namespace literals

struct GpuTextureTag {};
struct TextureAssetTag {};
struct AtlasAssetTag {};
struct BlobAssetTag {};
struct AudioAssetTag {};

using GpuTextureHandle = Handle<GpuTextureTag>;
using TextureHandle = Handle<TextureAssetTag>;
using AtlasHandle = Handle<AtlasAssetTag>;
using BlobHandle = Handle<BlobAssetTag>;
using AudioHandle = Handle<AudioAssetTag>;

inline constexpr GpuTextureHandle kWhiteGpuTexture{0, 1};
using ScopeId = u8;
inline constexpr ScopeId kInvalidScope = 0xFFU;
inline constexpr usize kMaxAssetScopes = 32;

enum class AssetErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    ScopeExhausted,
    InvalidScope,
    MountLimitReached,
    CorruptedPackHeader,
    CorruptedPayload,
    AssetNotFound,
    DecompressionFailed,
    PoolExhausted,
};

enum class AssetKind : u8 {
    None = 0,
    Texture = 1,
    Atlas = 2,
    Blob = 3,
    Audio = 4,
};

enum class AssetResidency : u8 {
    Sim = 0,
    Render = 1,
    Stream = 2,
};

enum class AssetState : u8 {
    Unloaded = 0,
    Loading = 1,
    Ready = 2,
    Failed = 3,
};

struct alignas(4) SpriteHandle {
    u16 spriteIndex{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
    [[nodiscard]] constexpr bool operator==(const SpriteHandle&) const noexcept = default;
};
static_assert(sizeof(SpriteHandle) == 4);

struct alignas(32) SpriteFrame {
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    f32 width{1.0f};
    f32 height{1.0f};
    GpuTextureHandle gpuTexture{kWhiteGpuTexture};
    u32 nameHash{0};
};
static_assert(sizeof(SpriteFrame) == 32);

struct alignas(8) TextureRecord {
    std::span<const u8> pixels{};
    GpuTextureHandle gpu{kWhiteGpuTexture};
    u16 width{1};
    u16 height{1};
    u16 generation{0};
    u8 channels{4};
    AssetState state{AssetState::Ready};
};
static_assert(sizeof(TextureRecord) == 28 || sizeof(TextureRecord) == 32);

struct alignas(8) AtlasRecord {
    u16 firstSprite{0};
    u16 spriteCount{0};
    u16 textureSlot{0};
    u16 generation{0};
};
static_assert(sizeof(AtlasRecord) == 8);

struct alignas(8) BlobRecord {
    const u8* data{nullptr};
    u32 size{0};
    u32 schemaHash{0};
    u16 generation{0};
    u16 version{1};
    u32 crc32{0};

    [[nodiscard]] constexpr std::span<const u8> Bytes() const noexcept {
        return data ? std::span<const u8>(data, size) : std::span<const u8>{};
    }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return data != nullptr && size > 0; }
};
static_assert(sizeof(BlobRecord) == 24);

struct alignas(8) AudioRecord {
    const f32* pcm{nullptr};
    u32 frameCount{0};
    u32 sampleRate{48000};
    u16 generation{0};
    u8 channels{2};
    bool looping{false};
    u32 totalSamples{0};

    [[nodiscard]] constexpr std::span<const f32> Samples() const noexcept {
        return pcm ? std::span<const f32>(pcm, totalSamples) : std::span<const f32>{};
    }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return pcm != nullptr && frameCount > 0; }
};
static_assert(sizeof(AudioRecord) == 24);

struct alignas(16) SlotMeta {
    u32 scopeMask{0};
    u32 lastUsedFrame{0};
    u16 generation{0};
    u16 version{0};
    u16 payloadSlot{0};
    AssetKind kind{AssetKind::None};
    AssetState state{AssetState::Unloaded};
};
static_assert(sizeof(SlotMeta) == 16);

inline constexpr u32 kEnPakMagic = 0x4B504E45U; // 'ENPK'

struct alignas(16) EnPakHeader {
    u32 magic{kEnPakMagic};
    u32 version{1};
    u32 entryCount{0};
    u32 headerCrc32{0};
};
static_assert(sizeof(EnPakHeader) == 16);

struct alignas(32) EnPakEntry {
    u64 assetId{0};
    u32 offset{0};
    u32 compressedSize{0};
    u32 uncompressedSize{0};
    u32 schemaOrMeta{0};
    u16 widthOrRate{0};
    u16 heightOrChannels{0};
    AssetKind kind{AssetKind::None};
    u8 compressed{0};
    u16 version{1};
};
static_assert(sizeof(EnPakEntry) == 32);

struct alignas(8) CookedAtlasSpriteEntry {
    f32 u0{0.0f}, v0{0.0f}, u1{1.0f}, v1{1.0f};
    f32 width{1.0f}, height{1.0f};
    f32 pivotX{0.5f}, pivotY{0.5f};
    u32 nameHash{0};
    u32 flags{0};
};
static_assert(sizeof(CookedAtlasSpriteEntry) == 40);

} // namespace en
