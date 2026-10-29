#pragma once

#include "assets/AssetTypes.h"
#include "core/Containers.h"

namespace en {

// Room-scoped asset bank with O(1) generational handle resolution over unified 32-byte AssetSlots.
// Move-disabled owner of an Arena; invalid handles resolve to permanent constexpr fallback records.
class AssetBank {
public:
    using GpuDestroyFn = void (*)(TextureHandle gpu, void* ctx) noexcept;

    struct FreeBlock {
        FreeBlock* next{nullptr};
        u32        bytes{0};
    };
    static_assert(sizeof(FreeBlock) == 16);

    AssetBank() noexcept = default;
    ~AssetBank() noexcept { Shutdown(); }
    AssetBank(const AssetBank&) = delete;
    AssetBank& operator=(const AssetBank&) = delete;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetBankConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    constexpr void SetGpuDestroy(GpuDestroyFn fn, void* ctx = nullptr) noexcept {
        m_gpuDestroy = fn;
        m_gpuCtx     = ctx;
    }

    [[nodiscard]] Result<u8, AssetErrc> BeginScope() noexcept;
    void EndScope(u8 scope) noexcept;
    constexpr void AdvanceFrame(u32 frame) noexcept {
        m_frame = static_cast<u16>(frame & 0xFFFFU);
    }
    [[nodiscard]] constexpr u16 ActiveScopes() const noexcept { return m_scopes; }
    [[nodiscard]] constexpr u16 Frame() const noexcept { return m_frame; }

    [[nodiscard]] constexpr const SpriteFrame& GetSprite(SpriteHandle h) const noexcept {
        return (h.IsValid() && h.index < m_spriteHigh &&
                m_sprites[h.index].generation == h.generation)
                   ? m_sprites[h.index]
                   : m_sprites[0];
    }

    [[nodiscard]] constexpr const SpriteFrame& GetAtlasFrame(AtlasHandle h, u16 i) const noexcept {
        const u16 slot = Resolve(h, AssetKind::Atlas);
        const AtlasRecord& a = (slot > 0U) ? m_slots[slot].atlas : detail::kDefaultAtlas;
        if (a.spriteCount > 0U) {
            const u16 idx = static_cast<u16>(a.firstSprite + (i % a.spriteCount));
            if (idx < m_spriteHigh && m_sprites[idx].generation != 0U) return m_sprites[idx];
        }
        return m_sprites[0];
    }

    [[nodiscard]] constexpr SpriteHandle GetSpriteHandle(AtlasHandle h, u16 i) const noexcept {
        const u16 slot = Resolve(h, AssetKind::Atlas);
        const AtlasRecord& a = (slot > 0U) ? m_slots[slot].atlas : detail::kDefaultAtlas;
        if (a.spriteCount > 0U) {
            const u16 idx = static_cast<u16>(a.firstSprite + (i % a.spriteCount));
            if (idx < m_spriteHigh && m_sprites[idx].generation != 0U) {
                return SpriteHandle{idx, m_sprites[idx].generation};
            }
        }
        return SpriteHandle{0U, 1U};
    }

    [[nodiscard]] constexpr const TextureRecord& GetTexture(TextureAssetHandle h) const noexcept {
        const u16 slot = Resolve(h, AssetKind::Texture);
        return (slot > 0U) ? m_slots[slot].tex : detail::kDefaultTexture;
    }

    [[nodiscard]] constexpr const BlobRecord& GetBlob(BlobHandle h) const noexcept {
        const u16 slot = Resolve(h, AssetKind::Blob);
        return (slot > 0U) ? m_slots[slot].blob : detail::kDefaultBlob;
    }

    [[nodiscard]] constexpr const AudioRecord& GetAudio(AudioHandle h) const noexcept {
        const u16 slot = Resolve(h, AssetKind::Audio);
        return (slot > 0U) ? m_slots[slot].audio : detail::kDefaultAudio;
    }

    [[nodiscard]] TextureAssetHandle RegisterTexture(
        AssetId id, u8 scope, u16 w, u16 h,
        std::span<const u8> rgba, TextureHandle gpu = kWhiteTexture) noexcept;

    [[nodiscard]] TextureAssetHandle LoadTexture(
        AssetId id, u8 scope,
        std::string_view png, std::string_view bmp,
        u16 w, u16 h, TextureHandle gpu = kWhiteTexture) noexcept;

