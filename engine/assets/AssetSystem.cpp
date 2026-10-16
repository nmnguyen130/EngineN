#include "assets/AssetSystem.h"

#include "platform/PlatformTypes.h"
#include "renderer/Renderer.h"

#include <SDL3/SDL_asyncio.h>
#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace en::assets {

namespace {

[[nodiscard]] constexpr u32 NextPow2(u32 v) noexcept {
    if (v <= 16) {
        return 16;
    }
    return 1u << (32u - std::countl_zero(v - 1u));
}

} // namespace

AssetSystem::~AssetSystem() noexcept {
    Shutdown();
}

AssetSystem::AssetSystem(AssetSystem&& other) noexcept
    : m_cfg(other.m_cfg)
    , m_bank(std::move(other.m_bank))
    , m_ioQueue(std::exchange(other.m_ioQueue, nullptr))
    , m_mounts(std::move(other.m_mounts))
    , m_mountCount(std::exchange(other.m_mountCount, 0))
    , m_stagingRing(std::move(other.m_stagingRing))
    , m_ringCap(std::exchange(other.m_ringCap, 0))
    , m_ringWrite(std::exchange(other.m_ringWrite, 0))
    , m_ringUsed(std::exchange(other.m_ringUsed, 0))
    , m_jobs(std::move(other.m_jobs))
    , m_nextSequence(other.m_nextSequence)
    , m_frameCounter(other.m_frameCounter)
    , m_headlessTexCounter(other.m_headlessTexCounter) {}

AssetSystem& AssetSystem::operator=(AssetSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_cfg = other.m_cfg;
        m_bank = std::move(other.m_bank);
        m_ioQueue = std::exchange(other.m_ioQueue, nullptr);
        m_mounts = std::move(other.m_mounts);
        m_mountCount = std::exchange(other.m_mountCount, 0);
        m_stagingRing = std::move(other.m_stagingRing);
        m_ringCap = std::exchange(other.m_ringCap, 0);
        m_ringWrite = std::exchange(other.m_ringWrite, 0);
        m_ringUsed = std::exchange(other.m_ringUsed, 0);
        m_jobs = std::move(other.m_jobs);
        m_nextSequence = other.m_nextSequence;
        m_frameCounter = other.m_frameCounter;
        m_headlessTexCounter = other.m_headlessTexCounter;
    }
    return *this;
}

void AssetSystem::BuildMountHashTable(
    std::span<const PakEntry> entries,
    std::span<MountTocSlot> slots,
    u32 hashMask) noexcept {
    for (u32 i = 0; i < entries.size(); ++i) {
        const u64 key = entries[i].assetId;
        u32 idx = static_cast<u32>((key * 11400714819323198485ULL) >> 32) & hashMask;
        MountTocSlot incoming{key, i, 1};
        for (;;) {
            MountTocSlot& cur = slots[idx];
            if (cur.key == 0) {
                cur = incoming;
                break;
            }
            if (incoming.psl > cur.psl) {
                std::swap(incoming, cur);
            }
            ++incoming.psl;
            idx = (idx + 1u) & hashMask;
        }
    }
}

Result<void, AssetErrc> AssetSystem::Init(const AssetSystemConfig& cfg) noexcept {
    Shutdown();
    m_cfg = cfg;

    if (auto res = m_bank.Init(cfg.bank); !res) {
        return res;
    }

    m_ioQueue = SDL_CreateAsyncIOQueue();
    if (!m_ioQueue) {
        return Err(AssetErrc::IoFailure);
    }

    m_mounts = std::make_unique_for_overwrite<PakMount[]>(cfg.maxMounts);
    for (u8 i = 0; i < cfg.maxMounts; ++i) {
        m_mounts[i] = PakMount{};
    }
    m_mountCount = 0;

    m_ringCap = cfg.stagingRingBytes;
    m_stagingRing = std::make_unique_for_overwrite<u8[]>(m_ringCap);
    m_ringWrite = 0;
    m_ringUsed = 0;

    m_jobs = std::make_unique_for_overwrite<PendingJob[]>(cfg.maxInFlightJobs);
    for (u16 i = 0; i < cfg.maxInFlightJobs; ++i) {
        m_jobs[i] = PendingJob{};
    }

    return {};
}

