#include "assets/AssetSystem.h"
#include "renderer/Renderer.h"
#include <SDL3/SDL.h>

namespace en {

Result<void, AssetErrc> AssetSystem::Init(const AssetSystemConfig& cfg) noexcept {
    Shutdown();
    if (cfg.vfsArenaBytes < 16384U || cfg.maxMounts == 0U || cfg.maxVfsEntries < 4U) {
        return Err(AssetErrc::InvalidConfig);
    }
    if (auto r = m_bank.Init(cfg.bank); !r.has_value()) {
        return r;
    }
    if (!m_vfsArena.Init(cfg.vfsArenaBytes)) {
        m_bank.Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }
    if (!m_vfsTable.Init(m_vfsArena, static_cast<u32>(cfg.maxVfsEntries) * 2U)) {
        Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }
    m_jobRing.Clear();
    m_packStreams.fill(nullptr);
    m_mountCount = 0U;
    m_maxMounts = std::min<u16>(cfg.maxMounts, kMaxMountedPacks);
    m_initialized = true;
    return {};
}

void AssetSystem::Shutdown() noexcept {
    for (SDL_IOStream*& io : m_packStreams) {
        if (io != nullptr) {
            SDL_CloseIO(io);
            io = nullptr;
        }
    }
    m_jobRing.Clear();
    m_vfsArena.Shutdown();
    m_bank.Shutdown();
    m_mountCount = 0U;
    m_initialized = false;
}

Result<void, AssetErrc> AssetSystem::MountPackMemory(
    std::span<const u8> pakBytes,
    i32 priority,
    bool copyToArena) noexcept {
    if (!m_initialized) return Err(AssetErrc::InvalidConfig);
    if (m_mountCount >= m_maxMounts) return Err(AssetErrc::MountLimitReached);
    if (pakBytes.size() < sizeof(EnPakHeader)) return Err(AssetErrc::CorruptedPackHeader);

    EnPakHeader hdr{};
    std::memcpy(&hdr, pakBytes.data(), sizeof(EnPakHeader));
    if (hdr.magic != kEnPakMagic) return Err(AssetErrc::CorruptedPackHeader);

    const usize tocBytes = static_cast<usize>(hdr.entryCount) * sizeof(EnPakEntry);
    if (sizeof(EnPakHeader) + tocBytes > pakBytes.size()) {
        return Err(AssetErrc::CorruptedPackHeader);
    }

    const u8* basePtr = pakBytes.data();
    const u32 marker = m_vfsArena.SaveMarker();
    if (copyToArena) {
        std::span<u8> ownedPak = m_vfsArena.Carve<u8>(pakBytes.size());
        if (ownedPak.empty()) return Err(AssetErrc::OutOfMemory);
        std::memcpy(ownedPak.data(), pakBytes.data(), pakBytes.size());
        basePtr = ownedPak.data();
    }

    const auto* entries = reinterpret_cast<const EnPakEntry*>(basePtr + sizeof(EnPakHeader));
    if (hdr.headerCrc32 != 0U) {
        const u32 actualTocCrc = ComputeTocCrc32(std::span<const EnPakEntry>(entries, hdr.entryCount));
        if (actualTocCrc != hdr.headerCrc32) {
            if (copyToArena) m_vfsArena.RollbackToMarker(marker);
            return Err(AssetErrc::CorruptedPackHeader);
        }
    }

    // Validate all entry payload bounds before mutating the VFS table
    for (u32 i = 0U; i < hdr.entryCount; ++i) {
        const EnPakEntry& e = entries[i];
        if (e.assetId == 0ULL ||
            static_cast<usize>(e.offset) > pakBytes.size() ||
            static_cast<usize>(e.compressedSize) > (pakBytes.size() - static_cast<usize>(e.offset))) {
            if (copyToArena) m_vfsArena.RollbackToMarker(marker);
            return Err(AssetErrc::CorruptedPayload);
        }
    }

    for (u32 i = 0U; i < hdr.entryCount; ++i) {
        const EnPakEntry& e = entries[i];
        if (const VfsTocSlot* existing = m_vfsTable.Find(e.assetId)) {
            if (existing->priority > priority) continue;
        }
        (void)m_vfsTable.InsertOrAssign(
            e.assetId,
            VfsTocSlot{e, basePtr, priority, kMemoryPackIndex, 0U});
    }

    ++m_mountCount;
    return {};
}

Result<void, AssetErrc> AssetSystem::MountPackFile(
    std::string_view filePath,
    i32 priority) noexcept {
    if (!m_initialized || filePath.empty()) return Err(AssetErrc::InvalidConfig);
    if (m_mountCount >= m_maxMounts) return Err(AssetErrc::MountLimitReached);

    u16 streamSlot = kMemoryPackIndex;
    for (u16 i = 0U; i < m_maxMounts; ++i) {
        if (m_packStreams[i] == nullptr) {
            streamSlot = i;
            break;
        }
    }
    if (streamSlot == kMemoryPackIndex) return Err(AssetErrc::MountLimitReached);

    FixedString<512> pathBuf{};
    if (!pathBuf.Assign(filePath)) return Err(AssetErrc::InvalidConfig);

    SDL_IOStream* io = SDL_IOFromFile(pathBuf.CStr(), "rb");
    if (!io) return Err(AssetErrc::AssetNotFound);

    EnPakHeader hdr{};
    if (SDL_ReadIO(io, &hdr, sizeof(EnPakHeader)) != sizeof(EnPakHeader) ||
        hdr.magic != kEnPakMagic) {
        SDL_CloseIO(io);
        return Err(AssetErrc::CorruptedPackHeader);
    }

    // Read and validate ONLY the TOC entries using temporary rewound scratch (0 persistent VFS arena bytes!)
    ArenaTempScope tocTemp{m_vfsArena};
    auto tocScratch = m_vfsArena.Carve<EnPakEntry>(hdr.entryCount);
    if (hdr.entryCount > 0U && tocScratch.empty()) {
        SDL_CloseIO(io);
        return Err(AssetErrc::OutOfMemory);
    }

    const usize tocBytes = static_cast<usize>(hdr.entryCount) * sizeof(EnPakEntry);
    if (tocBytes > 0U && SDL_ReadIO(io, tocScratch.data(), tocBytes) != tocBytes) {
        SDL_CloseIO(io);
        return Err(AssetErrc::CorruptedPackHeader);
    }

    if (hdr.headerCrc32 != 0U) {
        const u32 actualCrc = ComputeTocCrc32(tocScratch);
        if (actualCrc != hdr.headerCrc32) {
            SDL_CloseIO(io);
            return Err(AssetErrc::CorruptedPackHeader);
        }
    }

    for (const EnPakEntry& e : tocScratch) {
        if (e.assetId == 0ULL || e.offset < sizeof(EnPakHeader) + tocBytes) {
            SDL_CloseIO(io);
            return Err(AssetErrc::CorruptedPayload);
        }
    }

    for (const EnPakEntry& e : tocScratch) {
        if (const VfsTocSlot* existing = m_vfsTable.Find(e.assetId)) {
            if (existing->priority > priority) continue;
        }
        (void)m_vfsTable.InsertOrAssign(
            e.assetId,
            VfsTocSlot{e, nullptr, priority, streamSlot, 0U});
    }

    m_packStreams[streamSlot] = io;
    ++m_mountCount;
    return {};
}

bool AssetSystem::LoadSlotImmediate(
    u16 masterSlot,
    AssetId id,
    u8 scope,
    AssetResidency residency,
    TextureAssetHandle optionalBackingTex) noexcept {
    const VfsTocSlot* vfs = m_vfsTable.Find(id.value);
    if (!vfs) {
        m_bank.MarkFailed(masterSlot);
        return false;
    }

    const EnPakEntry& e = vfs->entry;

    // Automatic Atlas-to-Texture dependency resolution when pinned via SceneDirector::Pin() or PinAtlas({}, ...)
    if (e.kind == AssetKind::Atlas && !optionalBackingTex.IsValid()) {
        const AssetId depTexId = e.BackingTexture();
        if (depTexId.IsValid()) {
            optionalBackingTex = m_bank.FindTextureHandle(depTexId);
            if (!optionalBackingTex.IsValid() && scope < kMaxAssetScopes) {
                optionalBackingTex = PinTexture(scope, depTexId, residency);
            }
        }
    }

    // Procedural uniform grid atlas support inside .enpak (uncompressedSize == 0, widthOrRate = cols, heightOrChannels = rows)
    if (e.kind == AssetKind::Atlas && e.uncompressedSize == 0U && e.compressedSize == 0U &&
        e.widthOrRate > 0U && e.heightOrChannels > 0U) {
        return m_bank.CommitGridAtlas(masterSlot, optionalBackingTex, e.widthOrRate, e.heightOrChannels).IsValid();
    }

    const bool isFileMount = (vfs->baseMemory == nullptr);
    SDL_IOStream* fileIo = nullptr;
    if (isFileMount) {
        if (vfs->mountIndex >= kMaxMountedPacks || m_packStreams[vfs->mountIndex] == nullptr) {
            m_bank.MarkFailed(masterSlot);
            return false;
        }
        fileIo = m_packStreams[vfs->mountIndex];
        if (SDL_SeekIO(fileIo, static_cast<Sint64>(e.offset), SDL_IO_SEEK_SET) < 0) {
            m_bank.MarkFailed(masterSlot);
            return false;
        }
    }

    // Fast path for uncompressed file-mounted Texture/Blob: read directly into m_bank's payload arena with zero VFS copy
    if (isFileMount && e.compressed == 0U && (e.kind == AssetKind::Texture || e.kind == AssetKind::Blob)) {
        std::span<u8> dstSlice = m_bank.AllocatePayloadSlice(e.uncompressedSize);
        if (e.uncompressedSize > 0U && dstSlice.empty()) {
            m_bank.MarkFailed(masterSlot);
            return false;
        }
        if (e.uncompressedSize > 0U && SDL_ReadIO(fileIo, dstSlice.data(), e.uncompressedSize) != e.uncompressedSize) {
            m_bank.FreePayloadSlice(dstSlice.data(), e.uncompressedSize);
            m_bank.MarkFailed(masterSlot);
            return false;
        }
        if (e.kind == AssetKind::Texture) {
            return m_bank.CommitTexture(masterSlot, e.widthOrRate, e.heightOrChannels, dstSlice, kWhiteTexture, true).IsValid();
        }
        return m_bank.CommitBlob(masterSlot, dstSlice, true).IsValid();
    }

    ArenaTempScope vfsTemp{m_vfsArena};
    std::span<const u8> rawPayload{};
    if (isFileMount) {
        std::span<u8> fileReadScratch = m_vfsArena.Carve<u8>(e.compressedSize);
        if (e.compressedSize > 0U && fileReadScratch.empty()) {
            m_bank.MarkFailed(masterSlot);
            return false;
        }
        if (e.compressedSize > 0U && SDL_ReadIO(fileIo, fileReadScratch.data(), e.compressedSize) != e.compressedSize) {
            m_bank.MarkFailed(masterSlot);
            return false;
        }
        rawPayload = fileReadScratch;
    } else {
        rawPayload = std::span<const u8>(vfs->baseMemory + e.offset, e.compressedSize);
    }

    std::span<const u8> decoded = rawPayload;
    std::span<u8> decompBuf{};
    const bool isCompressed = (e.compressed != 0U);

    if (isCompressed) {
        decompBuf = m_bank.AllocatePayloadSlice(e.uncompressedSize);
        if (e.uncompressedSize > 0U && decompBuf.empty()) {
            m_bank.MarkFailed(masterSlot);
            return false;
        }
        if (e.compressed == 2U) {
            std::span<u8> cipherScratch = m_vfsArena.Carve<u8>(rawPayload.size());
            if (!rawPayload.empty() && cipherScratch.empty()) {
                m_bank.FreePayloadSlice(decompBuf.data(), e.uncompressedSize);
                m_bank.MarkFailed(masterSlot);
                return false;
            }
            if (!rawPayload.empty()) {
                std::memcpy(cipherScratch.data(), rawPayload.data(), rawPayload.size());
                XorCipherPayload(cipherScratch, e.assetId);
            }
            auto res = AssetBank::DecompressLz4(cipherScratch, decompBuf);
            if (!res.has_value() || *res != e.uncompressedSize) {
                m_bank.FreePayloadSlice(decompBuf.data(), e.uncompressedSize);
                m_bank.MarkFailed(masterSlot);
                return false;
            }
        } else {
            auto res = AssetBank::DecompressLz4(rawPayload, decompBuf);
            if (!res.has_value() || *res != e.uncompressedSize) {
                m_bank.FreePayloadSlice(decompBuf.data(), e.uncompressedSize);
                m_bank.MarkFailed(masterSlot);
                return false;
            }
        }
        decoded = decompBuf;
    }

    switch (e.kind) {
        case AssetKind::Texture: {
            // If decompressed into m_bank or in-memory VFS pack, zero-copy commit!
            const auto h = m_bank.CommitTexture(
                masterSlot, e.widthOrRate, e.heightOrChannels, decoded, kWhiteTexture, true);
            return h.IsValid();
        }
        case AssetKind::Atlas: {
            const usize count = decoded.size() / sizeof(CookedAtlasSpriteEntry);
            std::span<const CookedAtlasSpriteEntry> sprites(
                reinterpret_cast<const CookedAtlasSpriteEntry*>(decoded.data()), count);
            const auto h = m_bank.CommitAtlas(masterSlot, optionalBackingTex, sprites);
            if (isCompressed && !decompBuf.empty()) {
                // Atlas entries were copied into m_sprites; reclaim temporary decompression slice immediately!
                m_bank.FreePayloadSlice(decompBuf.data(), e.uncompressedSize);
            }
            return h.IsValid();
        }
        case AssetKind::Blob: {
            // Zero-copy commit: points directly to decompressed slice in m_bank or raw slice in memory VFS pack!
            const auto h = m_bank.CommitBlob(masterSlot, decoded, true);
            return h.IsValid();
        }
        case AssetKind::Audio: {
            const usize sampleCount = decoded.size() / sizeof(f32);
            std::span<const f32> pcm(reinterpret_cast<const f32*>(decoded.data()), sampleCount);
            const auto h = m_bank.CommitAudio(
                masterSlot,
                pcm,
                e.widthOrRate > 0U ? e.widthOrRate : 48000U,
                static_cast<u8>(e.heightOrChannels > 0U ? e.heightOrChannels : 2U),
                e.schemaOrMeta != 0U);
            if (isCompressed && !decompBuf.empty()) {
                m_bank.FreePayloadSlice(decompBuf.data(), e.uncompressedSize);
            }
            return h.IsValid();
        }
        default:
            if (isCompressed && !decompBuf.empty()) {
                m_bank.FreePayloadSlice(decompBuf.data(), e.uncompressedSize);
            }
            m_bank.MarkFailed(masterSlot);
            return false;
    }
}

u16 AssetSystem::Pin(
    u8 scope,
    AssetId id,
    AssetKind kind,
    AssetResidency residency) noexcept {
    bool needsLoad = false;
    const u16 slot = m_bank.AcquireSlot(id, kind, scope, residency, needsLoad);
    if (slot != 0U && needsLoad) {
        // When pinning an Atlas via generic Pin(), auto-pin its backing texture in the same scope first
        TextureAssetHandle autoTex{};
        if (kind == AssetKind::Atlas) {
            if (const VfsTocSlot* vfs = m_vfsTable.Find(id.value)) {
                const AssetId depTexId = vfs->entry.BackingTexture();
                if (depTexId.IsValid()) {
                    autoTex = PinTexture(scope, depTexId, residency);
                }
            }
        }
        if (residency == AssetResidency::Stream && !m_jobRing.Full()) {
            (void)m_jobRing.Push(PendingJob{id.value, autoTex, slot, scope, residency});
        } else {
            (void)LoadSlotImmediate(slot, id, scope, residency, autoTex);
        }
    }
    return slot;
}

TextureAssetHandle AssetSystem::PinTexture(
    u8 scope,
    AssetId id,
    AssetResidency residency) noexcept {
    const u16 slot = Pin(scope, id, AssetKind::Texture, residency);
    if (slot == 0U) return {};
    const SlotMeta& m = m_bank.GetSlotMeta(slot);
    return (m.payloadSlot != 0U && m.state != AssetState::Failed)
               ? TextureAssetHandle{m.payloadSlot, m.generation}
               : TextureAssetHandle{};
}

AtlasHandle AssetSystem::PinAtlas(
    u8 scope,
    AssetId id,
    TextureAssetHandle backingTex,
    AssetResidency residency) noexcept {
    if (!backingTex.IsValid()) {
        if (const VfsTocSlot* vfs = m_vfsTable.Find(id.value)) {
            const AssetId depTexId = vfs->entry.BackingTexture();
            if (depTexId.IsValid()) {
                backingTex = PinTexture(scope, depTexId, residency);
            }
        }
    }
    bool needsLoad = false;
    const u16 slot = m_bank.AcquireSlot(id, AssetKind::Atlas, scope, residency, needsLoad);
    if (slot != 0U && needsLoad) {
        if (residency == AssetResidency::Stream && !m_jobRing.Full()) {
            (void)m_jobRing.Push(PendingJob{id.value, backingTex, slot, scope, residency});
        } else {
            (void)LoadSlotImmediate(slot, id, scope, residency, backingTex);
        }
    }
    const SlotMeta& m = m_bank.GetSlotMeta(slot);
    return (slot != 0U && m.payloadSlot != 0U && m.state != AssetState::Failed)
               ? AtlasHandle{m.payloadSlot, m.generation}
               : AtlasHandle{};
}

BlobHandle AssetSystem::PinBlob(
    u8 scope,
    AssetId id,
    AssetResidency residency) noexcept {
    const u16 slot = Pin(scope, id, AssetKind::Blob, residency);
    if (slot == 0U) return {};
    const SlotMeta& m = m_bank.GetSlotMeta(slot);
    return (m.payloadSlot != 0U && m.state != AssetState::Failed)
               ? BlobHandle{m.payloadSlot, m.generation}
               : BlobHandle{};
}

AudioHandle AssetSystem::PinAudio(
    u8 scope,
    AssetId id,
    AssetResidency residency) noexcept {
    const u16 slot = Pin(scope, id, AssetKind::Audio, residency);
    if (slot == 0U) return {};
    const SlotMeta& m = m_bank.GetSlotMeta(slot);
    return (m.payloadSlot != 0U && m.state != AssetState::Failed)
               ? AudioHandle{m.payloadSlot, m.generation}
               : AudioHandle{};
}

void AssetSystem::XorCipherPayload(std::span<u8> data, u64 keySeed) noexcept {
    u64 state = (keySeed != 0ULL) ? keySeed : 0xA5A5F0F09E3779B9ULL;
    for (usize i = 0U; i < data.size(); ++i) {
        if ((i & 7U) == 0U) state = Mix64(state);
        data[i] ^= static_cast<u8>((state >> ((i & 7U) * 8U)) & 0xFFU);
    }
}

u32 AssetSystem::ComputeTocCrc32(std::span<const EnPakEntry> entries) noexcept {
    return ComputeCrc32(std::span<const u8>(
        reinterpret_cast<const u8*>(entries.data()), entries.size() * sizeof(EnPakEntry)));
}

void AssetSystem::Pump(Renderer* optionalRenderer, u32 maxJobs) noexcept {
    u32 executedJobs = 0U;
    PendingJob job{};
    while (executedJobs < maxJobs && m_jobRing.Pop(job)) {
        if (m_bank.GetSlotMeta(job.masterSlot).state == AssetState::Loading) {
            (void)LoadSlotImmediate(
                job.masterSlot, AssetId{job.assetId}, job.scope, job.residency, job.backingTex);
            ++executedJobs;
        }
    }

    if (!optionalRenderer) return;

    // O(1) early-out when all CPU-decoded textures have already been uploaded to the GPU
    if (m_bank.PendingGpuTextureCount() > 0U) {
        u32 uploaded = 0U;
        for (u16 t = 1U; t < m_bank.HighWaterTex() && uploaded < maxJobs && m_bank.PendingGpuTextureCount() > 0U; ++t) {
            const TextureRecord& rec = m_bank.GetTextureBySlot(t);
            if (rec.gpu == kWhiteTexture && rec.pixels != nullptr && rec.width > 0U && rec.height > 0U) {
                TextureDesc desc{};
                desc.width = rec.width;
                desc.height = rec.height;
                desc.pitch = static_cast<u32>(rec.width) * 4U;
                desc.pixels = rec.PixelBytes();
                desc.filter = TextureFilter::Nearest;
                const auto gpuRes = optionalRenderer->CreateTexture(desc);
                if (gpuRes.has_value() && gpuRes->IsValid() && *gpuRes != kWhiteTexture) {
                    m_bank.BindGpuTexture(t, *gpuRes);
                    ++uploaded;
                }
            }
        }
    }

    if (m_bank.IsOverVramBudget()) {
        std::array<TextureHandle, 32> destroyedGpu{};
        const u32 evicted = m_bank.EvictLruToVramBudget(destroyedGpu);
        for (u32 i = 0U; i < std::min<u32>(evicted, 32U); ++i) {
            if (destroyedGpu[i].IsValid() && destroyedGpu[i] != kWhiteTexture) {
                optionalRenderer->DestroyTexture(destroyedGpu[i]);
            }
        }
    }
}

bool AssetSystem::WaitSimReady(u8 scope, Renderer* optionalRenderer) noexcept {
    if (scope >= kMaxAssetScopes) return false;
    while (!m_bank.IsScopeSimReady(scope) && !m_jobRing.Empty()) {
        Pump(optionalRenderer, kMaxPendingJobs);
    }
    if (optionalRenderer && m_bank.PendingGpuTextureCount() > 0U) {
        Pump(optionalRenderer, kMaxPendingJobs);
    }
    return m_bank.IsScopeSimReady(scope);
}

} // namespace en
