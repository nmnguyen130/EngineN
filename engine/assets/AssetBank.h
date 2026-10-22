#pragma once

#include "assets/AssetTypes.h"
#include "core/Containers.h"

namespace en {

struct alignas(8) AssetBankConfig {
    usize arenaBytes{8U * 1024U * 1024U};
    u32 vramBudgetBytes{64U * 1024U * 1024U};
    u16 maxSlots{1024};
    u16 maxSprites{2048};
    u16 maxAtlases{256};
    u16 maxTextures{512};
    u16 maxBlobs{512};
    u16 maxAudio{256};
    u32 reserved{0};
};
static_assert(sizeof(AssetBankConfig) == 32);

// Pure C++23 256-byte (4 cache lines exact, zero padding) data-oriented asset storage, coalescing payload sub-allocator, LZ4 codec, 32-scope manager, and LRU VRAM eviction engine.
// Packs all 120 Hz / 240 Hz hot table pointers and high-water bounds into Cache Line 0 (offset 0..63), and m_scopeSimPending[32] into Cache Line 1.
// Invariant: Slot 0 of every resource table holds a valid non-null fallback record so hot-path queries never branch on asset readiness.
class alignas(64) AssetBank {
public:
    // 16-byte intrusive header placed inside freed 64B-aligned payload slices in m_arena.
    struct alignas(16) FreePayloadBlock {
        FreePayloadBlock* next{nullptr};
        u32 byteSize{0};
        u32 reserved{0};
    };
    static_assert(sizeof(FreePayloadBlock) == 16);

    AssetBank() noexcept = default;
    ~AssetBank() noexcept { Shutdown(); }

    AssetBank(const AssetBank&) = delete;
    AssetBank& operator=(const AssetBank&) = delete;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetBankConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<u8, AssetErrc> BeginScope() noexcept;
    void EndScope(u8 scope) noexcept;
    constexpr void AdvanceFrame(u32 frameIndex) noexcept { m_currentFrame = frameIndex; }

    [[nodiscard]] constexpr bool IsScopeSimReady(u8 scope) const noexcept {
        return scope < kMaxAssetScopes && m_scopeSimPending[scope] == 0U;
    }

    [[nodiscard]] constexpr u32 ActiveScopesMask() const noexcept { return m_activeScopesMask; }
    [[nodiscard]] constexpr u32 CurrentFrame() const noexcept { return m_currentFrame; }

    // Branchless-ready O(1) hot-path queries
    [[nodiscard]] constexpr const SpriteFrame& GetSprite(SpriteHandle h) const noexcept {
        return (h.index < m_highWaterSprite && m_spriteGens[h.index] == h.generation)
                   ? m_sprites[h.index]
                   : m_sprites[0];
    }

    [[nodiscard]] constexpr const SpriteFrame& GetAtlasFrame(AtlasHandle h, u16 frameOffset) const noexcept {
        if (h.index < m_highWaterAtlas && m_atlases[h.index].generation == h.generation) {
            const AtlasRecord& rec = m_atlases[h.index];
            if (rec.spriteCount > 0U) {
                const u16 idx = static_cast<u16>(rec.firstSprite + (frameOffset % rec.spriteCount));
                if (idx < m_highWaterSprite) return m_sprites[idx];
            }
        }
        return m_sprites[0];
    }

    [[nodiscard]] constexpr SpriteHandle GetSpriteHandle(AtlasHandle h, u16 frameOffset) const noexcept {
        if (h.index < m_highWaterAtlas && m_atlases[h.index].generation == h.generation) {
            const AtlasRecord& rec = m_atlases[h.index];
            if (rec.spriteCount > 0U) {
                const u16 idx = static_cast<u16>(rec.firstSprite + (frameOffset % rec.spriteCount));
                if (idx < m_highWaterSprite) return SpriteHandle{idx, m_spriteGens[idx]};
            }
        }
        return SpriteHandle{0U, 1U};
    }

    [[nodiscard]] SpriteHandle FindNamedFrame(AtlasHandle h, u32 nameHash) const noexcept;
    [[nodiscard]] SpriteHandle FindNamedFrame(AtlasHandle h, std::string_view name) const noexcept {
        return FindNamedFrame(h, static_cast<u32>(AssetId::Hash(name).value));
    }

