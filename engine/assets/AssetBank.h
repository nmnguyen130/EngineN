#pragma once

#include "assets/AssetTypes.h"

#include <memory>
#include <span>
#include <string_view>

namespace en::assets {

// Pure C++23 data-oriented asset storage, Robin Hood hash index, LZ4 codec, and scope lifetime engine.
// Owns pre-sized SoA tables and a contiguous blob arena; zero OS or SDL3 dependencies.
// Invariant: Slot 0 of every typed table is permanently initialized to a valid fallback record.
class AssetBank {
public:
    AssetBank() = default;
    ~AssetBank() noexcept = default;

    AssetBank(const AssetBank&) = delete;
    AssetBank& operator=(const AssetBank&) = delete;
    AssetBank(AssetBank&&) noexcept = default;
    AssetBank& operator=(AssetBank&&) noexcept = default;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetBankConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<ScopeId, AssetErrc> BeginScope() noexcept;
    void EndScope(ScopeId scope) noexcept;
    void AdvanceFrame(u32 frameIndex) noexcept { m_currentFrame = frameIndex; }

    [[nodiscard]] Result<u16, AssetErrc> AcquireSlot(
        AssetId id,
        AssetKind kind,
        ScopeId scope,
        Residency residency,
        bool& outNeedsLoad) noexcept;

    [[nodiscard]] u16 FindSlot(AssetId id) const noexcept;

    [[nodiscard]] constexpr TextureAssetHandle ToTextureHandle(u16 slot) const noexcept {
        return (slot < m_slotCap && m_slots[slot].kind == AssetKind::Texture)
                   ? TextureAssetHandle{m_slots[slot].payloadSlot, m_slots[slot].generation}
                   : TextureAssetHandle{};
    }

    [[nodiscard]] constexpr AtlasHandle ToAtlasHandle(u16 slot) const noexcept {
        return (slot < m_slotCap && m_slots[slot].kind == AssetKind::Atlas)
                   ? AtlasHandle{m_slots[slot].payloadSlot, m_slots[slot].generation}
                   : AtlasHandle{};
    }

    [[nodiscard]] constexpr BlobHandle ToBlobHandle(u16 slot) const noexcept {
        return (slot < m_slotCap && m_slots[slot].kind == AssetKind::Blob)
                   ? BlobHandle{m_slots[slot].payloadSlot, m_slots[slot].generation}
                   : BlobHandle{};
    }

    [[nodiscard]] constexpr AudioHandle ToAudioHandle(u16 slot) const noexcept {
        return (slot < m_slotCap && m_slots[slot].kind == AssetKind::Audio)
                   ? AudioHandle{m_slots[slot].payloadSlot, m_slots[slot].generation}
                   : AudioHandle{};
    }

    [[nodiscard]] constexpr const SpriteFrame& GetSprite(SpriteHandle h) const noexcept {
        const u32 idx = (h.spriteIndex < m_spriteCount && m_spriteGens[h.spriteIndex] == h.generation)
                            ? h.spriteIndex
                            : 0u;
        return m_sprites[idx];
    }

    [[nodiscard]] constexpr const SpriteFrame& GetAtlasFrame(AtlasHandle h, u16 frameOffset) const noexcept {
        const u32 aIdx = (h.index < m_atlasCap && m_atlasGens[h.index] == h.generation) ? h.index : 0u;
        const AtlasRecord& rec = m_atlases[aIdx];
        const u32 clamped = (frameOffset < rec.spriteCount) ? frameOffset : 0u;
        return m_sprites[rec.firstSprite + clamped];
    }

    [[nodiscard]] constexpr SpriteHandle GetSpriteHandle(AtlasHandle h, u16 frameOffset) const noexcept {
        const u32 aIdx = (h.index < m_atlasCap && m_atlasGens[h.index] == h.generation) ? h.index : 0u;
        const AtlasRecord& rec = m_atlases[aIdx];
        const u16 clamped = (frameOffset < rec.spriteCount) ? frameOffset : 0u;
        return SpriteHandle{static_cast<u16>(rec.firstSprite + clamped), m_atlasGens[aIdx]};
    }

    [[nodiscard]] constexpr const TextureRecord& GetTexture(TextureAssetHandle h) const noexcept {
        const u32 idx = (h.index < m_texCap && m_texGens[h.index] == h.generation) ? h.index : 0u;
        return m_textures[idx];
    }

    [[nodiscard]] constexpr const BlobRecord& GetBlob(BlobHandle h) const noexcept {
        const u32 idx = (h.index < m_blobCap && m_blobGens[h.index] == h.generation) ? h.index : 0u;
        return m_blobs[idx];
    }

