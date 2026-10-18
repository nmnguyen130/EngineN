#include "assets/AssetSystem.h"

#include "renderer/Renderer.h"

#include <SDL3/SDL_asyncio.h>
#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace en::assets {

AssetSystem::~AssetSystem() noexcept {
    Shutdown();
}

AssetSystem::AssetSystem(AssetSystem&& other) noexcept {
    *this = std::move(other);
}

AssetSystem& AssetSystem::operator=(AssetSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_ioQueue            = std::exchange(other.m_ioQueue, nullptr);
        m_mounts             = std::exchange(other.m_mounts, nullptr);
        m_jobs               = std::exchange(other.m_jobs, nullptr);
        m_stagingRing        = std::exchange(other.m_stagingRing, nullptr);
        m_tocEntriesPool     = std::exchange(other.m_tocEntriesPool, nullptr);
        m_vfsHash            = std::exchange(other.m_vfsHash, nullptr);
        m_vfsHashMask        = std::exchange(other.m_vfsHashMask, 0);
        m_ringWrite          = std::exchange(other.m_ringWrite, 0);
        m_ringUsed           = std::exchange(other.m_ringUsed, 0);
        m_activeJobHead      = std::exchange(other.m_activeJobHead, kInvalidJob);
        m_activeJobTail      = std::exchange(other.m_activeJobTail, kInvalidJob);
        m_freeJobHead        = std::exchange(other.m_freeJobHead, kInvalidJob);
        m_highWaterJob       = std::exchange(other.m_highWaterJob, 0);
        m_activeJobCount     = std::exchange(other.m_activeJobCount, 0);
        m_stagingJobCount    = std::exchange(other.m_stagingJobCount, 0);
        m_headlessTexCounter = std::exchange(other.m_headlessTexCounter, 1);
        m_mountCount         = std::exchange(other.m_mountCount, 0);
        m_frameCounter       = std::exchange(other.m_frameCounter, 0);
        m_tocEntriesUsed     = std::exchange(other.m_tocEntriesUsed, 0);
        m_cfg                = other.m_cfg;
        m_arena              = std::move(other.m_arena);
        m_bank               = std::move(other.m_bank);
    }
    return *this;
}

Result<void, AssetErrc> AssetSystem::Init(const AssetSystemConfig& cfg) noexcept {
    Shutdown();
    m_cfg = cfg;
    if (m_cfg.maxTotalTocEntries == 0) {
        m_cfg.maxTotalTocEntries = 4096u;
    }

    if (auto res = m_bank.Init(m_cfg.bank); !res) {
        return res;
    }

    m_ioQueue = SDL_CreateAsyncIOQueue();
    if (!m_ioQueue) {
        m_bank.Shutdown();
        return Err(AssetErrc::IoFailure);
    }

    const u32 vfsHashCap = NextPow2(m_cfg.maxTotalTocEntries * 2u);
    m_vfsHashMask = vfsHashCap - 1u;

    const std::size_t totalBytes =
        64u +
        Align64(static_cast<std::size_t>(m_cfg.maxMounts) * sizeof(PakMount)) +
        Align64(static_cast<std::size_t>(m_cfg.maxInFlightJobs) * sizeof(PendingJob)) +
        Align64(static_cast<std::size_t>(m_cfg.maxTotalTocEntries) * sizeof(PakEntry)) +
        Align64(static_cast<std::size_t>(vfsHashCap) * sizeof(VfsTocSlot)) +
        Align64(m_cfg.stagingRingBytes);

    m_arena = std::make_unique_for_overwrite<u64[]>((totalBytes + 7u) >> 3u);
    const auto rawAddr = reinterpret_cast<std::uintptr_t>(m_arena.get());
    u8* cursor = reinterpret_cast<u8*>((rawAddr + 63u) & ~static_cast<std::uintptr_t>(63u));

    m_mounts         = CarveAligned64<PakMount>(cursor, m_cfg.maxMounts);
    m_jobs           = CarveAligned64<PendingJob>(cursor, m_cfg.maxInFlightJobs);
    m_tocEntriesPool = CarveAligned64<PakEntry>(cursor, m_cfg.maxTotalTocEntries);
    m_vfsHash        = CarveAligned64<VfsTocSlot>(cursor, vfsHashCap);
    m_stagingRing    = CarveAligned64<u8>(cursor, m_cfg.stagingRingBytes);

    std::fill_n(m_vfsHash, vfsHashCap, VfsTocSlot{});
    return {};
}