    [[nodiscard]] constexpr const TextureRecord& GetTexture(TextureAssetHandle h) const noexcept {
        return (h.index < m_highWaterTex && m_texGens[h.index] == h.generation)
                   ? m_textures[h.index]
                   : m_textures[0];
    }

    [[nodiscard]] constexpr const BlobRecord& GetBlob(BlobHandle h) const noexcept {
        return (h.index < m_highWaterBlob && m_blobs[h.index].generation == h.generation)
                   ? m_blobs[h.index]
                   : m_blobs[0];
    }

    [[nodiscard]] constexpr const AudioRecord& GetAudio(AudioHandle h) const noexcept {
        return (h.index < m_highWaterAudio && m_audioGens[h.index] == h.generation)
                   ? m_audio[h.index]
                   : m_audio[0];
    }

    [[nodiscard]] u16 AcquireSlot(
        AssetId id,
        AssetKind kind,
        u8 scope,
        AssetResidency residency,
        bool& outNeedsLoad) noexcept;

    void TouchAsset(AssetId id) noexcept;

    [[nodiscard]] std::span<u8> AllocatePayloadSlice(usize bytes) noexcept;
    void FreePayloadSlice(const void* ptr, usize bytes) noexcept;

    [[nodiscard]] constexpr u32 SavePayloadMarker() const noexcept { return m_arena.SaveMarker(); }
    void RollbackPayloadMarker(u32 marker) noexcept { m_arena.RollbackToMarker(marker); }

    TextureAssetHandle CommitTexture(
        u16 masterSlot,
        u16 width,
        u16 height,
        std::span<const u8> rgbaPixels,
        TextureHandle gpu = kWhiteTexture,
        bool zeroCopyPixels = false) noexcept;

    AtlasHandle CommitAtlas(
        u16 masterSlot,
        TextureAssetHandle texHandle,
        std::span<const CookedAtlasSpriteEntry> entries) noexcept;

    // Procedural uniform grid sprite-sheet slicer generating cols * rows SpriteFrames without external .enpak tables.
    AtlasHandle CommitGridAtlas(
        u16 masterSlot,
        TextureAssetHandle texHandle,
        u16 cols,
        u16 rows,
        Vec2 pivot = {0.5f, 0.5f}) noexcept;

    BlobHandle CommitBlob(
        u16 masterSlot,
        std::span<const u8> payload,
        bool zeroCopyPayload = false) noexcept;

    bool HotReloadBlob(AssetId id, std::span<const u8> newPayload) noexcept;

    AudioHandle CommitAudio(
        u16 masterSlot,
        std::span<const f32> pcmSamples,
        u32 sampleRate = 48000,
        u8 channels = 2,
        bool looping = false) noexcept;

    void MarkFailed(u16 masterSlot) noexcept;

    // Evicts all unpinned Ready/Failed slots, coalesces freed arena payload slices, and pushes slots onto intrusive freelists.
    [[nodiscard]] u32 EvictUnpinned(std::span<TextureHandle> outDestroyedGpuTextures = {}) noexcept;

    // Evicts least-recently-used (lowest lastUsedFrame) unpinned textures until CurrentVramBytes() <= targetVramBytes.
    [[nodiscard]] u32 EvictLruToVramBudget(
        std::span<TextureHandle> outDestroyedGpuTextures,
        u32 targetVramBytes = 0xFFFFFFFFU) noexcept;

    [[nodiscard]] constexpr u32 CurrentVramBytes() const noexcept { return m_currentVramBytes; }
    [[nodiscard]] constexpr u32 VramBudgetBytes() const noexcept { return m_vramBudgetBytes; }
    [[nodiscard]] constexpr bool IsOverVramBudget() const noexcept { return m_currentVramBytes > m_vramBudgetBytes; }
    [[nodiscard]] constexpr u32 EvictedCount() const noexcept { return m_evictedCount; }
    [[nodiscard]] constexpr u32 HotReloadCount() const noexcept { return m_hotReloadCount; }
    [[nodiscard]] constexpr u16 PendingGpuTextureCount() const noexcept { return m_pendingGpuTexCount; }
    [[nodiscard]] constexpr u32 ArenaUsedBytes() const noexcept { return m_arena.UsedBytes(); }

