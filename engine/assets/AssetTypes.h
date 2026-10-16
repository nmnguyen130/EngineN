#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "renderer/RendererTypes.h"

#include <span>
#include <string_view>

namespace en::assets {

enum class AssetErrc : u8 {
    NotFound = 1,
    PoolExhausted,
    ScopeExhausted,
    CorruptedHeader,
    CorruptedPayload,
    HashMismatch,
    IoFailure,
    StagingOverflow,
    DeviceUploadFailed,
    InvalidMount
};

enum class AssetKind : u8 {
    None = 0,
    Texture,
    Atlas,
    Blob,
    Audio
};

enum class AssetState : u8 {
    Empty = 0,
    Loading,
    Ready,
    Failed
};

enum class AssetCodec : u8 {
    Raw = 0,
    Lz4
};

enum class Residency : u8 {
    Presentation = 0,
    Sim
};

// Normalized 64-bit FNV-1a path identifier.
// Invariant: backslashes are converted to forward slashes and ASCII uppercase is folded to lowercase.
struct AssetId {
    u64 value{0};

    constexpr bool operator==(const AssetId&) const noexcept = default;
    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }

    [[nodiscard]] static constexpr AssetId Hash(std::string_view path) noexcept {
        if (path.empty()) {
            return {};
        }
        u64 h = 14695981039346656037ULL;
        for (char raw : path) {
            const char c = (raw == '\\') ? '/' : ((raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw + 32) : raw);
            h = (h ^ static_cast<u8>(c)) * 1099511628211ULL;
        }
        return AssetId{h};
    }
};

[[nodiscard]] consteval AssetId operator""_aid(const char* str, std::size_t len) noexcept {
    return AssetId::Hash(std::string_view{str, len});
}

template <typename Tag>
struct Handle {
    u16 index{0};
    u16 generation{0};

    constexpr bool operator==(const Handle&) const noexcept = default;
    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
};

using TextureAssetHandle = Handle<struct TextureAssetTag>;
using AtlasHandle        = Handle<struct AtlasAssetTag>;
using BlobHandle         = Handle<struct BlobAssetTag>;
using AudioHandle        = Handle<struct AudioAssetTag>;

// Direct 1-hop handle into AssetBank's flat SpriteFrame array.
struct SpriteHandle {
    u16 spriteIndex{0};
    u16 generation{0};

    constexpr bool operator==(const SpriteHandle&) const noexcept = default;
    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
};

using ScopeId = u8;
inline constexpr ScopeId kInvalidScope = 0xFF;
inline constexpr u32 kMaxScopes        = 32;

// Cache-line half-sized (32B) hot sprite record consumed directly by RenderList extraction.
// Packs generation into the trailing 4 bytes so SpriteHandle validation requires zero extra arrays.
struct alignas(32) SpriteFrame {
    renderer::FRect uv{0.0f, 0.0f, 1.0f, 1.0f};
    f32 width{1.0f};
    f32 height{1.0f};
    i16 pivotX{0};
    i16 pivotY{0};
    renderer::TextureHandle gpuTexture{renderer::kWhiteTexture};
};
static_assert(sizeof(SpriteFrame) == 32 && alignof(SpriteFrame) == 32);

struct TextureRecord {
    renderer::TextureHandle gpu{renderer::kWhiteTexture};
    u16 width{1};
    u16 height{1};
};

struct AtlasRecord {
    u16 firstSprite{0};
    u16 spriteCount{1};
    u16 textureSlot{0};
    u16 reserved{0};
};

struct alignas(16) BlobRecord {
    const u8* data{nullptr};
    u32 size{0};
    u32 schemaHash{0};
};

struct alignas(16) AudioRecord {
    const i16* pcm{nullptr};
    u32 frameCount{0};
    u16 sampleRate{48000};
    u8 channels{2};
    u8 reserved{0};
};

// Unified 16-byte slot control metadata with 32-bit scope bitmask pinning.
struct alignas(16) SlotMeta {
    u32 scopeMask{0};
    u32 lastUsedFrame{0};
    u16 generation{1};
    u16 version{1};
    u16 payloadSlot{0};
    AssetKind kind{AssetKind::None};
    AssetState state{AssetState::Empty};
};
static_assert(sizeof(SlotMeta) == 16);

inline constexpr u32 kPakMagic   = 0x4B504E45u; // 'ENPK'
inline constexpr u32 kPakVersion = 1u;

struct alignas(32) PakHeader {
    u32 magic{kPakMagic};
    u32 version{kPakVersion};
    u32 entryCount{0};
    u32 flags{0};
    u64 tocOffset{0};
    u64 contentHash{0};
};
static_assert(sizeof(PakHeader) == 32);

struct alignas(32) PakEntry {
    u64 assetId{0};
    u64 offset{0};
    u32 storedSize{0};
    u32 rawSize{0};
    u32 metaA{0}; // Texture: (height << 16) | width; Blob: schemaHash; Audio: sampleRate; Atlas: (nameCount << 16) | spriteCount
    u16 metaB{0}; // Texture: (filter << 8) | format; Audio: channels
    AssetKind kind{AssetKind::None};
    AssetCodec codec{AssetCodec::Raw};
};
static_assert(sizeof(PakEntry) == 32);

struct CookedAtlasHeader {
    u64 backingTextureId{0};
    u16 spriteCount{0};
    u16 namedFrameCount{0};
    u32 reserved{0};
};
static_assert(sizeof(CookedAtlasHeader) == 16);

struct CookedFrameName {
    u32 nameHash{0};
    u16 frameOffset{0};
    u16 reserved{0};
};
static_assert(sizeof(CookedFrameName) == 8);

struct AssetBankConfig {
    u16 maxSlots{4096};
    u16 maxTextures{1024};
    u16 maxAtlases{512};
    u16 maxBlobs{1024};
    u16 maxAudio{1024};
    u32 maxSprites{16384};
    u32 maxNamedFrames{16384};
    u32 blobArenaBytes{16 * 1024 * 1024};
    u64 vramBudgetBytes{256ULL * 1024 * 1024};
    u32 evictGraceFrames{600};
};

} // namespace en::assets