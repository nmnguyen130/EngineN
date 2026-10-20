#pragma once

#include "assets/AssetTypes.h"
#include "core/Containers.h"

namespace en {

struct AssetBankConfig {
    usize arenaBytes{8 * 1024 * 1024};
    usize vramBudgetBytes{64 * 1024 * 1024};
    u16 maxSlots{1024};
    u16 maxSprites{2048};
    u16 maxAtlases{256};
    u16 maxTextures{512};
    u16 maxBlobs{512};
    u16 maxAudio{256};
};

// Pure C++23 data-oriented asset storage, LZ4 codec, 32-scope manager, and LRU VRAM eviction engine.
// Move-only RAII owner of a single 64B-aligned arena slab; zero OS or renderer coupling.
// Invariant: Slot 0 of every resource table holds a valid non-null fallback record.
class alignas(64) AssetBank {
public:
    AssetBank() noexcept = default;
    ~AssetBank() noexcept { Shutdown(); }

    AssetBank(const AssetBank&) = delete;
    AssetBank& operator=(const AssetBank&) = delete;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetBankConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<ScopeId, AssetErrc> BeginScope() noexcept;
    void EndScope(ScopeId scope) noexcept;
    constexpr void AdvanceFrame(u32 frameIndex) noexcept { m_currentFrame = frameIndex; }

    [[nodiscard]] constexpr bool IsScopeSimReady(ScopeId scope) const noexcept {
        return scope < kMaxAssetScopes && m_scopeSimPending[scope] == 0;
    }
    [[nodiscard]] constexpr u32 ActiveScopesMask() const noexcept { return m_activeScopesMask; }

    [[nodiscard]] const SpriteFrame& GetSprite(SpriteHandle h) const noexcept;
    [[nodiscard]] const SpriteFrame& GetAtlasFrame(AtlasHandle h, u16 frameOffset) const noexcept;
    [[nodiscard]] SpriteHandle GetSpriteHandle(AtlasHandle h, u16 frameOffset) const noexcept;
    [[nodiscard]] const TextureRecord& GetTexture(TextureHandle h) const noexcept;
    [[nodiscard]] const BlobRecord& GetBlob(BlobHandle h) const noexcept;
    [[nodiscard]] const AudioRecord& GetAudio(AudioHandle h) const noexcept;

    [[nodiscard]] u16 AcquireSlot(AssetId id, AssetKind kind, ScopeId scope, AssetResidency residency, bool& outNewlyCreated) noexcept;
    [[nodiscard]] std::span<u8> AllocatePayloadSlice(usize bytes) noexcept { return m_arena.Carve<u8>(bytes); }

    TextureHandle CommitTexture(u16 masterSlot, u16 width, u16 height, std::span<const u8> rgbaPixels, GpuTextureHandle gpu = kWhiteGpuTexture) noexcept;
    AtlasHandle CommitAtlas(u16 masterSlot, TextureHandle texHandle, std::span<const CookedAtlasSpriteEntry> entries) noexcept;
    BlobHandle CommitBlob(u16 masterSlot, std::span<const u8> payload, u32 schemaHash = 0) noexcept;
    bool HotReloadBlob(AssetId id, std::span<const u8> newPayload) noexcept;
    AudioHandle CommitAudio(u16 masterSlot, std::span<const f32> pcmSamples, u32 sampleRate = 48000, u8 channels = 2) noexcept;
    void MarkFailed(u16 masterSlot) noexcept;

    [[nodiscard]] u32 EvictUnpinned(std::span<GpuTextureHandle> outDestroyedGpuTextures) noexcept;
    [[nodiscard]] constexpr usize CurrentVramBytes() const noexcept { return m_currentVramBytes; }
    [[nodiscard]] constexpr bool IsOverVramBudget() const noexcept { return m_currentVramBytes > m_vramBudgetBytes; }

    [[nodiscard]] const SlotMeta* FindSlotByAssetId(AssetId id) const noexcept;
    [[nodiscard]] const SlotMeta& GetSlotMeta(u16 slotIdx) const noexcept { return m_slots[slotIdx]; }
    [[nodiscard]] TextureRecord* TryGetMutableTexture(TextureHandle h) noexcept;
    [[nodiscard]] constexpr u16 HighWaterTex() const noexcept { return m_highWaterTex; }
    [[nodiscard]] const TextureRecord& GetTextureBySlot(u16 texSlot) const noexcept {
        return (texSlot < m_highWaterTex) ? m_textures[texSlot] : m_textures[0];
    }
    void BindGpuTexture(u16 texSlot, GpuTextureHandle gpu) noexcept;

    [[nodiscard]] static Result<usize, AssetErrc> DecompressLz4(std::span<const u8> src, std::span<u8> dst) noexcept;
    [[nodiscard]] static Result<usize, AssetErrc> CompressLz4Literal(std::span<const u8> src, std::span<u8> dst) noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    void CompleteSlot(SlotMeta& meta, AssetState finalState) noexcept;

    std::span<SpriteFrame> m_sprites{};
    std::span<u16> m_spriteGens{};
    std::span<AtlasRecord> m_atlases{};
    std::span<TextureRecord> m_textures{};
    std::span<BlobRecord> m_blobs{};
    std::span<AudioRecord> m_audio{};
    std::span<SlotMeta> m_slots{};

    u32 m_activeScopesMask{0};
    u32 m_currentFrame{1};
    u16 m_highWaterSlot{1};
    u16 m_highWaterSprite{1};
    u16 m_highWaterAtlas{1};
    u16 m_highWaterTex{1};
    u16 m_highWaterBlob{1};
    u16 m_highWaterAudio{1};

    std::array<u16, kMaxAssetScopes> m_scopeSimPending{};
    usize m_currentVramBytes{0};
    usize m_vramBudgetBytes{64 * 1024 * 1024};

    RobinHoodMap<u16> m_idToSlot{};
    AlignedArena m_arena{};
};

} // namespace en
