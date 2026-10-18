#pragma once

#include "assets/AssetTypes.h"

#include <memory>
#include <span>

namespace en::assets {

// Pure C++23 data-oriented asset storage, Robin Hood hash index, LZ4 codec, and scope lifetime engine.
// Owns a single 64-byte aligned arena slab carving all SoA tables and blob storage; zero OS or SDL3 dependencies.
// Invariant: Slot 0 of every typed table is permanently initialized to a valid fallback record.
class alignas(64) AssetBank {
public:
    AssetBank() = default;
    ~AssetBank() noexcept = default;

    AssetBank(const AssetBank&) = delete;
    AssetBank& operator=(const AssetBank&) = delete;
    AssetBank(AssetBank&&) noexcept = default;
    AssetBank& operator=(AssetBank&&) noexcept = default;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetBankConfig& cfg = {}) noexcept;
    void Shutdown() noexcept { *this = AssetBank{}; }

    [[nodiscard]] Result<ScopeId, AssetErrc> BeginScope() noexcept;
    void EndScope(ScopeId scope) noexcept;
    constexpr void AdvanceFrame(u32 frameIndex) noexcept { m_currentFrame = frameIndex; }

    [[nodiscard]] Result<u16, AssetErrc> AcquireSlot(
        AssetId id,
        AssetKind kind,
        ScopeId scope,
        Residency residency,
        bool& outNeedsLoad) noexcept;

    [[nodiscard]] u16 FindSlot(AssetId id) const noexcept;

    [[nodiscard]] constexpr TextureAssetHandle ToTextureHandle(u16 slot) const noexcept {
        return ToHandle<TextureAssetHandle>(slot, AssetKind::Texture);
    }
    [[nodiscard]] constexpr AtlasHandle ToAtlasHandle(u16 slot) const noexcept {
        return ToHandle<AtlasHandle>(slot, AssetKind::Atlas);
    }
    [[nodiscard]] constexpr BlobHandle ToBlobHandle(u16 slot) const noexcept {
        return ToHandle<BlobHandle>(slot, AssetKind::Blob);
    }
    [[nodiscard]] constexpr AudioHandle ToAudioHandle(u16 slot) const noexcept {
        return ToHandle<AudioHandle>(slot, AssetKind::Audio);
    }

    [[nodiscard]] constexpr const SpriteFrame& GetSprite(SpriteHandle h) const noexcept {
        const u32 idx = (h.spriteIndex < m_spriteCount && m_spriteGens[h.spriteIndex] == h.generation)
                            ? h.spriteIndex
                            : 0u;
        return m_sprites[idx];
    }

    [[nodiscard]] constexpr const SpriteFrame& GetAtlasFrame(AtlasHandle h, u16 frameOffset) const noexcept {
        const u32 aIdx = (h.index < m_highWaterAtlas && m_atlases[h.index].generation == h.generation)
                             ? h.index
                             : 0u;
        const AtlasRecord& rec = m_atlases[aIdx];
        const u32 clamped = (frameOffset < rec.spriteCount) ? frameOffset : 0u;
        return m_sprites[rec.firstSprite + clamped];
    }

    [[nodiscard]] constexpr SpriteHandle GetSpriteHandle(AtlasHandle h, u16 frameOffset) const noexcept {
        const u32 aIdx = (h.index < m_highWaterAtlas && m_atlases[h.index].generation == h.generation)
                             ? h.index
                             : 0u;
        const AtlasRecord& rec = m_atlases[aIdx];
        const u16 clamped = (frameOffset < rec.spriteCount) ? frameOffset : 0u;
        return SpriteHandle{static_cast<u16>(rec.firstSprite + clamped), rec.generation};
    }

    [[nodiscard]] constexpr const TextureRecord& GetTexture(TextureAssetHandle h) const noexcept {
        const u16 slot = (h.index < m_highWaterTex) ? m_texMasterSlot[h.index] : 0u;
        const u32 idx  = (slot != 0 && m_slots[slot].generation == h.generation) ? h.index : 0u;
        return m_textures[idx];
    }

    [[nodiscard]] constexpr const BlobRecord& GetBlob(BlobHandle h) const noexcept {
        const u32 idx = (h.index < m_highWaterBlob && h.generation == 1u) ? h.index : 0u;
        return m_blobs[idx];
    }

    [[nodiscard]] constexpr const AudioRecord& GetAudio(AudioHandle h) const noexcept {
        const u32 idx = (h.index < m_highWaterAudio && h.generation == 1u) ? h.index : 0u;
        return m_audio[idx];
    }

    [[nodiscard]] u16 FindNamedFrame(AtlasHandle h, u32 nameHash) const noexcept;