    [[nodiscard]] constexpr const AudioRecord& GetAudio(AudioHandle h) const noexcept {
        const u32 idx = (h.index < m_audioCap && m_audioGens[h.index] == h.generation) ? h.index : 0u;
        return m_audio[idx];
    }

    [[nodiscard]] u16 FindNamedFrame(AtlasHandle h, u32 nameHash) const noexcept;

    Result<void, AssetErrc> CommitTexture(
        u16 slot,
        renderer::TextureHandle gpuHandle,
        u16 width,
        u16 height,
        u32 vramBytes) noexcept;

    Result<void, AssetErrc> CommitAtlas(
        u16 slot,
        u16 backingTextureSlot,
        std::span<const u8> rawPayload) noexcept;

    Result<void, AssetErrc> CommitBlob(
        u16 slot,
        std::span<const u8> rawPayload,
        u32 schemaHash,
        bool isMemoryMapped) noexcept;

    Result<void, AssetErrc> CommitAudio(
        u16 slot,
        std::span<const u8> rawPayload,
        u16 sampleRate,
        u8 channels,
        bool isMemoryMapped) noexcept;

    void MarkFailed(u16 slot) noexcept;

    // Collects unpinned TextureHandles exceeding VRAM budget and grace period for GPU destruction.
    [[nodiscard]] u32 EvictUnpinned(std::span<renderer::TextureHandle> outDestroyedTextures) noexcept;

    [[nodiscard]] bool IsScopeSimReady(ScopeId scope) const noexcept;
    [[nodiscard]] u64 CurrentVramBytes() const noexcept { return m_currentVramBytes; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    [[nodiscard]] static Result<u32, AssetErrc> DecompressLz4(
        std::span<const u8> src,
        std::span<u8> dst) noexcept;

private:
    struct alignas(16) HashSlot {
        u64 key{0};
        u16 slotIndex{0};
        u16 psl{0};
        u32 reserved{0};
    };

    struct AtlasCold {
        u32 firstNamedFrame{0};
        u16 namedFrameCount{0};
        u16 masterSlot{0};
    };

    void InsertHash(u64 key, u16 slotIndex) noexcept;
    void RemoveHash(u64 key) noexcept;

    AssetBankConfig m_cfg{};
    u32 m_activeScopesMask{0};
    u32 m_currentFrame{0};
    u64 m_currentVramBytes{0};

    u16 m_slotCap{0};
    u16 m_freeSlotTop{0};
    std::unique_ptr<SlotMeta[]> m_slots;
    std::unique_ptr<AssetId[]> m_slotIds;
    std::unique_ptr<u8[]> m_slotResidency;
    std::unique_ptr<u16[]> m_freeSlots;

    u32 m_hashCap{0};
    u32 m_hashMask{0};
    std::unique_ptr<HashSlot[]> m_hashTable;

    u16 m_texCap{0};
    u16 m_freeTexTop{0};
    std::unique_ptr<TextureRecord[]> m_textures;
    std::unique_ptr<u16[]> m_texGens;
    std::unique_ptr<u32[]> m_texVramBytes;
    std::unique_ptr<u16[]> m_texMasterSlot;
    std::unique_ptr<u16[]> m_freeTex;

    u16 m_atlasCap{0};
    u16 m_freeAtlasTop{0};
    std::unique_ptr<AtlasRecord[]> m_atlases;
    std::unique_ptr<u16[]> m_atlasGens;
    std::unique_ptr<AtlasCold[]> m_atlasCold;
    std::unique_ptr<u16[]> m_freeAtlases;

    u32 m_spriteCap{0};
    u32 m_spriteCount{1};
    std::unique_ptr<SpriteFrame[]> m_sprites;
    std::unique_ptr<u16[]> m_spriteGens;

    u32 m_namedFrameCap{0};
    u32 m_namedFrameCount{0};
    std::unique_ptr<CookedFrameName[]> m_namedFrames;

    u16 m_blobCap{0};
    u16 m_freeBlobTop{0};
    std::unique_ptr<BlobRecord[]> m_blobs;
    std::unique_ptr<u16[]> m_blobGens;
    std::unique_ptr<u16[]> m_freeBlobs;

    u16 m_audioCap{0};
    u16 m_freeAudioTop{0};
    std::unique_ptr<AudioRecord[]> m_audio;
    std::unique_ptr<u16[]> m_audioGens;
    std::unique_ptr<u16[]> m_freeAudio;

    u32 m_blobArenaCap{0};
    u32 m_blobArenaHead{0};
    std::unique_ptr<u8[]> m_blobArena;
};

} // namespace en::assets