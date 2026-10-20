#include "assets/AssetSystem.h"
#include "renderer/Renderer2D.h"
#include "core/Serialization.h"
#include <filesystem>
#include <fstream>

namespace en {

Result<void, AssetErrc> AssetSystem::Init(const AssetSystemConfig& cfg) noexcept {
    Shutdown();
    if (auto r = m_bank.Init(cfg.bank); !r) return r;
    if (!m_vfsArena.Init(cfg.vfsArenaBytes)) {
        m_bank.Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }
    if (!m_vfsTable.Init(m_vfsArena, cfg.maxVfsEntries * 2U)) {
        Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }
    m_mountCount = 0;
    m_initialized = true;
    return {};
}

void AssetSystem::Shutdown() noexcept {
    m_vfsArena.Shutdown();
    m_bank.Shutdown();
    m_mountCount = 0;
    m_initialized = false;
}

Result<void, AssetErrc> AssetSystem::MountPackMemory(std::span<const u8> pakBytes, i32 priority) noexcept {
    if (!m_initialized) return Err(AssetErrc::InvalidConfig);
    if (pakBytes.size() < sizeof(EnPakHeader)) return Err(AssetErrc::CorruptedPackHeader);

    EnPakHeader hdr{};
    std::memcpy(&hdr, pakBytes.data(), sizeof(EnPakHeader));
    if (hdr.magic != kEnPakMagic) return Err(AssetErrc::CorruptedPackHeader);

    const usize tocBytes = static_cast<usize>(hdr.entryCount) * sizeof(EnPakEntry);
    if (sizeof(EnPakHeader) + tocBytes > pakBytes.size()) {
        return Err(AssetErrc::CorruptedPackHeader);
    }

    std::span<u8> ownedPak = m_vfsArena.Carve<u8>(pakBytes.size());
    if (ownedPak.empty()) return Err(AssetErrc::OutOfMemory);
    std::memcpy(ownedPak.data(), pakBytes.data(), pakBytes.size());

    const auto* entries = reinterpret_cast<const EnPakEntry*>(ownedPak.data() + sizeof(EnPakHeader));
    if (hdr.headerCrc32 != 0U) {
        const u32 actualTocCrc = ComputeTocCrc32(std::span<const EnPakEntry>(entries, hdr.entryCount));
        if (actualTocCrc != hdr.headerCrc32) {
            return Err(AssetErrc::CorruptedPackHeader);
        }
    }
    for (u32 i = 0; i < hdr.entryCount; ++i) {
        const EnPakEntry& e = entries[i];
        if (static_cast<usize>(e.offset) + e.compressedSize > ownedPak.size()) {
            return Err(AssetErrc::CorruptedPayload);
        }
        if (const VfsTocSlot* existing = m_vfsTable.Find(e.assetId)) {
            if (existing->priority > priority) continue;
        }
        (void)m_vfsTable.InsertOrAssign(e.assetId, VfsTocSlot{e, ownedPak.data(), priority});
    }
    ++m_mountCount;
    return {};
}

bool AssetSystem::LoadSlotImmediate(u16 masterSlot, AssetId id, TextureHandle optionalBackingTex) noexcept {
    const VfsTocSlot* vfs = m_vfsTable.Find(id.value);
    if (!vfs || !vfs->baseMemory) {
        m_bank.MarkFailed(masterSlot);
        return false;
    }
    const EnPakEntry& e = vfs->entry;
    std::span<const u8> rawPayload(vfs->baseMemory + e.offset, e.compressedSize);
    std::span<const u8> decoded = rawPayload;

    if (e.compressed != 0) {
        std::span<u8> decompBuf = m_bank.AllocatePayloadSlice(e.uncompressedSize);
        if (decompBuf.empty()) {
            m_bank.MarkFailed(masterSlot);
            return false;
        }
        if (e.compressed == 2) {
            std::span<u8> cipherScratch = m_vfsArena.Carve<u8>(rawPayload.size());
            if (cipherScratch.empty()) {
                m_bank.MarkFailed(masterSlot);
                return false;
            }
            std::memcpy(cipherScratch.data(), rawPayload.data(), rawPayload.size());
            XorCipherPayload(cipherScratch, e.assetId);
            auto res = AssetBank::DecompressLz4(cipherScratch, decompBuf);
            if (!res || *res != e.uncompressedSize) {
                m_bank.MarkFailed(masterSlot);
                return false;
            }
        } else {
            auto res = AssetBank::DecompressLz4(rawPayload, decompBuf);
            if (!res || *res != e.uncompressedSize) {
                m_bank.MarkFailed(masterSlot);
                return false;
            }
        }
        decoded = decompBuf;
    }

    switch (e.kind) {
        case AssetKind::Texture:
            (void)m_bank.CommitTexture(masterSlot, e.widthOrRate, e.heightOrChannels, decoded);
            return true;
        case AssetKind::Atlas: {
            const usize count = decoded.size() / sizeof(CookedAtlasSpriteEntry);
            std::span<const CookedAtlasSpriteEntry> sprites(
                reinterpret_cast<const CookedAtlasSpriteEntry*>(decoded.data()), count);
            (void)m_bank.CommitAtlas(masterSlot, optionalBackingTex, sprites);
            return true;
        }
        case AssetKind::Blob:
            (void)m_bank.CommitBlob(masterSlot, decoded, e.schemaOrMeta);
            return true;
        case AssetKind::Audio: {
            const usize sampleCount = decoded.size() / sizeof(f32);
            std::span<const f32> pcm(reinterpret_cast<const f32*>(decoded.data()), sampleCount);
            (void)m_bank.CommitAudio(
                masterSlot, pcm, e.widthOrRate > 0 ? e.widthOrRate : 48000U,
                static_cast<u8>(e.heightOrChannels > 0 ? e.heightOrChannels : 2U));
            return true;
        }
        default:
            m_bank.MarkFailed(masterSlot);
            return false;
    }
}

u16 AssetSystem::Pin(ScopeId scope, AssetId id, AssetKind kind, AssetResidency residency) noexcept {
    bool newlyCreated = false;
    const u16 slot = m_bank.AcquireSlot(id, kind, scope, residency, newlyCreated);
    if (slot != 0 && m_bank.GetSlotMeta(slot).state == AssetState::Loading) {
        LoadSlotImmediate(slot, id);
    }
    return slot;
}

TextureHandle AssetSystem::PinTexture(ScopeId scope, AssetId id, AssetResidency residency) noexcept {
    const u16 slot = Pin(scope, id, AssetKind::Texture, residency);
    if (slot == 0) return {};
    const SlotMeta& m = m_bank.GetSlotMeta(slot);
    return m.state == AssetState::Ready ? TextureHandle{m.payloadSlot, m.generation} : TextureHandle{};
}

AtlasHandle AssetSystem::PinAtlas(
    ScopeId scope, AssetId id, TextureHandle backingTex, AssetResidency residency) noexcept {
    bool newlyCreated = false;
    const u16 slot = m_bank.AcquireSlot(id, AssetKind::Atlas, scope, residency, newlyCreated);
    if (slot != 0 && m_bank.GetSlotMeta(slot).state == AssetState::Loading) {
        LoadSlotImmediate(slot, id, backingTex);
    }
    const SlotMeta& m = m_bank.GetSlotMeta(slot);
    return (slot != 0 && m.state == AssetState::Ready) ? AtlasHandle{m.payloadSlot, m.generation} : AtlasHandle{};
}

BlobHandle AssetSystem::PinBlob(ScopeId scope, AssetId id, AssetResidency residency) noexcept {
    const u16 slot = Pin(scope, id, AssetKind::Blob, residency);
    if (slot == 0) return {};
    const SlotMeta& m = m_bank.GetSlotMeta(slot);
    return m.state == AssetState::Ready ? BlobHandle{m.payloadSlot, m.generation} : BlobHandle{};
}

AudioHandle AssetSystem::PinAudio(ScopeId scope, AssetId id, AssetResidency residency) noexcept {
    const u16 slot = Pin(scope, id, AssetKind::Audio, residency);
    if (slot == 0) return {};
    const SlotMeta& m = m_bank.GetSlotMeta(slot);
    return m.state == AssetState::Ready ? AudioHandle{m.payloadSlot, m.generation} : AudioHandle{};
}


void AssetSystem::XorCipherPayload(std::span<u8> data, u64 keySeed) noexcept {
    u64 state = (keySeed != 0) ? keySeed : 0xA5A5F0F09E3779B9ULL;
    for (usize i = 0; i < data.size(); ++i) {
        if ((i & 7U) == 0U) state = Mix64(state);
        data[i] ^= static_cast<u8>((state >> ((i & 7U) * 8U)) & 0xFFU);
    }
}

u32 AssetSystem::ComputeTocCrc32(std::span<const EnPakEntry> entries) noexcept {
    return ComputeCrc32(std::span<const u8>(
        reinterpret_cast<const u8*>(entries.data()), entries.size() * sizeof(EnPakEntry)));
}

bool AssetSystem::WatchAssetFile(AssetId id, std::string_view diskPath) noexcept {
    if (!id.IsValid() || diskPath.empty()) return false;
    for (HotWatchEntry& w : m_hotWatches) {
        if (!w.active || w.id == id) {
            w.id = id;
            w.path.Assign(diskPath);
            std::error_code ec{};
            const auto ftime = std::filesystem::last_write_time(w.path.CStr(), ec);
            w.lastModTick = ec ? 0ULL : static_cast<u64>(ftime.time_since_epoch().count());
            w.active = true;
            return true;
        }
    }
    return false;
}

u32 AssetSystem::PumpHotReload() noexcept {
    u32 reloaded = 0;
    for (HotWatchEntry& w : m_hotWatches) {
        if (!w.active) continue;
        std::error_code ec{};
        const auto ftime = std::filesystem::last_write_time(w.path.CStr(), ec);
        if (ec) continue;
        const u64 modTick = static_cast<u64>(ftime.time_since_epoch().count());
        if (modTick != w.lastModTick) {
            w.lastModTick = modTick;
            const auto fsize = std::filesystem::file_size(w.path.CStr(), ec);
            if (!ec && fsize > 0 && fsize <= 64 * 1024) {
                std::span<u8> scratch = m_vfsArena.Carve<u8>(static_cast<usize>(fsize));
                if (!scratch.empty()) {
                    std::ifstream ifs(w.path.CStr(), std::ios::binary);
                    if (ifs.is_open()) {
                        ifs.read(reinterpret_cast<char*>(scratch.data()), static_cast<std::streamsize>(fsize));
                        if (m_bank.HotReloadBlob(w.id, scratch)) {
                            ++reloaded;
                        }
                    }
                }
            }
        }
    }
    return reloaded;
}

void AssetSystem::Pump(Renderer2D* optionalRenderer, u32 maxJobs) noexcept {
    (void)PumpHotReload();
    if (!optionalRenderer) return;
    u32 uploaded = 0;
    for (u16 t = 1; t < m_bank.HighWaterTex() && uploaded < maxJobs; ++t) {
        const TextureRecord& rec = m_bank.GetTextureBySlot(t);
        if (rec.gpu == kWhiteGpuTexture && !rec.pixels.empty() && rec.width > 0 && rec.height > 0) {
            const GpuTextureHandle gpu = optionalRenderer->CreateTexture(rec.width, rec.height, rec.pixels);
            if (gpu.IsValid() && gpu != kWhiteGpuTexture) {
                m_bank.BindGpuTexture(t, gpu);
                ++uploaded;
            }
        }
    }
}

} // namespace en