void AssetSystem::Shutdown() noexcept {
    if (m_mounts) {
        for (u8 i = 0; i < m_mountCount; ++i) {
            if (m_mounts[i].asyncFile) {
                SDL_CloseAsyncIO(m_mounts[i].asyncFile, false, m_ioQueue, nullptr);
            }
        }
    }
    if (m_ioQueue) {
        SDL_DestroyAsyncIOQueue(m_ioQueue);
        m_ioQueue = nullptr;
    }
    m_arena.reset();
    m_mounts = nullptr;
    m_jobs = nullptr;
    m_stagingRing = nullptr;
    m_tocEntriesPool = nullptr;
    m_vfsHash = nullptr;
    m_bank.Shutdown();

    m_vfsHashMask = 0;
    m_ringWrite = 0;
    m_ringUsed = 0;
    m_activeJobHead = kInvalidJob;
    m_activeJobTail = kInvalidJob;
    m_freeJobHead = kInvalidJob;
    m_highWaterJob = 0;
    m_activeJobCount = 0;
    m_stagingJobCount = 0;
    m_mountCount = 0;
    m_tocEntriesUsed = 0;
}

void AssetSystem::RegisterVfsEntries(u8 mountIdx, u8 priority, u32 firstEntry, u32 entryCount) noexcept {
    for (u32 i = 0; i < entryCount; ++i) {
        const u32 globalIdx = firstEntry + i;
        const u64 key = m_tocEntriesPool[globalIdx].assetId;
        if (key == 0) {
            continue;
        }
        u32 idx = FibonacciHash(key, m_vfsHashMask);
        VfsTocSlot incoming{key, globalIdx, 1, mountIdx, priority};
        for (;;) {
            VfsTocSlot& cur = m_vfsHash[idx];
            if (cur.key == 0) {
                cur = incoming;
                break;
            }
            if (cur.key == key) {
                if (incoming.priority > cur.priority) {
                    cur.entryIndex = incoming.entryIndex;
                    cur.mountIndex = incoming.mountIndex;
                    cur.priority   = incoming.priority;
                }
                break;
            }
            if (incoming.psl > cur.psl) {
                std::swap(incoming, cur);
            }
            ++incoming.psl;
            idx = (idx + 1u) & m_vfsHashMask;
        }
    }
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
    if (hdr.tocOffset > mappedPak.size() || tocBytes > mappedPak.size() - hdr.tocOffset) {
        return Err(AssetErrc::CorruptedHeader);
    }
    if (hdr.entryCount > m_cfg.maxTotalTocEntries - m_tocEntriesUsed) {
        return Err(AssetErrc::PoolExhausted);
    }

    const u8 mountIdx = m_mountCount++;
    m_mounts[mountIdx] = PakMount{nullptr, mappedPak.data(), mappedPak.size(), 0};

    if (hdr.entryCount > 0) {
        const u32 firstEntry = m_tocEntriesUsed;
        std::memcpy(&m_tocEntriesPool[firstEntry], mappedPak.data() + hdr.tocOffset, tocBytes);
        m_tocEntriesUsed += hdr.entryCount;
        RegisterVfsEntries(mountIdx, priority, firstEntry, hdr.entryCount);
    }
    return {};
}

Result<void, AssetErrc> AssetSystem::MountPackFile(
    std::string_view filePath,
    u8 priority) noexcept {
    if (m_mountCount >= m_cfg.maxMounts) {
        return Err(AssetErrc::PoolExhausted);
    }
    if (filePath.empty() || filePath.size() >= 1024u) {
        return Err(AssetErrc::InvalidMount);
    }

    char pathBuf[1024];
    std::memcpy(pathBuf, filePath.data(), filePath.size());
    pathBuf[filePath.size()] = '\0';

    SDL_IOStream* syncIo = SDL_IOFromFile(pathBuf, "rb");
    if (!syncIo) {
        return Err(AssetErrc::IoFailure);
    }

    PakHeader hdr{};
    if (SDL_ReadIO(syncIo, &hdr, sizeof(PakHeader)) != sizeof(PakHeader) ||
        hdr.magic != kPakMagic || hdr.version != kPakVersion) {
        SDL_CloseIO(syncIo);
        return Err(AssetErrc::CorruptedHeader);
    }
    if (hdr.entryCount > m_cfg.maxTotalTocEntries - m_tocEntriesUsed) {
        SDL_CloseIO(syncIo);
        return Err(AssetErrc::PoolExhausted);
    }

    const u32 firstEntry = m_tocEntriesUsed;
    const std::size_t tocBytes = static_cast<std::size_t>(hdr.entryCount) * sizeof(PakEntry);
    if (hdr.entryCount > 0 &&
        (SDL_SeekIO(syncIo, static_cast<Sint64>(hdr.tocOffset), SDL_IO_SEEK_SET) < 0 ||
         SDL_ReadIO(syncIo, &m_tocEntriesPool[firstEntry], tocBytes) != tocBytes)) {
        SDL_CloseIO(syncIo);
        return Err(AssetErrc::CorruptedHeader);
    }
    SDL_CloseIO(syncIo);

    SDL_AsyncIO* asyncFile = SDL_AsyncIOFromFile(pathBuf, "r");
    if (!asyncFile) {
        return Err(AssetErrc::IoFailure);
    }

    const u8 mountIdx = m_mountCount++;
    m_mounts[mountIdx] = PakMount{asyncFile, nullptr, 0, 0};
    m_tocEntriesUsed += hdr.entryCount;
    RegisterVfsEntries(mountIdx, priority, firstEntry, hdr.entryCount);
    return {};
}

