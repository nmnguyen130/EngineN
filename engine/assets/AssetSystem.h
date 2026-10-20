#pragma once

#include "assets/AssetBank.h"

namespace en {

class Renderer2D;

struct AssetSystemConfig {
    AssetBankConfig bank{};
    usize vfsArenaBytes{2 * 1024 * 1024};
    u16 maxMounts{16};
    u16 maxVfsEntries{2048};
};

// Virtual file system (.enpak) mount coordinator and scope-pinned streaming pipeline.
class alignas(64) AssetSystem {
public:
    struct alignas(16) VfsTocSlot {
        EnPakEntry entry{};
        const u8* baseMemory{nullptr};
        i32 priority{-32768};
    };

    AssetSystem() noexcept = default;
    ~AssetSystem() noexcept { Shutdown(); }

    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;

    [[nodiscard]] Result<void, AssetErrc> Init(const AssetSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<void, AssetErrc> MountPackMemory(std::span<const u8> pakBytes, i32 priority = 0) noexcept;

    [[nodiscard]] Result<ScopeId, AssetErrc> BeginScope() noexcept { return m_bank.BeginScope(); }
    void EndScope(ScopeId scope) noexcept { m_bank.EndScope(scope); }

    [[nodiscard]] u16 Pin(ScopeId scope, AssetId id, AssetKind kind, AssetResidency residency = AssetResidency::Sim) noexcept;
    [[nodiscard]] TextureHandle PinTexture(ScopeId scope, AssetId id, AssetResidency residency = AssetResidency::Render) noexcept;
    [[nodiscard]] AtlasHandle PinAtlas(ScopeId scope, AssetId id, TextureHandle backingTex, AssetResidency residency = AssetResidency::Render) noexcept;
    [[nodiscard]] BlobHandle PinBlob(ScopeId scope, AssetId id, AssetResidency residency = AssetResidency::Sim) noexcept;
    [[nodiscard]] AudioHandle PinAudio(ScopeId scope, AssetId id, AssetResidency residency = AssetResidency::Sim) noexcept;

    struct HotWatchEntry {
        AssetId id{};
        FixedString<256> path{};
        u64 lastModTick{0};
        bool active{false};
    };

    bool WatchAssetFile(AssetId id, std::string_view diskPath) noexcept;
    u32 PumpHotReload() noexcept;

    static void XorCipherPayload(std::span<u8> data, u64 keySeed = 0xA5A5F0F09E3779B9ULL) noexcept;
    [[nodiscard]] static u32 ComputeTocCrc32(std::span<const EnPakEntry> entries) noexcept;

    void Pump(Renderer2D* optionalRenderer = nullptr, u32 maxJobs = 64) noexcept;
    void PumpHeadless(u32 maxJobs = 64) noexcept { Pump(nullptr, maxJobs); }

    [[nodiscard]] constexpr AssetBank& Bank() noexcept { return m_bank; }
    [[nodiscard]] constexpr const AssetBank& Bank() const noexcept { return m_bank; }

private:
    bool LoadSlotImmediate(u16 masterSlot, AssetId id, TextureHandle optionalBackingTex = {}) noexcept;

    AssetBank m_bank{};
    AlignedArena m_vfsArena{};
    RobinHoodMap<VfsTocSlot> m_vfsTable{};
    std::array<HotWatchEntry, 16> m_hotWatches{};
    u16 m_mountCount{0};
    bool m_initialized{false};
};

} // namespace en
