#pragma once

#include "assets/AssetBank.h"

#include <memory>
#include <string_view>

struct SDL_AsyncIO;
struct SDL_AsyncIOQueue;

namespace en::renderer { class Renderer; }

namespace en::assets {

struct AssetSystemConfig {
    AssetBankConfig bank{};
    u32 stagingRingBytes{32 * 1024 * 1024};
    u32 maxTotalTocEntries{16384};
    u16 maxInFlightJobs{256};
    u8 maxMounts{8};
};

struct PumpBudget {
    u32 maxGpuUploads{4};
    u32 maxUploadBytes{16 * 1024 * 1024};
};

// SDL3 hardware and OS I/O coordinator for the asset subsystem.
// Bridges AssetBank with OS memory-mapped archives, SDL_AsyncIOQueue kernel streaming, and renderer::Renderer.
// Move-only resource owner; all mutations occur exclusively on the main thread inside Pump().
class alignas(64) AssetSystem {
public:
    AssetSystem() = default;
    ~AssetSystem() noexcept;

    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;
    AssetSystem(AssetSystem&& other) noexcept;
    AssetSystem& operator=(AssetSystem&& other) noexcept;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<void, AssetErrc> MountPackMemory(
        std::span<const u8> mappedPak,
        u8 priority = 0) noexcept;

    [[nodiscard]] Result<void, AssetErrc> MountPackFile(
        std::string_view filePath,
        u8 priority = 0) noexcept;

    [[nodiscard]] Result<ScopeId, AssetErrc> BeginScope() noexcept { return m_bank.BeginScope(); }
    void EndScope(ScopeId scope) noexcept { m_bank.EndScope(scope); }

    [[nodiscard]] Result<u16, AssetErrc> Pin(
        ScopeId scope,
        AssetId id,
        AssetKind kind,
        Residency residency) noexcept {
        return EnqueueLoad(id, kind, scope, residency);
    }

    [[nodiscard]] Result<TextureAssetHandle, AssetErrc> PinTexture(
        ScopeId scope,
        AssetId id,
        Residency residency = Residency::Presentation) noexcept {
        return PinTyped<TextureAssetHandle>(scope, id, AssetKind::Texture, residency);
    }

    [[nodiscard]] Result<AtlasHandle, AssetErrc> PinAtlas(
        ScopeId scope,
        AssetId id,
        Residency residency = Residency::Presentation) noexcept {
        return PinTyped<AtlasHandle>(scope, id, AssetKind::Atlas, residency);
    }

    [[nodiscard]] Result<BlobHandle, AssetErrc> PinBlob(
        ScopeId scope,
        AssetId id,
        Residency residency = Residency::Sim) noexcept {
        return PinTyped<BlobHandle>(scope, id, AssetKind::Blob, residency);
    }

    [[nodiscard]] Result<AudioHandle, AssetErrc> PinAudio(
        ScopeId scope,
        AssetId id,
        Residency residency = Residency::Presentation) noexcept {
        return PinTyped<AudioHandle>(scope, id, AssetKind::Audio, residency);
    }

    void Pump(renderer::Renderer& renderer, const PumpBudget& budget = {}) noexcept {
        PumpInternal(&renderer, budget);
    }
    void PumpHeadless(const PumpBudget& budget = {}) noexcept {
        PumpInternal(nullptr, budget);
    }

    [[nodiscard]] Result<void, AssetErrc> WaitSimReady(
        ScopeId scope,
        renderer::Renderer* optionalRenderer = nullptr) noexcept;

    [[nodiscard]] constexpr AssetBank& Bank() noexcept { return m_bank; }
    [[nodiscard]] constexpr const AssetBank& Bank() const noexcept { return m_bank; }

private:
    static constexpr u16 kInvalidJob  = 0xFFFFu;
    static constexpr u8  kJobComplete = 1u << 0;
    static constexpr u8  kJobSuccess  = 1u << 1;

