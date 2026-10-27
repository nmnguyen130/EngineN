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
};
static_assert(sizeof(AssetBankConfig) == 24);

// Data-oriented asset storage, coalescing payload sub-allocator, 16-scope lifetime tracker, and LRU VRAM eviction manager.
// Non-copyable owner of an AlignedArena storing resource tables and payload slices.
// Invariant: Slot 0 of every resource table holds a valid non-null fallback record so queries never return null.
class alignas(64) AssetBank {
public:
    using GpuTextureDestroyFn = void (*)(TextureHandle gpu, void* userCtx) noexcept;

    struct alignas(16) FreePayloadBlock {
        FreePayloadBlock* next{nullptr};
        u32 byteSize{0};
    };
    static_assert(sizeof(FreePayloadBlock) == 16);

    struct PoolState {
        u16 max{0};
        u16 highWater{0};
        u16 freeHead{0};

        constexpr void Reset(u16 maxItems = 0U) noexcept {
            max = maxItems;
            highWater = (maxItems > 0U) ? 1U : 0U;
            freeHead = 0U;
        }
    };

    AssetBank() noexcept = default;
    ~AssetBank() noexcept { Shutdown(); }

    AssetBank(const AssetBank&) = delete;
    AssetBank& operator=(const AssetBank&) = delete;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetBankConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    constexpr void SetGpuTextureDestroyCallback(GpuTextureDestroyFn fn, void* userCtx = nullptr) noexcept {
        m_gpuDestroyFn = fn;
        m_gpuDestroyUserCtx = userCtx;
    }

    [[nodiscard]] Result<u8, AssetErrc> BeginScope() noexcept;
    void EndScope(u8 scope) noexcept;
    constexpr void AdvanceFrame(u32 frameIndex) noexcept {
        m_currentFrame = static_cast<u16>(frameIndex & 0xFFFFU);
    }

    [[nodiscard]] constexpr u16 ActiveScopesMask() const noexcept { return m_activeScopesMask; }
    [[nodiscard]] constexpr u16 CurrentFrame() const noexcept { return m_currentFrame; }

    [[nodiscard]] constexpr const SpriteFrame& GetSprite(SpriteHandle h) const noexcept {
        return (h.index < m_highWaterSprite && m_sprites[h.index].generation == h.generation)
                   ? m_sprites[h.index]
                   : m_sprites[0];
    }

    [[nodiscard]] constexpr const SpriteFrame& GetAtlasFrame(AtlasHandle h, u16 frameOffset) const noexcept {
        const u16 aIdx = ResolvePayloadSlot(h, AssetKind::Atlas);
        const AtlasRecord& rec = m_atlases[aIdx];
        if (rec.spriteCount > 0U) {
            const u16 idx = static_cast<u16>(rec.firstSprite + (frameOffset % rec.spriteCount));
            if (idx < m_highWaterSprite) return m_sprites[idx];
        }
        return m_sprites[0];
    }

    [[nodiscard]] constexpr SpriteHandle GetSpriteHandle(AtlasHandle h, u16 frameOffset) const noexcept {
        const u16 aIdx = ResolvePayloadSlot(h, AssetKind::Atlas);
        const AtlasRecord& rec = m_atlases[aIdx];
        if (rec.spriteCount > 0U) {
            const u16 idx = static_cast<u16>(rec.firstSprite + (frameOffset % rec.spriteCount));
            if (idx < m_highWaterSprite) return SpriteHandle{idx, m_sprites[idx].generation};
        }
        return SpriteHandle{0U, 1U};
    }

    [[nodiscard]] constexpr const TextureRecord& GetTexture(TextureAssetHandle h) const noexcept {
        return m_textures[ResolvePayloadSlot(h, AssetKind::Texture)];
    }

    [[nodiscard]] constexpr const BlobRecord& GetBlob(BlobHandle h) const noexcept {
        return m_blobs[ResolvePayloadSlot(h, AssetKind::Blob)];
    }

    [[nodiscard]] constexpr const AudioRecord& GetAudio(AudioHandle h) const noexcept {
        return m_audio[ResolvePayloadSlot(h, AssetKind::Audio)];
    }

    [[nodiscard]] u16 AcquireSlot(
        AssetId id,
        AssetKind kind,
        u8 scope,
        bool& outNeedsLoad) noexcept;

    void TouchAsset(AssetId id) noexcept;
    void TouchAtlas(AtlasHandle h) noexcept;

    [[nodiscard]] std::span<u8> AllocatePayloadSlice(usize bytes) noexcept;
    void FreePayloadSlice(const void* ptr, usize bytes) noexcept;

    TextureAssetHandle CommitTexture(
        u16 masterSlot,
        u16 width,
        u16 height,
        std::span<const u8> rgbaPixels,
        TextureHandle gpu = kWhiteTexture) noexcept;

    AtlasHandle CommitAtlas(
        u16 masterSlot,
        TextureAssetHandle texHandle,
        std::span<const SpriteFrame> frames) noexcept;

    AtlasHandle CommitGridAtlas(
        u16 masterSlot,
        TextureAssetHandle texHandle,
        u16 cols,
        u16 rows,
        Vec2 pivot = {0.5f, 0.5f}) noexcept;

    BlobHandle CommitBlob(
        u16 masterSlot,
        std::span<const u8> payload) noexcept;