bool AssetSystem::LocateEntry(AssetId id, u8& outMountIdx, PakEntry& outEntry) const noexcept {
    if (!id.IsValid() || m_mountCount == 0) {
        return false;
    }

    u32 idx = FibonacciHash(id.value, m_vfsHashMask);
    u16 psl = 1;
    for (;;) {
        const VfsTocSlot& slot = m_vfsHash[idx];
        if (slot.key == 0 || psl > slot.psl) {
            return false;
        }
        if (slot.key == id.value) {
            outMountIdx = slot.mountIndex;
            outEntry    = m_tocEntriesPool[slot.entryIndex];
            return true;
        }
        ++psl;
        idx = (idx + 1u) & m_vfsHashMask;
    }
}

bool AssetSystem::AllocateStaging(u32 size, u32& outOffset, u32& outReserve) noexcept {
    if (size == 0) {
        outOffset = 0;
        outReserve = 0;
        return true;
    }
    const u32 alignedSize = (size + 15u) & ~15u;
    if (alignedSize > m_cfg.stagingRingBytes - m_ringUsed) {
        return false;
    }
    if (m_stagingJobCount == 0) {
        m_ringUsed = 0;
        m_ringWrite = 0;
    }

    u32 ringRead = m_ringWrite;
    for (u16 curr = m_activeJobHead; curr != kInvalidJob; curr = m_jobs[curr].nextJob) {
        if (m_jobs[curr].stagingReserve > 0) {
            ringRead = m_jobs[curr].stagingOffset;
            break;
        }
    }

    if (m_stagingJobCount == 0 || m_ringWrite > ringRead) {
        const u32 tailSpace = m_cfg.stagingRingBytes - m_ringWrite;
        if (tailSpace >= alignedSize) {
            outOffset = m_ringWrite;
            outReserve = alignedSize;
            m_ringWrite = (m_ringWrite + alignedSize == m_cfg.stagingRingBytes) ? 0u : (m_ringWrite + alignedSize);
            m_ringUsed += alignedSize;
            ++m_stagingJobCount;
            return true;
        }
        if (alignedSize <= ringRead && tailSpace + alignedSize <= m_cfg.stagingRingBytes - m_ringUsed) {
            outOffset = 0;
            outReserve = tailSpace + alignedSize;
            m_ringWrite = alignedSize;
            m_ringUsed += outReserve;
            ++m_stagingJobCount;
            return true;
        }
        return false;
    }

    if (ringRead - m_ringWrite >= alignedSize) {
        outOffset = m_ringWrite;
        outReserve = alignedSize;
        m_ringWrite += alignedSize;
        m_ringUsed += alignedSize;
        ++m_stagingJobCount;
        return true;
    }
    return false;
}

