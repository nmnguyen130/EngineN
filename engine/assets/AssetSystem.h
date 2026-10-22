#pragma once

#include "assets/AssetBank.h"

struct SDL_IOStream;

namespace en {

class Renderer;

struct alignas(8) AssetSystemConfig {
    AssetBankConfig bank{};
    usize vfsArenaBytes{2U * 1024U * 1024U};
    u16 maxMounts{16};
    u16 maxVfsEntries{2048};
    u32 reserved{0};
};
static_assert(sizeof(AssetSystemConfig) == 48);

// Virtual file system (.enpak) mount coordinator, on-demand range streamer, time-sliced job queue, and GPU texture uploader.
// Move-only RAII owner of AssetBank, a 64B-aligned VFS arena, and up to kMaxMountedPacks open SDL_IOStream file handles.
// Invariant: Higher-priority .enpak mounts override lower-priority entries in 1 Robin Hood probe; file mounts stream payloads on demand via SDL_SeekIO + SDL_ReadIO without loading entire archives into RAM.
class alignas(64) AssetSystem {
public:
    static constexpr u32 kMaxPendingJobs = 128U;
    static constexpr u16 kMaxMountedPacks = 16U;
    static constexpr u16 kMemoryPackIndex = 0xFFFFU;

    // 48-byte (zero padding) unified VFS table-of-contents slot.
    // When baseMemory != nullptr, payload resides in RAM; when baseMemory == nullptr, mountIndex indexes m_packStreams for on-demand range reads.
    struct alignas(8) VfsTocSlot {
        EnPakEntry entry{};
        const u8* baseMemory{nullptr};
        i32 priority{-32768};
        u16 mountIndex{kMemoryPackIndex};
        u16 flags{0};
    };
    static_assert(sizeof(VfsTocSlot) == 48);

    // 16-byte (4 per cache line, zero padding) queued background streaming job.
    struct alignas(16) PendingJob {
        u64 assetId{0};
        TextureAssetHandle backingTex{};
        u16 masterSlot{0};
        u8 scope{kInvalidScope};
        AssetResidency residency{AssetResidency::Stream};
    };
    static_assert(sizeof(PendingJob) == 16);

    AssetSystem() noexcept = default;
    ~AssetSystem() noexcept { Shutdown(); }

    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    // Mounts an in-memory .enpak archive. When copyToArena is false, references pakBytes directly (zero-copy).
    [[nodiscard]] Result<void, AssetErrc> MountPackMemory(
        std::span<const u8> pakBytes,
        i32 priority = 0,
        bool copyToArena = true) noexcept;

    // Mounts an .enpak archive on disk via SDL_IOStream, validating only its header and TOC and streaming asset payloads on demand.
    [[nodiscard]] Result<void, AssetErrc> MountPackFile(
        std::string_view filePath,
        i32 priority = 0) noexcept;

    [[nodiscard]] Result<u8, AssetErrc> BeginScope() noexcept { return m_bank.BeginScope(); }
    void EndScope(u8 scope) noexcept { m_bank.EndScope(scope); }

    [[nodiscard]] u16 Pin(
        u8 scope,
        AssetId id,
        AssetKind kind,
        AssetResidency residency = AssetResidency::Sim) noexcept;

    [[nodiscard]] TextureAssetHandle PinTexture(
        u8 scope,
        AssetId id,
        AssetResidency residency = AssetResidency::Render) noexcept;

    [[nodiscard]] AtlasHandle PinAtlas(
        u8 scope,
        AssetId id,
        TextureAssetHandle backingTex = {},
        AssetResidency residency = AssetResidency::Render) noexcept;

    [[nodiscard]] BlobHandle PinBlob(
        u8 scope,
        AssetId id,
        AssetResidency residency = AssetResidency::Sim) noexcept;

    [[nodiscard]] AudioHandle PinAudio(
        u8 scope,
        AssetId id,
        AssetResidency residency = AssetResidency::Sim) noexcept;

    static void XorCipherPayload(
        std::span<u8> data,
        u64 keySeed = 0xA5A5F0F09E3779B9ULL) noexcept;

    [[nodiscard]] static u32 ComputeTocCrc32(std::span<const EnPakEntry> entries) noexcept;

    // Executes up to maxJobs queued streaming decodes, uploads pending textures to Renderer, and enforces VRAM budget.
    void Pump(Renderer* optionalRenderer = nullptr, u32 maxJobs = 64U) noexcept;

    // Synchronously drains any pending jobs required for scope to reach IsScopeSimReady(scope) == true.
    bool WaitSimReady(u8 scope, Renderer* optionalRenderer = nullptr) noexcept;

    [[nodiscard]] constexpr u32 PendingJobCount() const noexcept { return m_jobRing.Size(); }
    [[nodiscard]] constexpr u16 MountCount() const noexcept { return m_mountCount; }
    [[nodiscard]] constexpr AssetBank& Bank() noexcept { return m_bank; }
    [[nodiscard]] constexpr const AssetBank& Bank() const noexcept { return m_bank; }

private:
    bool LoadSlotImmediate(
        u16 masterSlot,
        AssetId id,
        u8 scope,
        AssetResidency residency,
        TextureAssetHandle optionalBackingTex = {}) noexcept;

    AssetBank m_bank{};
    AlignedArena m_vfsArena{};
    RobinHoodMap<VfsTocSlot> m_vfsTable{};
    RingBuffer<PendingJob, kMaxPendingJobs> m_jobRing{};
    std::array<SDL_IOStream*, kMaxMountedPacks> m_packStreams{};
    u16 m_mountCount{0};
    u16 m_maxMounts{kMaxMountedPacks};
    bool m_initialized{false};
};
static_assert((sizeof(AssetSystem) % 64U) == 0U);

} // namespace en