    AudioHandle CommitAudio(
        u16 masterSlot,
        std::span<const f32> pcmSamples,
        u32 sampleRate = 48000,
        u8 channels = 2,
        bool looping = false) noexcept;

    [[nodiscard]] TextureAssetHandle LoadTextureFromDisk(
        AssetId id,
        u8 scope,
        std::string_view primaryPngPath,
        std::string_view fallbackBmpPath,
        u16 width,
        u16 height,
        TextureHandle gpu = kWhiteTexture) noexcept;

    [[nodiscard]] AtlasHandle RegisterGridAtlas(
        AssetId id,
        u8 scope,
        TextureAssetHandle texHandle,
        u16 cols,
        u16 rows,
        Vec2 pivot = {0.5f, 0.5f}) noexcept;

    [[nodiscard]] BlobHandle RegisterBlob(
        AssetId id,
        u8 scope,
        std::span<const u8> payload) noexcept;

    [[nodiscard]] AudioHandle RegisterAudio(
        AssetId id,
        u8 scope,
        std::span<const f32> pcmSamples,
        u32 sampleRate = 48000,
        u8 channels = 2,
        bool looping = false) noexcept;

    void MarkFailed(u16 masterSlot) noexcept;

    [[nodiscard]] u32 EvictUnpinned(std::span<TextureHandle> outDestroyedGpuTextures = {}) noexcept;

    [[nodiscard]] u32 EvictLruToVramBudget(
        std::span<TextureHandle> outDestroyedGpuTextures = {},
        u32 targetVramBytes = 0xFFFFFFFFU) noexcept;

    [[nodiscard]] constexpr u32 CurrentVramBytes() const noexcept { return m_currentVramBytes; }
    [[nodiscard]] constexpr u32 VramBudgetBytes() const noexcept { return m_vramBudgetBytes; }
    [[nodiscard]] constexpr bool IsOverVramBudget() const noexcept { return m_currentVramBytes > m_vramBudgetBytes; }
    [[nodiscard]] constexpr u32 EvictedCount() const noexcept { return m_evictedCount; }
    [[nodiscard]] constexpr u32 ArenaUsedBytes() const noexcept { return m_arena.UsedBytes(); }

    [[nodiscard]] TextureAssetHandle FindTextureHandle(AssetId id) const noexcept {
        return FindTypedHandle<TextureAssetHandle>(id, AssetKind::Texture);
    }
    [[nodiscard]] AtlasHandle FindAtlasHandle(AssetId id) const noexcept {
        return FindTypedHandle<AtlasHandle>(id, AssetKind::Atlas);
    }
    [[nodiscard]] BlobHandle FindBlobHandle(AssetId id) const noexcept {
        return FindTypedHandle<BlobHandle>(id, AssetKind::Blob);
    }
    [[nodiscard]] AudioHandle FindAudioHandle(AssetId id) const noexcept {
        return FindTypedHandle<AudioHandle>(id, AssetKind::Audio);
    }

    void BindGpuTexture(TextureAssetHandle texHandle, TextureHandle gpu) noexcept;

private:
    template <typename HandleT>
    [[nodiscard]] constexpr u16 ResolvePayloadSlot(HandleT h, AssetKind expectedKind) const noexcept {
        if (h.index > 0U && h.index < m_slotPool.highWater) {
            const SlotMeta& m = m_slots[h.index];
            if (m.generation == h.generation && m.Kind() == expectedKind) {
                return m.payloadSlot;
            }
        }
        return 0U;
    }

    template <typename HandleT>
    [[nodiscard]] HandleT FindTypedHandle(AssetId id, AssetKind expectedKind) const noexcept {
        const u16* slotIdx = m_idToSlot.Find(id.value);
        if (slotIdx && *slotIdx < m_slotPool.highWater) {
            const SlotMeta& m = m_slots[*slotIdx];
            if (m.Kind() == expectedKind && m.payloadSlot != 0U) {
                return HandleT{*slotIdx, m.generation};
            }
        }
        return HandleT{};
    }

    [[nodiscard]] bool EnsurePayloadSlot(SlotMeta& meta) noexcept;
    void PropagateAtlasGpuTexture(u16 texMasterSlot, TextureHandle gpu) noexcept;
    void EvictSlotInternal(
        u16 slotIdx,
        std::span<TextureHandle> outDestroyedGpuTextures,
        u32& ioGpuDestroyCount) noexcept;

    SlotMeta* m_slots{nullptr};
    SpriteFrame* m_sprites{nullptr};
    AtlasRecord* m_atlases{nullptr};
    TextureRecord* m_textures{nullptr};
    BlobRecord* m_blobs{nullptr};
    AudioRecord* m_audio{nullptr};
    FreePayloadBlock* m_freePayloadHead{nullptr};
    GpuTextureDestroyFn m_gpuDestroyFn{nullptr};
    void* m_gpuDestroyUserCtx{nullptr};

    RobinHoodMap<u16> m_idToSlot{};
    AlignedArena m_arena{};

    u32 m_currentVramBytes{0};
    u32 m_vramBudgetBytes{64U * 1024U * 1024U};
    u32 m_payloadBaseMarker{0};
    u32 m_evictedCount{0};

    PoolState m_slotPool{};
    std::array<PoolState, 4> m_payloadPools{};
    u16 m_maxSprites{0};
    u16 m_highWaterSprite{0};
    u16 m_activeScopesMask{0};
    u16 m_currentFrame{1};
};
static_assert(sizeof(AssetBank) == 192);

} // namespace en
