#pragma once

#include "assets/AssetBank.h"

#include <memory>
#include <string>
#include <string_view>

struct SDL_AsyncIO;
struct SDL_AsyncIOQueue;

namespace en::renderer { class Renderer; }

namespace en::assets {

struct AssetSystemConfig {
    AssetBankConfig bank{};
    u32 stagingRingBytes{32 * 1024 * 1024};
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
class AssetSystem {
public:
    AssetSystem() = default;
    ~AssetSystem() noexcept;

    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;
    AssetSystem(AssetSystem&& other) noexcept;
    AssetSystem& operator=(AssetSystem&& other) noexcept;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    // Mounts a cooked .enpak file (either via zero-copy memory view or SDL_AsyncIO file handle).
    [[nodiscard]] Result<void, AssetErrc> MountPackMemory(
        std::span<const u8> mappedPak,
        u8 priority = 0) noexcept;

    [[nodiscard]] Result<void, AssetErrc> MountPackFile(
        std::string_view filePath,
        u8 priority = 0) noexcept;

    [[nodiscard]] Result<ScopeId, AssetErrc> BeginScope() noexcept { return m_bank.BeginScope(); }
    void EndScope(ScopeId scope) noexcept { m_bank.EndScope(scope); }

    [[nodiscard]] Result<TextureAssetHandle, AssetErrc> PinTexture(
        ScopeId scope,
        AssetId id,
        Residency residency = Residency::Presentation) noexcept;

    [[nodiscard]] Result<AtlasHandle, AssetErrc> PinAtlas(
        ScopeId scope,
        AssetId id,
        Residency residency = Residency::Presentation) noexcept;

    [[nodiscard]] Result<BlobHandle, AssetErrc> PinBlob(
        ScopeId scope,
        AssetId id,
        Residency residency = Residency::Sim) noexcept;

    [[nodiscard]] Result<AudioHandle, AssetErrc> PinAudio(
        ScopeId scope,
        AssetId id,
        Residency residency = Residency::Presentation) noexcept;

    // Drains completed async reads in deterministic request order and uploads textures to Renderer.
    void Pump(renderer::Renderer& renderer, const PumpBudget& budget = {}) noexcept;

    // Headless pump for CI unit tests without an active SDL_Renderer.
    void PumpHeadless(const PumpBudget& budget = {}) noexcept;

    [[nodiscard]] Result<void, AssetErrc> WaitSimReady(
        ScopeId scope,
        renderer::Renderer* optionalRenderer = nullptr) noexcept;

    [[nodiscard]] constexpr const AssetBank& Bank() const noexcept { return m_bank; }
    [[nodiscard]] constexpr AssetBank& Bank() noexcept { return m_bank; }

private:
    struct MountTocSlot {
        u64 key{0};
        u32 entryIndex{0};
        u32 psl{0};
    };

    struct PakMount {
        SDL_AsyncIO* asyncFile{nullptr};
        const u8* mappedBase{nullptr};
        std::size_t mappedSize{0};
        std::unique_ptr<PakEntry[]> entries;
        std::unique_ptr<MountTocSlot[]> tocHash;
        u32 entryCount{0};
        u32 hashMask{0};
        u8 priority{0};
        bool active{false};
    };

    struct PendingJob {
        PakEntry entry{};
        u64 sequence{0};
        u32 stagingOffset{0};
        u32 stagingReserve{0};
        u16 slot{0};
        u16 depTextureSlot{0};
        u8 mountIndex{0};
        bool ioComplete{false};
        bool ioSuccess{false};
        bool active{false};
    };

    static void BuildMountHashTable(
        std::span<const PakEntry> entries,
        std::span<MountTocSlot> slots,
        u32 hashMask) noexcept;

    [[nodiscard]] bool LocateEntry(AssetId id, u8& outMountIdx, PakEntry& outEntry) const noexcept;
    [[nodiscard]] Result<u16, AssetErrc> EnqueueLoad(
        AssetId id,
        AssetKind expectedKind,
        ScopeId scope,
        Residency residency) noexcept;

    [[nodiscard]] bool AllocateStaging(u32 size, u32& outOffset, u32& outReserve) noexcept;
    void FinalizeJob(PendingJob& job, renderer::Renderer* renderer) noexcept;
    void PumpInternal(renderer::Renderer* renderer, const PumpBudget& budget) noexcept;

    AssetSystemConfig m_cfg{};
    AssetBank m_bank{};
    SDL_AsyncIOQueue* m_ioQueue{nullptr};

    std::unique_ptr<PakMount[]> m_mounts;
    u8 m_mountCount{0};

    std::unique_ptr<u8[]> m_stagingRing;
    u32 m_ringCap{0};
    u32 m_ringWrite{0};
    u32 m_ringRead{0};
    u32 m_ringUsed{0};

    std::unique_ptr<PendingJob[]> m_jobs;
    u64 m_nextSequence{1};
    u32 m_frameCounter{0};
    u16 m_headlessTexCounter{1};
};

} // namespace en::assets