void AssetSystem::Shutdown() noexcept {
    if (m_mounts) {
        for (u8 i = 0; i < m_mountCount; ++i) {
            if (m_mounts[i].asyncFile) {
                SDL_CloseAsyncIO(m_mounts[i].asyncFile, false, m_ioQueue, nullptr);
                m_mounts[i].asyncFile = nullptr;
            }
        }
    }
    if (m_ioQueue) {
        SDL_DestroyAsyncIOQueue(m_ioQueue);
        m_ioQueue = nullptr;
    }
    m_mounts.reset();
    m_stagingRing.reset();
    m_jobs.reset();
    m_bank.Shutdown();
    m_mountCount = 0;
    m_ringCap = 0;
    m_ringUsed = 0;
}

Result<void, AssetErrc> AssetSystem::MountPackMemory(
    std::span<const u8> mappedPak,
    u8 priority) noexcept {
    if (m_mountCount >= m_cfg.maxMounts) {
        return Err(AssetErrc::PoolExhausted);
    }
    if (mappedPak.size() < sizeof(PakHeader)) {
        return Err(AssetErrc::CorruptedHeader);
    }

    PakHeader hdr{};
    std::memcpy(&hdr, mappedPak.data(), sizeof(PakHeader));
    if (hdr.magic != kPakMagic || hdr.version != kPakVersion) {
        return Err(AssetErrc::CorruptedHeader);
    }

    const std::size_t tocBytes = static_cast<std::size_t>(hdr.entryCount) * sizeof(PakEntry);
    if (hdr.tocOffset + tocBytes > mappedPak.size()) {
        return Err(AssetErrc::CorruptedHeader);
    }

    PakMount& m = m_mounts[m_mountCount++];
    m.mappedBase = mappedPak.data();
    m.mappedSize = mappedPak.size();
    m.entryCount = hdr.entryCount;
    m.priority = priority;
    m.active = true;

    m.entries = std::make_unique_for_overwrite<PakEntry[]>(hdr.entryCount);
    std::memcpy(m.entries.get(), mappedPak.data() + hdr.tocOffset, tocBytes);

    const u32 hashCap = NextPow2(hdr.entryCount * 2u);
    m.hashMask = hashCap - 1u;
    m.tocHash = std::make_unique<MountTocSlot[]>(hashCap);

    BuildMountHashTable(
        std::span<const PakEntry>(m.entries.get(), hdr.entryCount),
        std::span<MountTocSlot>(m.tocHash.get(), hashCap),
        m.hashMask);

    return {};
}

