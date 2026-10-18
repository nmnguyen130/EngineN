#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "renderer/RendererTypes.h"

#include <bit>
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

[[nodiscard]] constexpr u32 NextPow2(u32 v) noexcept {
    return (v <= 16u) ? 16u : (1u << (32u - std::countl_zero(v - 1u)));
}

[[nodiscard]] constexpr u32 FibonacciHash(u64 key, u32 mask) noexcept {
    return static_cast<u32>((key * 11400714819323198485ULL) >> 32) & mask;
}

[[nodiscard]] constexpr std::size_t Align64(std::size_t bytes) noexcept {
    return (bytes + 63u) & ~static_cast<std::size_t>(63u);
}

template <typename T>
[[nodiscard]] T* CarveAligned64(u8*& cursor, std::size_t count) noexcept {
    if (count == 0) {
        return nullptr;
    }
    auto* ptr = reinterpret_cast<T*>(cursor);
    cursor += Align64(count * sizeof(T));
    return ptr;
}

// Normalized 64-bit FNV-1a path identifier.
// Invariant: backslashes are normalized to forward slashes and ASCII uppercase is folded to lowercase.
struct alignas(8) AssetId {
    u64 value{0};

    constexpr bool operator==(const AssetId&) const noexcept = default;
    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }

    [[nodiscard]] static constexpr AssetId Hash(std::string_view path) noexcept {
        if (path.empty()) {
            return {};
        }
        u64 h = 14695981039346656037ULL;
        for (char ch : path) {
            const u8 raw = static_cast<u8>(ch);
            const u8 norm = (raw == '\\')
                                ? static_cast<u8>('/')
                                : ((raw >= 'A' && raw <= 'Z') ? static_cast<u8>(raw + 32u) : raw);
            h = (h ^ norm) * 1099511628211ULL;
        }
        return AssetId{h};
    }
};
static_assert(sizeof(AssetId) == 8 && alignof(AssetId) == 8);

[[nodiscard]] consteval AssetId operator""_aid(const char* str, std::size_t len) noexcept {
    return AssetId::Hash(std::string_view{str, len});
}

template <typename Tag>
struct alignas(4) Handle {
    u16 index{0};
    u16 generation{0};

    constexpr bool operator==(const Handle&) const noexcept = default;
    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
};

using TextureAssetHandle = Handle<struct TextureAssetTag>;
using AtlasHandle        = Handle<struct AtlasAssetTag>;
using BlobHandle         = Handle<struct BlobAssetTag>;
using AudioHandle        = Handle<struct AudioAssetTag>;
static_assert(sizeof(TextureAssetHandle) == 4 && alignof(TextureAssetHandle) == 4);

// Direct 1-hop generational handle into AssetBank's flat SpriteFrame table.
struct alignas(4) SpriteHandle {
    u16 spriteIndex{0};
    u16 generation{0};

    constexpr bool operator==(const SpriteHandle&) const noexcept = default;
    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
};
static_assert(sizeof(SpriteHandle) == 4 && alignof(SpriteHandle) == 4);

using ScopeId = u8;
inline constexpr ScopeId kInvalidScope = 0xFF;
inline constexpr u32 kMaxScopes        = 32;

// Half-cache-line (32B) hot sprite record consumed directly by WorldExtractor and RenderList.
// Embeds the resolved GPU TextureHandle so GetSprite() requires a single array read with zero indirection.
struct alignas(32) SpriteFrame {
    renderer::FRect uv{0.0f, 0.0f, 1.0f, 1.0f};
    f32 width{1.0f};
    f32 height{1.0f};
    i16 pivotX{0};
    i16 pivotY{0};
    renderer::TextureHandle gpuTexture{renderer::kWhiteTexture};
};
static_assert(sizeof(SpriteFrame) == 32 && alignof(SpriteFrame) == 32);

struct alignas(8) TextureRecord {
    renderer::TextureHandle gpu{renderer::kWhiteTexture};
    u16 width{1};
    u16 height{1};
};
static_assert(sizeof(TextureRecord) == 8 && alignof(TextureRecord) == 8);

// 8-byte hot atlas slice record embedding its generation for single-load validation.
struct alignas(8) AtlasRecord {
    u16 firstSprite{0};
    u16 spriteCount{1};
    u16 textureSlot{0};
    u16 generation{0};
};
static_assert(sizeof(AtlasRecord) == 8 && alignof(AtlasRecord) == 8);

struct alignas(16) BlobRecord {
    const u8* data{nullptr};
    u32 size{0};
    u32 schemaHash{0};
};
static_assert(sizeof(BlobRecord) == 16 && alignof(BlobRecord) == 16);

struct alignas(16) AudioRecord {
    const i16* pcm{nullptr};
    u32 frameCount{0};
    u16 sampleRate{48000};
    u8 channels{2};
    u8 reserved{0};
};
static_assert(sizeof(AudioRecord) == 16 && alignof(AudioRecord) == 16);

// Unified 16-byte slot control metadata with 32-bit scope bitmask pinning.
// Invariant: while state == AssetState::Loading, lastUsedFrame stores the 32-bit Sim-residency scope mask.
struct alignas(16) SlotMeta {
    u32 scopeMask{0};
    u32 lastUsedFrame{0};
    u16 generation{1};
    u16 version{1};
    u16 payloadSlot{0};
    AssetKind kind{AssetKind::None};
    AssetState state{AssetState::Empty};
};
static_assert(sizeof(SlotMeta) == 16 && alignof(SlotMeta) == 16);

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
static_assert(sizeof(PakHeader) == 32 && alignof(PakHeader) == 32);

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
static_assert(sizeof(PakEntry) == 32 && alignof(PakEntry) == 32);

struct alignas(8) CookedAtlasHeader {
    u64 backingTextureId{0};
    u16 spriteCount{0};
    u16 namedFrameCount{0};
    u32 reserved{0};
};
static_assert(sizeof(CookedAtlasHeader) == 16 && alignof(CookedAtlasHeader) == 8);

struct alignas(4) CookedFrameName {
    u32 nameHash{0};
    u16 frameOffset{0};
    u16 reserved{0};
};
static_assert(sizeof(CookedFrameName) == 8 && alignof(CookedFrameName) == 4);

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
