#pragma once

#include "core/Math.h"
#include "core/Memory.h"
#include "renderer/RendererTypes.h"

namespace en {

inline constexpr u8 kMaxAssetScopes = 16U;

using AssetId = StringId;

namespace literals {
[[nodiscard]] consteval AssetId operator""_aid(const char* str, usize len) noexcept {
    return AssetId::Hash(std::string_view{str, len});
}
} // namespace literals

struct TextureAssetTag {};
struct SpriteAssetTag {};
struct AtlasAssetTag {};
struct BlobAssetTag {};
struct AudioAssetTag {};

using TextureAssetHandle = Handle<TextureAssetTag>;
using SpriteHandle       = Handle<SpriteAssetTag>;
using AtlasHandle        = Handle<AtlasAssetTag>;
using BlobHandle         = Handle<BlobAssetTag>;
using AudioHandle        = Handle<AudioAssetTag>;

enum class AssetErrc : u8 {
    None           = 0,
    InvalidConfig  = 1,
    OutOfMemory    = 2,
    ScopeExhausted = 3,
    PoolExhausted  = 4,
    DecodeFailed   = 5,
};

enum class AssetKind : u8 {
    None    = 0,
    Texture = 1,
    Atlas   = 2,
    Blob    = 3,
    Audio   = 4,
};

// 40-byte sprite atlas cell descriptor (natural 4-byte alignment, no artificial 64B padding).
struct SpriteFrame {
    Rect          uv{0.0f, 0.0f, 1.0f, 1.0f};
    f32           width{1.0f};
    f32           height{1.0f};
    Vec2          pivot{0.5f, 0.5f};
    TextureHandle gpuTexture{kWhiteTexture};
    u16           generation{1};
    u16           pad{0};
};
static_assert(sizeof(SpriteFrame) == 40);

struct TextureRecord {
    const u8*     pixels{nullptr};
    TextureHandle gpu{kWhiteTexture};
    u16           width{1};
    u16           height{1};
};
static_assert(sizeof(TextureRecord) == 16);

struct AtlasRecord {
    u16 firstSprite{0};
    u16 spriteCount{0};
    u16 texMasterSlot{0};
    u16 pad{0};
};
static_assert(sizeof(AtlasRecord) == 8);

struct BlobRecord {
    const u8* data{nullptr};
    u32       size{0};
    u32       pad{0};

    [[nodiscard]] constexpr std::span<const u8> Bytes() const noexcept {
        return (data && size > 0U) ? std::span<const u8>{data, size} : std::span<const u8>{};
    }
};
static_assert(sizeof(BlobRecord) == 16);

struct AudioRecord {
    const f32* pcm{nullptr};
    u32        frameCount{0};
    u16        sampleRate{48000};
    u8         channels{2};
    bool       looping{false};

    [[nodiscard]] constexpr u32 TotalSamples() const noexcept {
        return frameCount * static_cast<u32>(channels);
    }
};
static_assert(sizeof(AudioRecord) == 16);

// 16-byte direct-indexed slot metadata: packs nextFree[0..12] and AssetKind[13..15] into 16 bits.
struct SlotMeta {
    u64 assetId{0};
    u16 scopeMask{0};
    u16 lastUsedFrame{0};
    u16 generation{1};
    u16 nextAndKind{0};

    [[nodiscard]] constexpr u16 NextFree() const noexcept {
        return static_cast<u16>(nextAndKind & 0x1FFFU);
    }
    [[nodiscard]] constexpr AssetKind Kind() const noexcept {
        return static_cast<AssetKind>((nextAndKind >> 13U) & 0x7U);
    }
    constexpr void SetNextFree(u16 next) noexcept {
        nextAndKind = static_cast<u16>((nextAndKind & 0xE000U) | (next & 0x1FFFU));
    }
    constexpr void SetKind(AssetKind k) noexcept {
        nextAndKind = static_cast<u16>((nextAndKind & 0x1FFFU) | (static_cast<u16>(k) << 13U));
    }
};
static_assert(sizeof(SlotMeta) == 16);

struct AssetBankConfig {
    usize arenaBytes{8U * 1024U * 1024U};
    u32   vramBudgetBytes{64U * 1024U * 1024U};
    u16   maxSlots{1024};
    u16   maxSprites{2048};
    u16   maxTextures{256};
    u16   maxAtlases{128};
    u16   maxBlobs{256};
    u16   maxAudio{128};
};
static_assert(sizeof(AssetBankConfig) == 24);

} // namespace en