Result<void, AssetErrc> AssetSystem::MountPackFile(
    std::string_view filePath,
    u8 priority) noexcept {
    if (m_mountCount >= m_cfg.maxMounts) {
        return Err(AssetErrc::PoolExhausted);
    }

    platform::FilePath pathBuf;
    if (!pathBuf.Assign(filePath) || pathBuf.Empty()) {
        return Err(AssetErrc::InvalidMount);
    }

    SDL_IOStream* syncIo = SDL_IOFromFile(pathBuf.CStr(), "rb");
    if (!syncIo) {
        return Err(AssetErrc::IoFailure);
    }

    PakHeader hdr{};
    if (SDL_ReadIO(syncIo, &hdr, sizeof(PakHeader)) != sizeof(PakHeader) ||
        hdr.magic != kPakMagic || hdr.version != kPakVersion) {
        SDL_CloseIO(syncIo);
        return Err(AssetErrc::CorruptedHeader);
    }

    auto entries = std::make_unique_for_overwrite<PakEntry[]>(hdr.entryCount);
    const std::size_t tocBytes = static_cast<std::size_t>(hdr.entryCount) * sizeof(PakEntry);
    if (SDL_SeekIO(syncIo, static_cast<Sint64>(hdr.tocOffset), SDL_IO_SEEK_SET) < 0 ||
        SDL_ReadIO(syncIo, entries.get(), tocBytes) != tocBytes) {
        SDL_CloseIO(syncIo);
        return Err(AssetErrc::CorruptedHeader);
    }
    SDL_CloseIO(syncIo);

    SDL_AsyncIO* asyncFile = SDL_AsyncIOFromFile(pathBuf.CStr(), "r");
    if (!asyncFile) {
        return Err(AssetErrc::IoFailure);
    }

    PakMount& m = m_mounts[m_mountCount++];
    m.asyncFile = asyncFile;
    m.entries = std::move(entries);
    m.entryCount = hdr.entryCount;
    m.priority = priority;
    m.active = true;

    const u32 hashCap = NextPow2(hdr.entryCount * 2u);
    m.hashMask = hashCap - 1u;
    m.tocHash = std::make_unique<MountTocSlot[]>(hashCap);

    BuildMountHashTable(
        std::span<const PakEntry>(m.entries.get(), hdr.entryCount),
        std::span<MountTocSlot>(m.tocHash.get(), hashCap),
        m.hashMask);

    return {};
}

bool AssetSystem::LocateEntry(AssetId id, u8& outMountIdx, PakEntry& outEntry) const noexcept {
    bool found = false;
    u8 bestPriority = 0;

    for (u8 mIdx = 0; mIdx < m_mountCount; ++mIdx) {
        const PakMount& m = m_mounts[mIdx];
        if (!m.active || m.entryCount == 0 || (found && m.priority <= bestPriority)) {
            continue;
        }

        u32 idx = static_cast<u32>((id.value * 11400714819323198485ULL) >> 32) & m.hashMask;
        u32 psl = 1;
        for (;;) {
            const MountTocSlot& slot = m.tocHash[idx];
            if (slot.key == 0 || psl > slot.psl) {
                break;
            }
            if (slot.key == id.value) {
                outMountIdx = mIdx;
                outEntry = m.entries[slot.entryIndex];
                bestPriority = m.priority;
                found = true;
                break;
            }
            ++psl;
            idx = (idx + 1u) & m.hashMask;
        }
    }
    return found;
}

bool AssetSystem::AllocateStaging(u32 size, u32& outOffset, u32& outReserve) noexcept {
    const u32 alignedSize = (size + 15u) & ~15u;
    if (alignedSize > m_ringCap - m_ringUsed) {
        return false;
    }

    const u32 tailSpace = m_ringCap - m_ringWrite;
    if (tailSpace >= alignedSize) {
        outOffset = m_ringWrite;
        outReserve = alignedSize;
        m_ringWrite = (m_ringWrite + alignedSize == m_ringCap) ? 0u : (m_ringWrite + alignedSize);
        m_ringUsed += alignedSize;
        return true;
    }

    if (tailSpace + alignedSize > m_ringCap - m_ringUsed) {
        return false;
    }
    outOffset = 0;
    outReserve = tailSpace + alignedSize;
    m_ringWrite = alignedSize;
    m_ringUsed += outReserve;
    return true;
}