    [[nodiscard]] const SlotMeta* FindSlotByAssetId(AssetId id) const noexcept;
    [[nodiscard]] u16 GetAssetVersion(AssetId id) const noexcept;
    [[nodiscard]] TextureAssetHandle FindTextureHandle(AssetId id) const noexcept;
    [[nodiscard]] AtlasHandle FindAtlasHandle(AssetId id) const noexcept;
    [[nodiscard]] BlobHandle FindBlobHandle(AssetId id) const noexcept;
    [[nodiscard]] AudioHandle FindAudioHandle(AssetId id) const noexcept;

    [[nodiscard]] constexpr const SlotMeta& GetSlotMeta(u16 slotIdx) const noexcept {
        return m_slots[slotIdx < m_highWaterSlot ? slotIdx : 0U];
    }

    [[nodiscard]] constexpr u16 HighWaterTex() const noexcept { return m_highWaterTex; }
    [[nodiscard]] constexpr const TextureRecord& GetTextureBySlot(u16 texSlot) const noexcept {
        return (texSlot < m_highWaterTex) ? m_textures[texSlot] : m_textures[0];
    }

    void BindGpuTexture(u16 texSlot, TextureHandle gpu) noexcept;

    // Bounds-safe LZ4 block decompressor and deterministic hash-chain LZ4 compressor.
    [[nodiscard]] static Result<usize, AssetErrc> DecompressLz4(
        std::span<const u8> src,
        std::span<u8> dst) noexcept;

    [[nodiscard]] static Result<usize, AssetErrc> CompressLz4(
        std::span<const u8> src,
        std::span<u8> dst) noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    [[nodiscard]] bool EnsurePayloadSlot(SlotMeta& meta) noexcept;
    void CompleteSlot(SlotMeta& meta, AssetState finalState) noexcept;
    void PropagateAtlasGpuTexture(u16 texSlot, TextureHandle gpu) noexcept;
    void EvictSlotInternal(
        u16 slotIdx,
        std::span<TextureHandle> outDestroyedGpuTextures,
        u32& ioGpuDestroyCount) noexcept;

    // Cache Line 0 (offset 0..63, 64B exact, 0B padding): 120 Hz / 240 Hz hot read-only pointers & bounds
    SpriteFrame* m_sprites{nullptr};
    u16* m_spriteGens{nullptr};
    AtlasRecord* m_atlases{nullptr};
    TextureRecord* m_textures{nullptr};
    BlobRecord* m_blobs{nullptr};
    AudioRecord* m_audio{nullptr};
    u32 m_activeScopesMask{0};
    u16 m_highWaterSprite{0};
    u16 m_highWaterAtlas{0};
    u16 m_highWaterTex{0};
    u16 m_highWaterBlob{0};
    u16 m_highWaterAudio{0};
    u16 m_highWaterSlot{0};

    // Cache Line 1 (offset 64..127, 64B exact, 0B padding): O(1) per-scope Residency::Sim readiness counters
    alignas(64) std::array<u16, kMaxAssetScopes> m_scopeSimPending{};

    // Cache Line 2 (offset 128..191, 64B exact, 0B padding): Master slot index, arena & VRAM budget state
    SlotMeta* m_slots{nullptr};
    u32* m_spriteNameHashes{nullptr};
    RobinHoodMap<u16> m_idToSlot{};
    AlignedArena m_arena{};
    u32 m_currentFrame{1};
    u32 m_currentVramBytes{0};
    u32 m_vramBudgetBytes{64U * 1024U * 1024U};
    u16 m_maxSlots{0};
    u16 m_freeSlotHead{kNullAssetSlot};

    // Cache Line 3 (offset 192..255, 64B exact, 0B padding): Generation arrays, intrusive freelist heads, coalescing allocator & telemetry
    u16* m_texGens{nullptr};
    u16* m_audioGens{nullptr};
    u64* m_slotAssetIds{nullptr};
    FreePayloadBlock* m_freePayloadHead{nullptr};
    u16 m_maxSprites{0};
    u16 m_maxAtlases{0};
    u16 m_maxTextures{0};
    u16 m_maxBlobs{0};
    u16 m_maxAudio{0};
    u16 m_freeTexHead{kNullAssetSlot};
    u16 m_freeAtlasHead{kNullAssetSlot};
    u16 m_freeBlobHead{kNullAssetSlot};
    u16 m_freeAudioHead{kNullAssetSlot};
    u16 m_pendingGpuTexCount{0};
    u32 m_payloadBaseMarker{0};
    u32 m_evictedCount{0};
    u32 m_hotReloadCount{0};
};
static_assert(sizeof(AssetBank) == 256);

} // namespace en