    [[nodiscard]] Result<std::span<u8>, AssetErrc> AllocateArenaSlice(u32 sizeBytes) noexcept;
    constexpr void RollbackArenaSlice(u32 prevHead) noexcept { m_blobArenaHead = prevHead; }
    [[nodiscard]] constexpr u32 BlobArenaHead() const noexcept { return m_blobArenaHead; }

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

    [[nodiscard]] u32 EvictUnpinned(std::span<renderer::TextureHandle> outDestroyedTextures) noexcept;

    [[nodiscard]] constexpr bool IsScopeSimReady(ScopeId scope) const noexcept {
        return scope >= kMaxScopes || m_scopeSimPending[scope] == 0;
    }
    [[nodiscard]] constexpr bool IsOverVramBudget() const noexcept { return m_currentVramBytes > m_cfg.vramBudgetBytes; }
    [[nodiscard]] constexpr u64 CurrentVramBytes() const noexcept { return m_currentVramBytes; }
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
    static_assert(sizeof(HashSlot) == 16 && alignof(HashSlot) == 16);

    struct alignas(16) AtlasCold {
        u32 firstNamedFrame{0};
        u16 namedFrameCount{0};
        u16 allocNamedCap{0};
        u16 allocFirstSprite{0};
        u16 allocSpriteCap{0};
        u32 reserved{0};
    };
    static_assert(sizeof(AtlasCold) == 16 && alignof(AtlasCold) == 16);

    template <typename H>
    [[nodiscard]] constexpr H ToHandle(u16 slot, AssetKind expected) const noexcept {
        return (slot < m_highWaterSlot && m_slots[slot].kind == expected)
                   ? H{m_slots[slot].payloadSlot, m_slots[slot].generation}
                   : H{};
    }

    constexpr void CompleteSlot(SlotMeta& meta, AssetState finalState) noexcept {
        if (meta.state == AssetState::Loading) {
            for (u32 mask = meta.lastUsedFrame; mask != 0; mask &= (mask - 1u)) {
                const u32 s = static_cast<u32>(std::countr_zero(mask));
                if (m_scopeSimPending[s] > 0) {
                    --m_scopeSimPending[s];
                }
            }
            meta.lastUsedFrame = m_currentFrame;
        }
        meta.state = finalState;
        if (finalState == AssetState::Ready) {
            ++meta.version;
        }
    }

    void InsertHash(u64 key, u16 slotIndex) noexcept;
    void RemoveHash(u64 key) noexcept;
    [[nodiscard]] Result<const u8*, AssetErrc> ResolveArenaPayload(
        std::span<const u8> rawPayload,
        bool isMemoryMapped) noexcept;

    // Cache Line 0 (offset 0..63): Hot 120Hz/240Hz read-only table pointers and active high-water bounds.
    SpriteFrame* m_sprites{nullptr};
    u16* m_spriteGens{nullptr};
    AtlasRecord* m_atlases{nullptr};
    TextureRecord* m_textures{nullptr};
    BlobRecord* m_blobs{nullptr};
    AudioRecord* m_audio{nullptr};
    u32 m_spriteCount{1};
    u32 m_activeScopesMask{0};
    u16 m_highWaterAtlas{1};
    u16 m_highWaterTex{1};
    u16 m_highWaterBlob{1};
    u16 m_highWaterAudio{1};

    // Cache Line 1 (offset 64..127): Slot metadata, Robin Hood hash index, and cold tables.
    SlotMeta* m_slots{nullptr};
    AssetId* m_slotIds{nullptr};
    HashSlot* m_hashTable{nullptr};
    u16* m_texMasterSlot{nullptr};
    u32* m_texVramBytes{nullptr};
    AtlasCold* m_atlasCold{nullptr};
    CookedFrameName* m_namedFrames{nullptr};
    u8* m_blobArena{nullptr};

    // Cache Line 2 (offset 128..191): O(1) per-scope Sim-residency pending counters (32 scopes x 2B = 64B).
    u16 m_scopeSimPending[kMaxScopes]{};

    // Cache Line 3+: Intrusive freelist heads, capacities, VRAM telemetry, and single arena slab owner.
    u16 m_highWaterSlot{1};
    u16 m_freeSlotHead{0};
    u16 m_freeTexHead{0};
    u16 m_freeAtlasHead{0};
    u32 m_hashCap{0};
    u32 m_hashMask{0};
    u32 m_namedFrameCount{0};
    u32 m_blobArenaHead{0};
    u32 m_currentFrame{0};
    u64 m_currentVramBytes{0};

    AssetBankConfig m_cfg{};
    std::unique_ptr<u64[]> m_arena;
};

} // namespace en::assets