    [[nodiscard]] AtlasHandle RegisterGridAtlas(
        AssetId id, u8 scope, TextureAssetHandle tex,
        u16 cols, u16 rows, Vec2 pivot = {0.5f, 0.5f}) noexcept;

    [[nodiscard]] BlobHandle RegisterBlob(AssetId id, u8 scope, std::span<const u8> data) noexcept;

    [[nodiscard]] AudioHandle RegisterAudio(
        AssetId id, u8 scope, std::span<const f32> pcm,
        u32 rate = 48000, u8 ch = 2, bool loop = false) noexcept;

    void Touch(AssetId id) noexcept;
    void TouchAtlas(AtlasHandle h) noexcept;
    void BindGpu(TextureAssetHandle tex, TextureHandle gpu) noexcept;

    [[nodiscard]] u32 EvictUnpinned(std::span<TextureHandle> outGpu = {}) noexcept;
    [[nodiscard]] u32 EvictLru(std::span<TextureHandle> outGpu = {},
                               u32 budget = 0xFFFFFFFFU) noexcept;

    [[nodiscard]] constexpr u32 VramBytes() const noexcept { return m_vram; }
    [[nodiscard]] constexpr u32 VramBudget() const noexcept { return m_vramBudget; }
    [[nodiscard]] constexpr bool OverVram() const noexcept { return m_vram > m_vramBudget; }
    [[nodiscard]] constexpr u32 EvictCount() const noexcept { return m_evicts; }
    [[nodiscard]] constexpr u32 ArenaUsed() const noexcept { return m_arena.Used(); }

    [[nodiscard]] TextureAssetHandle FindTexture(AssetId id) const noexcept {
        return Find<TextureAssetHandle>(id, AssetKind::Texture);
    }
    [[nodiscard]] AtlasHandle FindAtlas(AssetId id) const noexcept {
        return Find<AtlasHandle>(id, AssetKind::Atlas);
    }
    [[nodiscard]] BlobHandle FindBlob(AssetId id) const noexcept {
        return Find<BlobHandle>(id, AssetKind::Blob);
    }
    [[nodiscard]] AudioHandle FindAudio(AssetId id) const noexcept {
        return Find<AudioHandle>(id, AssetKind::Audio);
    }

private:
    template <typename H>
    [[nodiscard]] constexpr u16 Resolve(H h, AssetKind kind) const noexcept {
        return (h.IsValid() && h.index > 0U && h.index < m_slotHigh &&
                m_slots[h.index].meta.generation == h.generation &&
                m_slots[h.index].meta.Kind() == kind)
                   ? h.index
                   : 0U;
    }

    template <typename H>
    [[nodiscard]] H Find(AssetId id, AssetKind kind) const noexcept {
        const u16* p = m_idMap.Find(id.value);
        if (p && *p > 0U && *p < m_slotHigh) {
            const SlotMeta& m = m_slots[*p].meta;
            if (m.assetId != 0ULL && m.Kind() == kind) return H{*p, m.generation};
        }
        return H{};
    }

    [[nodiscard]] u16 Acquire(AssetId id, AssetKind kind, u8 scope, bool& needsLoad) noexcept;
    void Fail(u16 slot) noexcept;
    [[nodiscard]] std::span<u8> AllocBytes(usize n) noexcept;
    void FreeBytes(const void* p, usize n) noexcept;
    void PropagateGpu(u16 texSlot, TextureHandle gpu) noexcept;
    void EvictOne(u16 slot, std::span<TextureHandle> outGpu, u32& nGpu) noexcept;

    AssetSlot*         m_slots{nullptr};
    SpriteFrame*       m_sprites{nullptr};
    FreeBlock*         m_freeHead{nullptr};
    GpuDestroyFn       m_gpuDestroy{nullptr};
    void*              m_gpuCtx{nullptr};

    HashMap<u16>       m_idMap{};
    Arena              m_arena{};

    u32                m_vram{0};
    u32                m_vramBudget{64U * 1024U * 1024U};
    u32                m_payloadMark{0};
    u32                m_evicts{0};

    u16                m_maxSlots{0};
    u16                m_slotHigh{0};
    u16                m_slotFree{0};
    u16                m_maxSprites{0};
    u16                m_spriteHigh{0};
    u16                m_scopes{0};
    u16                m_frame{1};
    std::array<u16, 5> m_maxByKind{};
    std::array<u16, 5> m_liveByKind{};
};

} // namespace en
