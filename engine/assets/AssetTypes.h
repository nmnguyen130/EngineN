#pragma once

#include "renderer/RendererTypes.h"

namespace en {

// 8-byte case-insensitive, slash-normalized FNV-1a asset identifier.
// Value type with implicit StringId interoperability and zero dynamic allocation.
// Invariant: value == 0 represents a null/unassigned asset identifier.
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

inline constexpr u8 kInvalidScope = 0xFFU;
inline constexpr usize kMaxAssetScopes = 32U;
inline constexpr u16 kNullAssetSlot = 0xFFFFU;

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

// 4-byte generational handle into AssetBank's contiguous SpriteFrame table.
struct alignas(4) SpriteHandle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const SpriteHandle&) const noexcept = default;
};
static_assert(sizeof(SpriteHandle) == 4);

// 4-byte generational handle into AssetBank's TextureRecord table.
struct alignas(4) TextureAssetHandle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const TextureAssetHandle&) const noexcept = default;
};
static_assert(sizeof(TextureAssetHandle) == 4);

// 4-byte generational handle into AssetBank's AtlasRecord table.
struct alignas(4) AtlasHandle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const AtlasHandle&) const noexcept = default;
};
static_assert(sizeof(AtlasHandle) == 4);

// 4-byte generational handle into AssetBank's BlobRecord table.
struct alignas(4) BlobHandle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const BlobHandle&) const noexcept = default;
};
static_assert(sizeof(BlobHandle) == 4);

// 4-byte generational handle into AssetBank's AudioRecord table.
struct alignas(4) AudioHandle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const AudioHandle&) const noexcept = default;
};
static_assert(sizeof(AudioHandle) == 4);

// 2-byte normalized [0.0f .. 1.0f] UNORM16 pivot coordinate allowing SpriteFrame and CookedAtlasSpriteEntry to fit in 32B.
struct alignas(2) UnitPivotU16 {
    u16 raw{32768U};

    constexpr UnitPivotU16() noexcept = default;
    constexpr UnitPivotU16(f32 v) noexcept
        : raw(static_cast<u16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f)) {}

    constexpr UnitPivotU16& operator=(f32 v) noexcept {
        raw = static_cast<u16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
        return *this;
    }

    [[nodiscard]] constexpr operator f32() const noexcept {
        return static_cast<f32>(raw) * (1.0f / 65535.0f);
    }

    [[nodiscard]] constexpr bool operator==(const UnitPivotU16&) const noexcept = default;
};
static_assert(sizeof(UnitPivotU16) == 2);

// 32-byte (2 per 64B cache line, zero padding) hot sprite frame record read at 240 Hz during render extraction.
struct alignas(32) SpriteFrame {
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    f32 width{1.0f};
    f32 height{1.0f};
    UnitPivotU16 pivotX{0.5f};
    UnitPivotU16 pivotY{0.5f};
    TextureHandle gpuTexture{kWhiteTexture};

    [[nodiscard]] constexpr Vec2 Pivot() const noexcept {
        return {static_cast<f32>(pivotX), static_cast<f32>(pivotY)};
    }
};
static_assert(sizeof(SpriteFrame) == 32);

// 16-byte (4 per 64B cache line, zero padding) texture record.
// While unoccupied on the intrusive freelist, width stores nextFree and height stores the retired ABA generation.
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

// 8-byte (8 per 64B cache line, zero padding) sprite atlas record with embedded ABA generation.
// While unoccupied on the intrusive freelist, textureSlot stores nextFree and firstSprite stores the retired generation.
struct alignas(8) AtlasRecord {
    u16 firstSprite{0};
    u16 spriteCount{0};
    u16 textureSlot{0};
    u16 generation{0};
};
static_assert(sizeof(AtlasRecord) == 8);

// 16-byte (4 per 64B cache line, zero padding) binary blob record with embedded ABA generation and hot-reload version.
// While unoccupied on the intrusive freelist, size stores nextFree and version stores the retired ABA generation.
struct alignas(16) BlobRecord {
    const u8* data{nullptr};
    u32 size{0};
    u16 generation{0};
    u16 version{0};

    [[nodiscard]] constexpr std::span<const u8> Bytes() const noexcept {
        return data ? std::span<const u8>(data, size) : std::span<const u8>{};
    }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return data != nullptr && size > 0U; }
};
static_assert(sizeof(BlobRecord) == 16);

// 16-byte (4 per 64B cache line, zero padding) decoded PCM audio clip record.
// While unoccupied on the intrusive freelist, sampleRate stores nextFree and frameCount stores the retired ABA generation.
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

// 16-byte (4 per 64B cache line, zero padding) master asset slot metadata.
// Dual-use lastUsedFrame: while state == AssetState::Loading, stores the 32-bit simScopeMask of scopes
// waiting on this slot; once Ready/Failed, stores the last access frame index for LRU VRAM eviction.
// While state == AssetState::Unloaded on the freelist, payloadSlot stores the nextFree master slot index.
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

// 16-byte binary header for cooked .enpak virtual archive files.
struct alignas(16) EnPakHeader {
    u32 magic{kEnPakMagic};
    u32 version{1};
    u32 entryCount{0};
    u32 headerCrc32{0};
};
static_assert(sizeof(EnPakHeader) == 16);

// 32-byte (2 per 64B cache line, zero padding) table-of-contents entry inside an .enpak archive.
// For AssetKind::Atlas entries, BackingTexture() / SetBackingTexture() pack a 64-bit dependency AssetId
// across {schemaOrMeta, widthOrRate, heightOrChannels} so SceneDirector::Pin() auto-resolves the atlas texture.
struct alignas(8) EnPakEntry {
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

    constexpr void SetBackingTexture(AssetId texId) noexcept {
        schemaOrMeta = static_cast<u32>(texId.value & 0xFFFFFFFFULL);
        widthOrRate = static_cast<u16>((texId.value >> 32U) & 0xFFFFULL);
        heightOrChannels = static_cast<u16>((texId.value >> 48U) & 0xFFFFULL);
    }

    [[nodiscard]] constexpr AssetId BackingTexture() const noexcept {
        const u64 raw = static_cast<u64>(schemaOrMeta) |
                        (static_cast<u64>(widthOrRate) << 32U) |
                        (static_cast<u64>(heightOrChannels) << 48U);
        return AssetId{raw};
    }
};
static_assert(sizeof(EnPakEntry) == 32);

// 32-byte (2 per 64B cache line, zero padding) cooked sprite frame descriptor inside an atlas payload.
struct alignas(8) CookedAtlasSpriteEntry {
    f32 u0{0.0f};
    f32 v0{0.0f};
    f32 u1{1.0f};
    f32 v1{1.0f};
    f32 width{1.0f};
    f32 height{1.0f};
    UnitPivotU16 pivotX{0.5f};
    UnitPivotU16 pivotY{0.5f};
    u32 nameHash{0};
};
static_assert(sizeof(CookedAtlasSpriteEntry) == 32);

} // namespace en