Result<u16, AssetErrc> AssetSystem::EnqueueLoad(
    AssetId id,
    AssetKind expectedKind,
    ScopeId scope,
    Residency residency) noexcept {
    u8 mountIdx = 0;
    PakEntry entry{};
    if (!LocateEntry(id, mountIdx, entry) || entry.kind != expectedKind) {
        return Err(AssetErrc::NotFound);
    }

    bool needsLoad = false;
    auto slotRes = m_bank.AcquireSlot(id, expectedKind, scope, residency, needsLoad);
    if (!slotRes) {
        return slotRes;
    }
    const u16 slot = *slotRes;
    if (!needsLoad) {
        return slot;
    }

    const PakMount& mount = m_mounts[mountIdx];

    // Zero-copy synchronous commit for uncompressed memory-mapped blobs and audio.
    if (mount.mappedBase && entry.codec == AssetCodec::Raw &&
        (expectedKind == AssetKind::Blob || expectedKind == AssetKind::Audio)) {
        if (entry.offset + entry.rawSize > mount.mappedSize) {
            m_bank.MarkFailed(slot);
            return Err(AssetErrc::CorruptedPayload);
        }
        const std::span<const u8> slice{mount.mappedBase + entry.offset, entry.rawSize};
        if (expectedKind == AssetKind::Blob) {
            (void)m_bank.CommitBlob(slot, slice, entry.metaA, true);
        } else {
            (void)m_bank.CommitAudio(slot, slice, static_cast<u16>(entry.metaA), static_cast<u8>(entry.metaB), true);
        }
        return slot;
    }

    u16 jobIdx = 0xFFFFu;
    for (u16 i = 0; i < m_cfg.maxInFlightJobs; ++i) {
        if (!m_jobs[i].active) {
            jobIdx = i;
            break;
        }
    }
    if (jobIdx == 0xFFFFu) {
        m_bank.MarkFailed(slot);
        return Err(AssetErrc::PoolExhausted);
    }

    PendingJob& job = m_jobs[jobIdx];
    job.entry = entry;
    job.sequence = m_nextSequence++;
    job.slot = slot;
    job.mountIndex = mountIdx;
    job.active = true;

    if (mount.mappedBase) {
        job.stagingOffset = 0;
        job.stagingReserve = 0;
        job.ioComplete = true;
        job.ioSuccess = (entry.offset + entry.storedSize <= mount.mappedSize);
    } else {
        const u32 requiredStaging = (entry.codec == AssetCodec::Lz4)
                                        ? (entry.storedSize + entry.rawSize)
                                        : entry.rawSize;
        if (!AllocateStaging(requiredStaging, job.stagingOffset, job.stagingReserve)) {
            job.active = false;
            m_bank.MarkFailed(slot);
            return Err(AssetErrc::StagingOverflow);
        }

        job.ioComplete = false;
        job.ioSuccess = false;
        if (!SDL_ReadAsyncIO(
                mount.asyncFile,
                &m_stagingRing[job.stagingOffset],
                entry.offset,
                entry.storedSize,
                m_ioQueue,
                &job)) {
            job.active = false;
            m_ringUsed -= job.stagingReserve;
            m_bank.MarkFailed(slot);
            return Err(AssetErrc::IoFailure);
        }
    }

    return slot;
}

Result<TextureAssetHandle, AssetErrc> AssetSystem::PinTexture(
    ScopeId scope,
    AssetId id,
    Residency residency) noexcept {
    auto slotRes = EnqueueLoad(id, AssetKind::Texture, scope, residency);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    return m_bank.ToTextureHandle(*slotRes);
}

Result<AtlasHandle, AssetErrc> AssetSystem::PinAtlas(
    ScopeId scope,
    AssetId id,
    Residency residency) noexcept {
    auto slotRes = EnqueueLoad(id, AssetKind::Atlas, scope, residency);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    return m_bank.ToAtlasHandle(*slotRes);
}

Result<BlobHandle, AssetErrc> AssetSystem::PinBlob(
    ScopeId scope,
    AssetId id,
    Residency residency) noexcept {
    auto slotRes = EnqueueLoad(id, AssetKind::Blob, scope, residency);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    return m_bank.ToBlobHandle(*slotRes);
}