void AssetSystem::ReleaseJob(u16 jobIdx) noexcept {
    PendingJob& job = m_jobs[jobIdx];
    if (job.stagingReserve > 0) {
        m_ringUsed -= job.stagingReserve;
        if (--m_stagingJobCount == 0) {
            m_ringUsed = 0;
            m_ringWrite = 0;
        }
        job.stagingReserve = 0;
    }

    if (job.prevJob != kInvalidJob) {
        m_jobs[job.prevJob].nextJob = job.nextJob;
    } else {
        m_activeJobHead = job.nextJob;
    }
    if (job.nextJob != kInvalidJob) {
        m_jobs[job.nextJob].prevJob = job.prevJob;
    } else {
        m_activeJobTail = job.prevJob;
    }

    job.flags = 0;
    job.prevJob = kInvalidJob;
    job.nextJob = m_freeJobHead;
    m_freeJobHead = jobIdx;
    --m_activeJobCount;
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

    if (mount.mappedBase && entry.codec == AssetCodec::Raw &&
        (expectedKind == AssetKind::Blob || expectedKind == AssetKind::Audio)) {
        if (entry.offset > mount.mappedSize || entry.rawSize > mount.mappedSize - entry.offset) {
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

    if (m_freeJobHead == kInvalidJob && m_highWaterJob >= m_cfg.maxInFlightJobs) {
        m_bank.MarkFailed(slot);
        return Err(AssetErrc::PoolExhausted);
    }

    const bool isArenaDest = (expectedKind == AssetKind::Blob || expectedKind == AssetKind::Audio);
    u32 requiredStaging = 0;
    if (mount.mappedBase) {
        if (entry.codec == AssetCodec::Lz4 && !isArenaDest) {
            requiredStaging = entry.rawSize;
        }
    } else {
        requiredStaging = (entry.codec == AssetCodec::Lz4 && !isArenaDest)
                              ? (entry.storedSize + entry.rawSize)
                              : ((entry.codec == AssetCodec::Lz4) ? entry.storedSize : entry.rawSize);
    }

    u32 stagingOffset = 0;
    u32 stagingReserve = 0;
    if (!AllocateStaging(requiredStaging, stagingOffset, stagingReserve)) {
        m_bank.MarkFailed(slot);
        return Err(AssetErrc::StagingOverflow);
    }

    const u16 jobIdx = (m_freeJobHead != kInvalidJob)
                           ? std::exchange(m_freeJobHead, m_jobs[m_freeJobHead].nextJob)
                           : m_highWaterJob++;

    PendingJob& job = m_jobs[jobIdx];
    job.entry = entry;
    job.stagingOffset = stagingOffset;
    job.stagingReserve = stagingReserve;
    job.slot = slot;
    job.mountIndex = mountIdx;
    job.prevJob = m_activeJobTail;
    job.nextJob = kInvalidJob;

    if (m_activeJobTail != kInvalidJob) {
        m_jobs[m_activeJobTail].nextJob = jobIdx;
    } else {
        m_activeJobHead = jobIdx;
    }
    m_activeJobTail = jobIdx;
    ++m_activeJobCount;

    if (mount.mappedBase) {
        const bool validRange = (entry.offset <= mount.mappedSize &&
                                 entry.storedSize <= mount.mappedSize - entry.offset);
        job.flags = kJobComplete | (validRange ? kJobSuccess : 0u);
    } else {
        job.flags = 0;
        if (!SDL_ReadAsyncIO(
                mount.asyncFile,
                &m_stagingRing[job.stagingOffset],
                entry.offset,
                entry.storedSize,
                m_ioQueue,
                &job)) {
            ReleaseJob(jobIdx);
            m_bank.MarkFailed(slot);
            return Err(AssetErrc::IoFailure);
        }
    }

    return slot;
}

void AssetSystem::FinalizeJob(PendingJob& job, renderer::Renderer* renderer) noexcept {
    const PakMount& mount = m_mounts[job.mountIndex];
    const PakEntry& entry = job.entry;

    const u8* storedPtr = mount.mappedBase
                              ? (mount.mappedBase + entry.offset)
                              : &m_stagingRing[job.stagingOffset];

    const bool isArenaDest = (entry.kind == AssetKind::Blob || entry.kind == AssetKind::Audio);
    const u32 prevArenaHead = m_bank.BlobArenaHead();
    std::span<const u8> decodedSpan{};

    if (entry.codec == AssetCodec::Raw) {
        decodedSpan = std::span<const u8>{storedPtr, entry.rawSize};
    } else if (entry.codec == AssetCodec::Lz4) {
        u8* dstPtr = nullptr;
        if (isArenaDest) {
            auto sliceRes = m_bank.AllocateArenaSlice(entry.rawSize);
            if (!sliceRes) {
                m_bank.MarkFailed(job.slot);
                return;
            }
            dstPtr = sliceRes->data();
        } else {
            dstPtr = mount.mappedBase
                         ? &m_stagingRing[job.stagingOffset]
                         : &m_stagingRing[job.stagingOffset + entry.storedSize];
        }

        auto decRes = AssetBank::DecompressLz4(
            std::span<const u8>{storedPtr, entry.storedSize},
            std::span<u8>{dstPtr, entry.rawSize});
        if (!decRes || *decRes != entry.rawSize) {
            if (isArenaDest) {
                m_bank.RollbackArenaSlice(prevArenaHead);
            }
            m_bank.MarkFailed(job.slot);
            return;
        }
        decodedSpan = std::span<const u8>{dstPtr, entry.rawSize};
    } else {
        m_bank.MarkFailed(job.slot);
        return;
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
            const bool isInPlace = (mount.mappedBase != nullptr && entry.codec == AssetCodec::Raw) ||
                                   (entry.codec == AssetCodec::Lz4);
            (void)m_bank.CommitBlob(job.slot, decodedSpan, entry.metaA, isInPlace);
            break;
        }
        case AssetKind::Audio: {
            const bool isInPlace = (mount.mappedBase != nullptr && entry.codec == AssetCodec::Raw) ||
                                   (entry.codec == AssetCodec::Lz4);
            (void)m_bank.CommitAudio(
                job.slot,
                decodedSpan,
                static_cast<u16>(entry.metaA),
                static_cast<u8>(entry.metaB),
                isInPlace);
            break;
        }
        default:
            m_bank.MarkFailed(job.slot);
            break;
    }
}

void AssetSystem::PumpInternal(renderer::Renderer* renderer, const PumpBudget& budget) noexcept {
    m_bank.AdvanceFrame(++m_frameCounter);

    if (m_activeJobCount > 0) {
        SDL_AsyncIOOutcome outcome{};
        while (SDL_GetAsyncIOResult(m_ioQueue, &outcome)) {
            if (auto* job = static_cast<PendingJob*>(outcome.userdata)) {
                job->flags |= kJobComplete;
                if (outcome.result == SDL_ASYNCIO_COMPLETE &&
                    outcome.bytes_transferred == job->entry.storedSize) {
                    job->flags |= kJobSuccess;
                }
            }
        }

        u32 uploadsDone = 0;
        u32 bytesUploaded = 0;

        u16 curr = m_activeJobHead;
        while (curr != kInvalidJob) {
            PendingJob& job = m_jobs[curr];
            if ((job.flags & kJobComplete) == 0) {
                curr = job.nextJob;
                continue;
            }

            const bool isTex = (job.entry.kind == AssetKind::Texture);
            if (isTex && (job.flags & kJobSuccess) != 0 &&
                (uploadsDone >= budget.maxGpuUploads ||
                 (uploadsDone > 0 && bytesUploaded + job.entry.rawSize > budget.maxUploadBytes))) {
                curr = job.nextJob;
                continue;
            }

            if ((job.flags & kJobSuccess) != 0) {
                FinalizeJob(job, renderer);
                if (isTex) {
                    ++uploadsDone;
                    bytesUploaded += job.entry.rawSize;
                }
            } else {
                m_bank.MarkFailed(job.slot);
            }

            const u16 next = job.nextJob;
            ReleaseJob(curr);
            curr = next;
        }
    }

    if (m_bank.IsOverVramBudget()) {
        renderer::TextureHandle evicted[16];
        const u32 evictedCount = m_bank.EvictUnpinned(evicted);
        if (renderer) {
            for (u32 i = 0; i < evictedCount; ++i) {
                renderer->DestroyTexture(evicted[i]);
            }
        }
    }
}

Result<void, AssetErrc> AssetSystem::WaitSimReady(
    ScopeId scope,
    renderer::Renderer* optionalRenderer) noexcept {
    if (!m_bank.IsScopeSimReady(scope)) {
        PumpInternal(optionalRenderer, PumpBudget{64, 256 * 1024 * 1024});
    }
    while (!m_bank.IsScopeSimReady(scope) && m_activeJobCount > 0) {
        SDL_AsyncIOOutcome outcome{};
        if (SDL_WaitAsyncIOResult(m_ioQueue, &outcome, 10)) {
            if (auto* job = static_cast<PendingJob*>(outcome.userdata)) {
                job->flags |= kJobComplete;
                if (outcome.result == SDL_ASYNCIO_COMPLETE &&
                    outcome.bytes_transferred == job->entry.storedSize) {
                    job->flags |= kJobSuccess;
                }
            }
        }
        PumpInternal(optionalRenderer, PumpBudget{64, 256 * 1024 * 1024});
    }
    return {};
}

} // namespace en::assets