    // Unified 16-byte VFS Robin Hood slot resolving AssetId directly to (mountIndex, entryIndex) in 1 probe.
    struct alignas(16) VfsTocSlot {
        u64 key{0};
        u32 entryIndex{0};
        u16 psl{0};
        u8 mountIndex{0};
        u8 priority{0};
    };
    static_assert(sizeof(VfsTocSlot) == 16 && alignof(VfsTocSlot) == 16);

    struct alignas(32) PakMount {
        SDL_AsyncIO* asyncFile{nullptr};
        const u8* mappedBase{nullptr};
        std::size_t mappedSize{0};
        u64 reserved{0};
    };
    static_assert(sizeof(PakMount) == 32 && alignof(PakMount) == 32);

    // 1-cache-line (64B) intrusive FIFO job record ordered strictly by arrival sequence.
    struct alignas(64) PendingJob {
        PakEntry entry{};
        u32 stagingOffset{0};
        u32 stagingReserve{0};
        u16 slot{0};
        u16 nextJob{kInvalidJob};
        u16 prevJob{kInvalidJob};
        u8 mountIndex{0};
        u8 flags{0};
    };
    static_assert(sizeof(PendingJob) == 64 && alignof(PendingJob) == 64);

    template <typename HandleT>
    [[nodiscard]] Result<HandleT, AssetErrc> PinTyped(
        ScopeId scope,
        AssetId id,
        AssetKind kind,
        Residency residency) noexcept {
        auto slotRes = EnqueueLoad(id, kind, scope, residency);
        if (!slotRes) {
            return Err(slotRes.error());
        }
        if constexpr (std::is_same_v<HandleT, TextureAssetHandle>) return m_bank.ToTextureHandle(*slotRes);
        if constexpr (std::is_same_v<HandleT, AtlasHandle>)        return m_bank.ToAtlasHandle(*slotRes);
        if constexpr (std::is_same_v<HandleT, BlobHandle>)         return m_bank.ToBlobHandle(*slotRes);
        if constexpr (std::is_same_v<HandleT, AudioHandle>)        return m_bank.ToAudioHandle(*slotRes);
    }

    void RegisterVfsEntries(u8 mountIdx, u8 priority, u32 firstEntry, u32 entryCount) noexcept;
    [[nodiscard]] bool LocateEntry(AssetId id, u8& outMountIdx, PakEntry& outEntry) const noexcept;
    [[nodiscard]] Result<u16, AssetErrc> EnqueueLoad(
        AssetId id,
        AssetKind expectedKind,
        ScopeId scope,
        Residency residency) noexcept;

    [[nodiscard]] bool AllocateStaging(u32 size, u32& outOffset, u32& outReserve) noexcept;
    void ReleaseJob(u16 jobIdx) noexcept;
    void FinalizeJob(PendingJob& job, renderer::Renderer* renderer) noexcept;
    void PumpInternal(renderer::Renderer* renderer, const PumpBudget& budget) noexcept;

    // Cache Line 0 (offset 0..63): Hot Pump(), EnqueueLoad(), and unified VFS LocateEntry() state.
    SDL_AsyncIOQueue* m_ioQueue{nullptr};
    PakMount* m_mounts{nullptr};
    PendingJob* m_jobs{nullptr};
    u8* m_stagingRing{nullptr};
    PakEntry* m_tocEntriesPool{nullptr};
    VfsTocSlot* m_vfsHash{nullptr};
    u32 m_vfsHashMask{0};
    u32 m_ringWrite{0};
    u32 m_ringUsed{0};
    u16 m_activeJobHead{kInvalidJob};
    u16 m_activeJobTail{kInvalidJob};

    // Cache Line 1+: Job/TOC counters, configuration, arena slab owner, and AssetBank.
    u16 m_freeJobHead{kInvalidJob};
    u16 m_highWaterJob{0};
    u16 m_activeJobCount{0};
    u16 m_stagingJobCount{0};
    u16 m_headlessTexCounter{1};
    u8 m_mountCount{0};
    u8 m_reservedPad{0};
    u32 m_frameCounter{0};
    u32 m_tocEntriesUsed{0};

    AssetSystemConfig m_cfg{};
    std::unique_ptr<u64[]> m_arena;
    AssetBank m_bank{};
};

} // namespace en::assets