Result<AudioHandle, AssetErrc> AssetSystem::PinAudio(
    ScopeId scope,
    AssetId id,
    Residency residency) noexcept {
    auto slotRes = EnqueueLoad(id, AssetKind::Audio, scope, residency);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    return m_bank.ToAudioHandle(*slotRes);
}

void AssetSystem::FinalizeJob(PendingJob& job, renderer::Renderer* renderer) noexcept {
    const PakMount& mount = m_mounts[job.mountIndex];
    const PakEntry& entry = job.entry;

    const u8* storedPtr = mount.mappedBase
                              ? (mount.mappedBase + entry.offset)
                              : &m_stagingRing[job.stagingOffset];

    std::span<const u8> decodedSpan{};
    u32 tempDecompressReserve = 0;

    if (entry.codec == AssetCodec::Raw) {
        decodedSpan = std::span<const u8>{storedPtr, entry.rawSize};
    } else if (entry.codec == AssetCodec::Lz4) {
        u8* dstPtr = nullptr;
        if (mount.mappedBase) {
            u32 offset = 0;
            if (!AllocateStaging(entry.rawSize, offset, tempDecompressReserve)) {
                m_bank.MarkFailed(job.slot);
                return;
            }
            dstPtr = &m_stagingRing[offset];
        } else {
            dstPtr = &m_stagingRing[job.stagingOffset + entry.storedSize];
        }

        auto decRes = AssetBank::DecompressLz4(
            std::span<const u8>{storedPtr, entry.storedSize},
            std::span<u8>{dstPtr, entry.rawSize});
        if (!decRes || *decRes != entry.rawSize) {
            m_ringUsed -= tempDecompressReserve;
            m_bank.MarkFailed(job.slot);
            return;
        }
        decodedSpan = std::span<const u8>{dstPtr, entry.rawSize};
    }

    switch (entry.kind) {
        case AssetKind::Texture: {
            const u16 w = static_cast<u16>(entry.metaA & 0xFFFFu);
            const u16 h = static_cast<u16>(entry.metaA >> 16);
            renderer::TextureHandle gpuHandle{m_headlessTexCounter++, 1};

            if (renderer) {
                renderer::TextureDesc desc{
                    .width  = w,
                    .height = h,
                    .format = static_cast<renderer::TextureFormat>(entry.metaB & 0xFFu),
                    .filter = static_cast<renderer::TextureFilter>((entry.metaB >> 8) & 0xFFu),
                    .pixels = decodedSpan,
                    .pitch  = static_cast<u32>(w) * 4u
                };

                auto texRes = renderer->CreateTexture(desc);
                if (!texRes) {
                    m_ringUsed -= tempDecompressReserve;
                    m_bank.MarkFailed(job.slot);
                    return;
                }
                gpuHandle = *texRes;
            }
            (void)m_bank.CommitTexture(job.slot, gpuHandle, w, h, entry.rawSize);
            break;
        }
        case AssetKind::Atlas: {
            if (decodedSpan.size() >= sizeof(CookedAtlasHeader)) {
                CookedAtlasHeader hdr{};
                std::memcpy(&hdr, decodedSpan.data(), sizeof(CookedAtlasHeader));
                u16 texSlot = 0;
                if (hdr.backingTextureId != 0) {
                    if (auto texRes = EnqueueLoad(
                            AssetId{hdr.backingTextureId},
                            AssetKind::Texture,
                            kInvalidScope,
                            Residency::Presentation)) {
                        texSlot = *texRes;
                    }
                }
                (void)m_bank.CommitAtlas(job.slot, texSlot, decodedSpan);
            } else {
                m_bank.MarkFailed(job.slot);
            }
            break;
        }
        case AssetKind::Blob: {
            const bool isMappedRaw = (mount.mappedBase != nullptr && entry.codec == AssetCodec::Raw);
            (void)m_bank.CommitBlob(job.slot, decodedSpan, entry.metaA, isMappedRaw);
            break;
        }
        case AssetKind::Audio: {
            const bool isMappedRaw = (mount.mappedBase != nullptr && entry.codec == AssetCodec::Raw);
            (void)m_bank.CommitAudio(
                job.slot,
                decodedSpan,
                static_cast<u16>(entry.metaA),
                static_cast<u8>(entry.metaB),
                isMappedRaw);
            break;
        }
        default:
            break;
    }

    m_ringUsed -= tempDecompressReserve;
}

