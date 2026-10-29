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

// Room-scoped asset bank. Slot 0 of every table is permanent fallback.
// Flow: BeginScope → Load/Register → Get* → EndScope → EvictUnpinned.
// GPU: Renderer::CreateTexture → BindGpu. Destroy via Evict callback/list.
class alignas(64) AssetBank {
public:
    using GpuDestroyFn = void (*)(TextureHandle gpu, void* ctx) noexcept;

    struct alignas(16) FreeBlock {
        FreeBlock* next{nullptr};
        u32 bytes{0};
    };
    static_assert(sizeof(FreeBlock) == 16);

    struct Pool {
        u16 max{0};
        u16 high{0};
        u16 free{0};

        constexpr void Reset(u16 n = 0U) noexcept {
            max = n;
            high = (n > 0U) ? 1U : 0U;
            free = 0U;
        }
    };

    AssetBank() noexcept = default;
    ~AssetBank() noexcept { Shutdown(); }
    AssetBank(const AssetBank&) = delete;
    AssetBank& operator=(const AssetBank&) = delete;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetBankConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    constexpr void SetGpuDestroy(GpuDestroyFn fn, void* ctx = nullptr) noexcept {
        m_gpuDestroy = fn;
        m_gpuCtx = ctx;
    }

    // Lifetime — scope ≈ room/area cluster
    [[nodiscard]] Result<u8, AssetErrc> BeginScope() noexcept;
    void EndScope(u8 scope) noexcept;
    constexpr void AdvanceFrame(u32 frame) noexcept {
        m_frame = static_cast<u16>(frame & 0xFFFFU);
    }
    [[nodiscard]] constexpr u16 ActiveScopes() const noexcept { return m_scopes; }
    [[nodiscard]] constexpr u16 Frame() const noexcept { return m_frame; }

    // Queries — invalid handle → slot 0 fallback, never null
    [[nodiscard]] constexpr const SpriteFrame& GetSprite(SpriteHandle h) const noexcept {
        return (h.IsValid() && h.index < m_spriteHigh &&
                m_sprites[h.index].generation == h.generation)
                   ? m_sprites[h.index]
                   : m_sprites[0];
    }
    [[nodiscard]] constexpr const SpriteFrame& GetAtlasFrame(AtlasHandle h, u16 i) const noexcept {
        const AtlasRecord& a = m_atlases[Resolve(h, AssetKind::Atlas)];
        if (a.spriteCount > 0U) {
            const u16 idx = static_cast<u16>(a.firstSprite + (i % a.spriteCount));
            if (idx < m_spriteHigh && m_sprites[idx].generation != 0U) return m_sprites[idx];
        }
        return m_sprites[0];
    }
    [[nodiscard]] constexpr SpriteHandle GetSpriteHandle(AtlasHandle h, u16 i) const noexcept {
        const AtlasRecord& a = m_atlases[Resolve(h, AssetKind::Atlas)];
        if (a.spriteCount > 0U) {
            const u16 idx = static_cast<u16>(a.firstSprite + (i % a.spriteCount));
            if (idx < m_spriteHigh && m_sprites[idx].generation != 0U) {
                return SpriteHandle{idx, m_sprites[idx].generation};
            }
        }
        return SpriteHandle{0U, 1U};
    }
    [[nodiscard]] constexpr const TextureRecord& GetTexture(TextureAssetHandle h) const noexcept {
        return m_textures[Resolve(h, AssetKind::Texture)];
    }
    [[nodiscard]] constexpr const BlobRecord& GetBlob(BlobHandle h) const noexcept {
        return m_blobs[Resolve(h, AssetKind::Blob)];
    }
    [[nodiscard]] constexpr const AudioRecord& GetAudio(AudioHandle h) const noexcept {
        return m_audio[Resolve(h, AssetKind::Audio)];
    }

    // Idempotent by AssetId
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
    [[nodiscard]] constexpr u32 ArenaUsed() const noexcept { return m_arena.UsedBytes(); }

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
    enum : u8 { kTex = 0, kAtl = 1, kBlob = 2, kAud = 3 };

    template <typename H>
    [[nodiscard]] constexpr u16 Resolve(H h, AssetKind kind) const noexcept {
        if (h.IsValid() && h.index > 0U && h.index < m_slots.high) {
            const SlotMeta& m = m_meta[h.index];
            if (m.generation == h.generation && m.Kind() == kind) return m.Payload();
        }
        return 0U;
    }

    template <typename H>
    [[nodiscard]] H Find(AssetId id, AssetKind kind) const noexcept {
        const u16* p = m_idMap.Find(id.value);
        if (p && *p > 0U && *p < m_slots.high) {
            const SlotMeta& m = m_meta[*p];
            if (m.Kind() == kind && m.Payload() != 0U) return H{*p, m.generation};
        }
        return H{};
    }

    [[nodiscard]] u16 Acquire(AssetId id, AssetKind kind, u8 scope, bool& needsLoad) noexcept;
    void Fail(u16 slot) noexcept;
    [[nodiscard]] bool EnsurePayload(SlotMeta& m) noexcept;
    [[nodiscard]] std::span<u8> AllocBytes(usize n) noexcept;
    void FreeBytes(const void* p, usize n) noexcept;
    void PropagateGpu(u16 texMaster, TextureHandle gpu) noexcept;
    void EvictOne(u16 slot, std::span<TextureHandle> outGpu, u32& nGpu) noexcept;

    TextureAssetHandle CommitTex(u16 slot, u16 w, u16 h, std::span<const u8> rgba,
                                 TextureHandle gpu) noexcept;
    AtlasHandle CommitGrid(u16 slot, TextureAssetHandle tex, u16 cols, u16 rows, Vec2 pivot) noexcept;
    BlobHandle CommitBlob(u16 slot, std::span<const u8> data) noexcept;
    AudioHandle CommitAudio(u16 slot, std::span<const f32> pcm, u32 rate, u8 ch, bool loop) noexcept;

    SlotMeta* m_meta{nullptr};
    SpriteFrame* m_sprites{nullptr};
    AtlasRecord* m_atlases{nullptr};
    TextureRecord* m_textures{nullptr};
    BlobRecord* m_blobs{nullptr};
    AudioRecord* m_audio{nullptr};
    FreeBlock* m_freeHead{nullptr};
    GpuDestroyFn m_gpuDestroy{nullptr};
    void* m_gpuCtx{nullptr};

    RobinHoodMap<u16> m_idMap{};
    AlignedArena m_arena{};

    u32 m_vram{0};
    u32 m_vramBudget{64U * 1024U * 1024U};
    u32 m_payloadMark{0};
    u32 m_evicts{0};

    Pool m_slots{};
    std::array<Pool, 4> m_pools{};
    u16 m_maxSprites{0};
    u16 m_spriteHigh{0};
    u16 m_scopes{0};
    u16 m_frame{1};
};
static_assert(sizeof(AssetBank) == 192);

} // namespace en