void AssetSystem::PumpInternal(renderer::Renderer* renderer, const PumpBudget& budget) noexcept {
    m_bank.AdvanceFrame(++m_frameCounter);

    SDL_AsyncIOOutcome outcome{};
    while (SDL_GetAsyncIOResult(m_ioQueue, &outcome)) {
        if (auto* job = static_cast<PendingJob*>(outcome.userdata)) {
            job->ioComplete = true;
            job->ioSuccess = (outcome.result == SDL_ASYNCIO_COMPLETE &&
                              outcome.bytes_transferred == job->entry.storedSize);
        }
    }

    u32 uploadsDone = 0;
    u32 bytesUploaded = 0;

    // Finalize completed jobs strictly in request sequence order; skip only over-budget Textures
    // so non-GPU jobs (Blobs, Audio, Atlases) never starve behind a large Texture upload.
    for (;;) {
        u16 bestIdx = 0xFFFFu;
        u64 bestSeq = ~0ULL;

        for (u16 i = 0; i < m_cfg.maxInFlightJobs; ++i) {
            const PendingJob& j = m_jobs[i];
            if (!j.active || !j.ioComplete || j.sequence >= bestSeq) {
                continue;
            }
            if (j.ioSuccess && j.entry.kind == AssetKind::Texture &&
                (uploadsDone >= budget.maxGpuUploads ||
                 (uploadsDone > 0 && bytesUploaded + j.entry.rawSize > budget.maxUploadBytes))) {
                continue;
            }
            bestSeq = j.sequence;
            bestIdx = i;
        }

        if (bestIdx == 0xFFFFu) {
            break;
        }

        PendingJob& job = m_jobs[bestIdx];
        if (job.ioSuccess) {
            FinalizeJob(job, renderer);
            if (job.entry.kind == AssetKind::Texture) {
                ++uploadsDone;
                bytesUploaded += job.entry.rawSize;
            }
        } else {
            m_bank.MarkFailed(job.slot);
        }

        m_ringUsed -= job.stagingReserve;
        if (m_ringUsed == 0) {
            m_ringWrite = 0;
        }
        job.active = false;
    }

    renderer::TextureHandle evicted[16]{};
    const u32 evictedCount = m_bank.EvictUnpinned(evicted);
    if (renderer) {
        for (u32 i = 0; i < evictedCount; ++i) {
            renderer->DestroyTexture(evicted[i]);
        }
    }
}

void AssetSystem::Pump(renderer::Renderer& renderer, const PumpBudget& budget) noexcept {
    PumpInternal(&renderer, budget);
}

void AssetSystem::PumpHeadless(const PumpBudget& budget) noexcept {
    PumpInternal(nullptr, budget);
}

Result<void, AssetErrc> AssetSystem::WaitSimReady(
    ScopeId scope,
    renderer::Renderer* optionalRenderer) noexcept {
    while (!m_bank.IsScopeSimReady(scope)) {
        SDL_AsyncIOOutcome outcome{};
        if (SDL_WaitAsyncIOResult(m_ioQueue, &outcome, 10)) {
            if (auto* job = static_cast<PendingJob*>(outcome.userdata)) {
                job->ioComplete = true;
                job->ioSuccess = (outcome.result == SDL_ASYNCIO_COMPLETE &&
                                  outcome.bytes_transferred == job->entry.storedSize);
            }
        }
        PumpInternal(optionalRenderer, PumpBudget{64, 256 * 1024 * 1024});
    }
    return {};
}

} // namespace en::assets