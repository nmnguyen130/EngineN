#include "assets/AssetBank.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace en::assets {

namespace {

alignas(16) constexpr u8 kFallbackZeroBlob[64] = {0};
alignas(16) constexpr i16 kFallbackSilencePcm[64] = {0};

} // namespace

Result<void, AssetErrc> AssetBank::Init(const AssetBankConfig& cfg) noexcept {
    Shutdown();
    m_cfg.maxSlots       = (cfg.maxSlots < 2) ? 2 : cfg.maxSlots;
    m_cfg.maxTextures    = (cfg.maxTextures < 2) ? 2 : cfg.maxTextures;
    m_cfg.maxAtlases     = (cfg.maxAtlases < 2) ? 2 : cfg.maxAtlases;
    m_cfg.maxBlobs       = (cfg.maxBlobs < 2) ? 2 : cfg.maxBlobs;
    m_cfg.maxAudio       = (cfg.maxAudio < 2) ? 2 : cfg.maxAudio;
    m_cfg.maxSprites     = (cfg.maxSprites < 2) ? 2 : cfg.maxSprites;
    m_cfg.maxNamedFrames = cfg.maxNamedFrames;
    m_cfg.blobArenaBytes = cfg.blobArenaBytes;
    m_cfg.vramBudgetBytes = cfg.vramBudgetBytes;
    m_cfg.evictGraceFrames = cfg.evictGraceFrames;

    m_hashCap  = NextPow2(static_cast<u32>(m_cfg.maxSlots) * 2u);
    m_hashMask = m_hashCap - 1u;

    const std::size_t totalBytes =
        64u +
        Align64(static_cast<std::size_t>(m_cfg.maxSprites) * sizeof(SpriteFrame)) +
        Align64(static_cast<std::size_t>(m_cfg.maxSprites) * sizeof(u16)) +
        Align64(static_cast<std::size_t>(m_cfg.maxAtlases) * sizeof(AtlasRecord)) +
        Align64(static_cast<std::size_t>(m_cfg.maxTextures) * sizeof(TextureRecord)) +
        Align64(static_cast<std::size_t>(m_cfg.maxBlobs) * sizeof(BlobRecord)) +
        Align64(static_cast<std::size_t>(m_cfg.maxAudio) * sizeof(AudioRecord)) +
        Align64(static_cast<std::size_t>(m_cfg.maxSlots) * sizeof(SlotMeta)) +
        Align64(static_cast<std::size_t>(m_cfg.maxSlots) * sizeof(AssetId)) +
        Align64(static_cast<std::size_t>(m_hashCap) * sizeof(HashSlot)) +
        Align64(static_cast<std::size_t>(m_cfg.maxTextures) * sizeof(u16)) +
        Align64(static_cast<std::size_t>(m_cfg.maxTextures) * sizeof(u32)) +
        Align64(static_cast<std::size_t>(m_cfg.maxAtlases) * sizeof(AtlasCold)) +
        Align64(static_cast<std::size_t>(m_cfg.maxNamedFrames) * sizeof(CookedFrameName)) +
        Align64(m_cfg.blobArenaBytes);

    m_arena = std::make_unique_for_overwrite<u64[]>((totalBytes + 7u) >> 3u);
    const auto rawAddr = reinterpret_cast<std::uintptr_t>(m_arena.get());
    u8* cursor = reinterpret_cast<u8*>((rawAddr + 63u) & ~static_cast<std::uintptr_t>(63u));

    m_sprites       = CarveAligned64<SpriteFrame>(cursor, m_cfg.maxSprites);
    m_spriteGens    = CarveAligned64<u16>(cursor, m_cfg.maxSprites);
    m_atlases       = CarveAligned64<AtlasRecord>(cursor, m_cfg.maxAtlases);
    m_textures      = CarveAligned64<TextureRecord>(cursor, m_cfg.maxTextures);
    m_blobs         = CarveAligned64<BlobRecord>(cursor, m_cfg.maxBlobs);
    m_audio         = CarveAligned64<AudioRecord>(cursor, m_cfg.maxAudio);
    m_slots         = CarveAligned64<SlotMeta>(cursor, m_cfg.maxSlots);
    m_slotIds       = CarveAligned64<AssetId>(cursor, m_cfg.maxSlots);
    m_hashTable     = CarveAligned64<HashSlot>(cursor, m_hashCap);
    m_texMasterSlot = CarveAligned64<u16>(cursor, m_cfg.maxTextures);
    m_texVramBytes  = CarveAligned64<u32>(cursor, m_cfg.maxTextures);
    m_atlasCold     = CarveAligned64<AtlasCold>(cursor, m_cfg.maxAtlases);
    m_namedFrames   = CarveAligned64<CookedFrameName>(cursor, m_cfg.maxNamedFrames);
    m_blobArena     = CarveAligned64<u8>(cursor, m_cfg.blobArenaBytes);

    std::fill_n(m_hashTable, m_hashCap, HashSlot{});

    m_slots[0] = SlotMeta{0, 0, 0, 1, 0, AssetKind::None, AssetState::Ready};
    m_slotIds[0] = AssetId{};
    m_textures[0] = TextureRecord{renderer::kWhiteTexture, 1, 1};
    m_texMasterSlot[0] = 0;
    m_texVramBytes[0] = 0;
    m_atlases[0] = AtlasRecord{0, 1, 0, 0};
    m_atlasCold[0] = AtlasCold{};
    m_sprites[0] = SpriteFrame{};
    m_spriteGens[0] = 0;
    m_blobs[0] = BlobRecord{kFallbackZeroBlob, sizeof(kFallbackZeroBlob), 0};
    m_audio[0] = AudioRecord{kFallbackSilencePcm, 32, 48000, 2, 0};

    return {};
}

Result<ScopeId, AssetErrc> AssetBank::BeginScope() noexcept {
    const int freeBit = std::countr_one(m_activeScopesMask);
    if (freeBit >= static_cast<int>(kMaxScopes)) {
        return Err(AssetErrc::ScopeExhausted);
    }
    m_activeScopesMask |= (1u << freeBit);
    return static_cast<ScopeId>(freeBit);
}

void AssetBank::EndScope(ScopeId scope) noexcept {
    if (scope >= kMaxScopes) {
        return;
    }
    const u32 bit = (1u << scope);
    if ((m_activeScopesMask & bit) == 0) {
        return;
    }
    const u32 clearMask = ~bit;
    m_activeScopesMask &= clearMask;
    m_scopeSimPending[scope] = 0;

    for (u16 i = 1; i < m_highWaterSlot; ++i) {
        SlotMeta& meta = m_slots[i];
        if ((meta.scopeMask & bit) != 0) {
            meta.scopeMask &= clearMask;
            if (meta.state == AssetState::Loading) {
                meta.lastUsedFrame &= clearMask;
            } else {
                meta.lastUsedFrame = m_currentFrame;
            }
        }
    }
}

void AssetBank::InsertHash(u64 key, u16 slotIndex) noexcept {
    u32 idx = FibonacciHash(key, m_hashMask);
    HashSlot incoming{key, slotIndex, 1, 0};

    for (;;) {
        HashSlot& cur = m_hashTable[idx];
        if (cur.key == 0) {
            cur = incoming;
            return;
        }
        if (cur.key == key) {
            cur.slotIndex = slotIndex;
            return;
        }
        if (incoming.psl > cur.psl) {
            std::swap(incoming, cur);
        }
        ++incoming.psl;
        idx = (idx + 1u) & m_hashMask;
    }
}

void AssetBank::RemoveHash(u64 key) noexcept {
    u32 idx = FibonacciHash(key, m_hashMask);
    u16 psl = 1;

    for (;;) {
        HashSlot& cur = m_hashTable[idx];
        if (cur.key == 0 || psl > cur.psl) {
            return;
        }
        if (cur.key == key) {
            for (;;) {
                const u32 nextIdx = (idx + 1u) & m_hashMask;
                const HashSlot& next = m_hashTable[nextIdx];
                if (next.key == 0 || next.psl <= 1) {
                    m_hashTable[idx] = HashSlot{};
                    return;
                }
                m_hashTable[idx] = next;
                --m_hashTable[idx].psl;
                idx = nextIdx;
            }
        }
        ++psl;
        idx = (idx + 1u) & m_hashMask;
    }
}

u16 AssetBank::FindSlot(AssetId id) const noexcept {
    if (!id.IsValid() || m_hashCap == 0) {
        return 0;
    }
    u32 idx = FibonacciHash(id.value, m_hashMask);
    u16 psl = 1;

    for (;;) {
        const HashSlot& cur = m_hashTable[idx];
        if (cur.key == 0 || psl > cur.psl) {
            return 0;
        }
        if (cur.key == id.value) {
            return cur.slotIndex;
        }
        ++psl;
        idx = (idx + 1u) & m_hashMask;
    }
}

Result<u16, AssetErrc> AssetBank::AcquireSlot(
    AssetId id,
    AssetKind kind,
    ScopeId scope,
    Residency residency,
    bool& outNeedsLoad) noexcept {
    outNeedsLoad = false;
    if (!id.IsValid()) {
        return Err(AssetErrc::NotFound);
    }

    const u32 scopeBit = (scope < kMaxScopes) ? (1u << scope) : 0u;
    if (const u16 existing = FindSlot(id); existing != 0) {
        SlotMeta& meta = m_slots[existing];
        meta.scopeMask |= scopeBit;
        if (meta.state == AssetState::Loading) {
            if (residency == Residency::Sim && scopeBit != 0 && (meta.lastUsedFrame & scopeBit) == 0) {
                meta.lastUsedFrame |= scopeBit;
                ++m_scopeSimPending[scope];
            }
        } else {
            meta.lastUsedFrame = m_currentFrame;
        }
        if (meta.kind == AssetKind::Atlas) {
            const u16 texPayload = m_atlases[meta.payloadSlot].textureSlot;
            if (texPayload != 0) {
                const u16 texMaster = m_texMasterSlot[texPayload];
                m_slots[texMaster].scopeMask |= scopeBit;
                if (m_slots[texMaster].state != AssetState::Loading) {
                    m_slots[texMaster].lastUsedFrame = m_currentFrame;
                }
            }
        }
        return existing;
    }

    if (m_freeSlotHead == 0 && m_highWaterSlot >= m_cfg.maxSlots) {
        return Err(AssetErrc::PoolExhausted);
    }

    u16 payloadIdx = 0;
    switch (kind) {
        case AssetKind::Texture:
            if (m_freeTexHead != 0) {
                payloadIdx = std::exchange(m_freeTexHead, m_textures[m_freeTexHead].width);
            } else if (m_highWaterTex < m_cfg.maxTextures) {
                payloadIdx = m_highWaterTex++;
            } else {
                return Err(AssetErrc::PoolExhausted);
            }
            m_textures[payloadIdx] = m_textures[0];
            m_texVramBytes[payloadIdx] = 0;
            break;
        case AssetKind::Atlas:
            if (m_freeAtlasHead != 0) {
                payloadIdx = std::exchange(m_freeAtlasHead, m_atlases[m_freeAtlasHead].textureSlot);
            } else if (m_highWaterAtlas < m_cfg.maxAtlases) {
                payloadIdx = m_highWaterAtlas++;
                m_atlasCold[payloadIdx] = AtlasCold{};
            } else {
                return Err(AssetErrc::PoolExhausted);
            }
            m_atlases[payloadIdx] = m_atlases[0];
            break;
        case AssetKind::Blob:
            if (m_highWaterBlob >= m_cfg.maxBlobs) return Err(AssetErrc::PoolExhausted);
            payloadIdx = m_highWaterBlob++;
            m_blobs[payloadIdx] = m_blobs[0];
            break;
        case AssetKind::Audio:
            if (m_highWaterAudio >= m_cfg.maxAudio) return Err(AssetErrc::PoolExhausted);
            payloadIdx = m_highWaterAudio++;
            m_audio[payloadIdx] = m_audio[0];
            break;
        default:
            return Err(AssetErrc::NotFound);
    }

    u16 slot = 0;
    if (m_freeSlotHead != 0) {
        slot = std::exchange(m_freeSlotHead, m_slots[m_freeSlotHead].payloadSlot);
    } else {
        slot = m_highWaterSlot++;
        m_slots[slot].generation = 1;
        m_slots[slot].version = 1;
    }

    const u32 simBit = (residency == Residency::Sim) ? scopeBit : 0u;
    if (simBit != 0) {
        ++m_scopeSimPending[scope];
    }

    SlotMeta& meta = m_slots[slot];
    meta.scopeMask = scopeBit;
    meta.lastUsedFrame = simBit;
    meta.payloadSlot = payloadIdx;
    meta.kind = kind;
    meta.state = AssetState::Loading;

    if (kind == AssetKind::Texture) {
        m_texMasterSlot[payloadIdx] = slot;
    } else if (kind == AssetKind::Atlas) {
        m_atlases[payloadIdx].generation = meta.generation;
    }

    m_slotIds[slot] = id;
    InsertHash(id.value, slot);

    outNeedsLoad = true;
    return slot;
}

Result<std::span<u8>, AssetErrc> AssetBank::AllocateArenaSlice(u32 sizeBytes) noexcept {
    if (sizeBytes == 0) {
        return std::span<u8>{};
    }
    const u32 alignedHead = (m_blobArenaHead + 15u) & ~15u;
    if (sizeBytes > m_cfg.blobArenaBytes || alignedHead > m_cfg.blobArenaBytes - sizeBytes) {
        return Err(AssetErrc::PoolExhausted);
    }
    m_blobArenaHead = alignedHead + sizeBytes;
    return std::span<u8>{&m_blobArena[alignedHead], sizeBytes};
}

Result<const u8*, AssetErrc> AssetBank::ResolveArenaPayload(
    std::span<const u8> rawPayload,
    bool isMemoryMapped) noexcept {
    if (isMemoryMapped || rawPayload.empty()) {
        return rawPayload.data();
    }
    if (m_blobArena && rawPayload.data() >= m_blobArena &&
        rawPayload.data() + rawPayload.size() <= m_blobArena + m_cfg.blobArenaBytes) {
        return rawPayload.data();
    }
    auto sliceRes = AllocateArenaSlice(static_cast<u32>(rawPayload.size()));
    if (!sliceRes) {
        return Err(sliceRes.error());
    }
    std::memcpy(sliceRes->data(), rawPayload.data(), rawPayload.size());
    return sliceRes->data();
}

Result<void, AssetErrc> AssetBank::CommitTexture(
    u16 slot,
    renderer::TextureHandle gpuHandle,
    u16 width,
    u16 height,
    u32 vramBytes) noexcept {
    if (slot == 0 || slot >= m_highWaterSlot || m_slots[slot].kind != AssetKind::Texture) {
        return Err(AssetErrc::NotFound);
    }

    SlotMeta& meta = m_slots[slot];
    const u16 tIdx = meta.payloadSlot;

    m_currentVramBytes -= m_texVramBytes[tIdx];
    m_textures[tIdx] = TextureRecord{gpuHandle, width, height};
    m_texVramBytes[tIdx] = vramBytes;
    m_currentVramBytes += vramBytes;

    CompleteSlot(meta, AssetState::Ready);

    for (u16 a = 1; a < m_highWaterAtlas; ++a) {
        if (m_atlases[a].textureSlot == tIdx && m_atlases[a].firstSprite != 0) {
            const u32 end = static_cast<u32>(m_atlases[a].firstSprite) + m_atlases[a].spriteCount;
            for (u32 s = m_atlases[a].firstSprite; s < end; ++s) {
                m_sprites[s].gpuTexture = gpuHandle;
            }
        }
    }
    return {};
}

Result<void, AssetErrc> AssetBank::CommitAtlas(
    u16 slot,
    u16 backingTextureSlot,
    std::span<const u8> rawPayload) noexcept {
    if (slot == 0 || slot >= m_highWaterSlot || m_slots[slot].kind != AssetKind::Atlas) {
        return Err(AssetErrc::NotFound);
    }
    if (rawPayload.size() < sizeof(CookedAtlasHeader)) {
        MarkFailed(slot);
        return Err(AssetErrc::CorruptedHeader);
    }

    CookedAtlasHeader hdr{};
    std::memcpy(&hdr, rawPayload.data(), sizeof(CookedAtlasHeader));

    const std::size_t framesBytes = static_cast<std::size_t>(hdr.spriteCount) * sizeof(SpriteFrame);
    const std::size_t namesBytes  = static_cast<std::size_t>(hdr.namedFrameCount) * sizeof(CookedFrameName);
    if (hdr.spriteCount == 0 || rawPayload.size() < sizeof(CookedAtlasHeader) + framesBytes + namesBytes) {
        MarkFailed(slot);
        return Err(AssetErrc::CorruptedPayload);
    }

    SlotMeta& meta = m_slots[slot];
    const u16 aIdx = meta.payloadSlot;
    AtlasRecord& rec = m_atlases[aIdx];
    AtlasCold& cold = m_atlasCold[aIdx];

    u16 firstSprite = cold.allocFirstSprite;
    if (firstSprite == 0 || cold.allocSpriteCap < hdr.spriteCount) {
        if (m_spriteCount + hdr.spriteCount > m_cfg.maxSprites) {
            MarkFailed(slot);
            return Err(AssetErrc::PoolExhausted);
        }
        firstSprite = static_cast<u16>(m_spriteCount);
        m_spriteCount += hdr.spriteCount;
        cold.allocFirstSprite = firstSprite;
        cold.allocSpriteCap = hdr.spriteCount;
    }

    if (hdr.namedFrameCount > 0) {
        u32 firstNamed = cold.firstNamedFrame;
        if (cold.allocNamedCap < hdr.namedFrameCount) {
            if (m_namedFrameCount + hdr.namedFrameCount > m_cfg.maxNamedFrames) {
                MarkFailed(slot);
                return Err(AssetErrc::PoolExhausted);
            }
            firstNamed = m_namedFrameCount;
            m_namedFrameCount += hdr.namedFrameCount;
            cold.firstNamedFrame = firstNamed;
            cold.allocNamedCap = hdr.namedFrameCount;
        }
        const u8* namesSrc = rawPayload.data() + sizeof(CookedAtlasHeader) + framesBytes;
        std::memcpy(&m_namedFrames[firstNamed], namesSrc, namesBytes);
    }
    cold.namedFrameCount = hdr.namedFrameCount;

    const u16 texPayload = (backingTextureSlot < m_highWaterSlot &&
                            m_slots[backingTextureSlot].kind == AssetKind::Texture)
                               ? m_slots[backingTextureSlot].payloadSlot
                               : 0u;
    if (backingTextureSlot != 0 && backingTextureSlot < m_highWaterSlot) {
        m_slots[backingTextureSlot].scopeMask |= meta.scopeMask;
    }

    const renderer::TextureHandle resolvedGpu = m_textures[texPayload].gpu;
    const u8* framesSrc = rawPayload.data() + sizeof(CookedAtlasHeader);
    std::memcpy(&m_sprites[firstSprite], framesSrc, framesBytes);

    for (u16 i = 0; i < hdr.spriteCount; ++i) {
        m_sprites[firstSprite + i].gpuTexture = resolvedGpu;
        m_spriteGens[firstSprite + i] = meta.generation;
    }

    rec.firstSprite = firstSprite;
    rec.spriteCount = hdr.spriteCount;
    rec.textureSlot = texPayload;
    rec.generation  = meta.generation;

    CompleteSlot(meta, AssetState::Ready);
    return {};
}

Result<void, AssetErrc> AssetBank::CommitBlob(
    u16 slot,
    std::span<const u8> rawPayload,
    u32 schemaHash,
    bool isMemoryMapped) noexcept {
    if (slot == 0 || slot >= m_highWaterSlot || m_slots[slot].kind != AssetKind::Blob) {
        return Err(AssetErrc::NotFound);
    }

    auto ptrRes = ResolveArenaPayload(rawPayload, isMemoryMapped);
    if (!ptrRes) {
        MarkFailed(slot);
        return Err(ptrRes.error());
    }

    SlotMeta& meta = m_slots[slot];
    m_blobs[meta.payloadSlot] = BlobRecord{
        *ptrRes ? *ptrRes : kFallbackZeroBlob,
        static_cast<u32>(rawPayload.size()),
        schemaHash};
    CompleteSlot(meta, AssetState::Ready);
    return {};
}

Result<void, AssetErrc> AssetBank::CommitAudio(
    u16 slot,
    std::span<const u8> rawPayload,
    u16 sampleRate,
    u8 channels,
    bool isMemoryMapped) noexcept {
    if (slot == 0 || slot >= m_highWaterSlot || m_slots[slot].kind != AssetKind::Audio) {
        return Err(AssetErrc::NotFound);
    }

    auto ptrRes = ResolveArenaPayload(rawPayload, isMemoryMapped);
    if (!ptrRes) {
        MarkFailed(slot);
        return Err(ptrRes.error());
    }

    const u32 divisor = static_cast<u32>(sizeof(i16)) * (channels > 0 ? channels : 1u);
    SlotMeta& meta = m_slots[slot];
    m_audio[meta.payloadSlot] = AudioRecord{
        *ptrRes ? reinterpret_cast<const i16*>(*ptrRes) : kFallbackSilencePcm,
        static_cast<u32>(rawPayload.size()) / divisor,
        sampleRate,
        channels,
        0};
    CompleteSlot(meta, AssetState::Ready);
    return {};
}

void AssetBank::MarkFailed(u16 slot) noexcept {
    if (slot != 0 && slot < m_highWaterSlot) {
        CompleteSlot(m_slots[slot], AssetState::Failed);
    }
}

u16 AssetBank::FindNamedFrame(AtlasHandle h, u32 nameHash) const noexcept {
    const u32 aIdx = (h.index < m_highWaterAtlas && m_atlases[h.index].generation == h.generation)
                         ? h.index
                         : 0u;
    if (aIdx == 0) {
        return 0xFFFFu;
    }
    const AtlasCold& cold = m_atlasCold[aIdx];
    const auto begin = m_namedFrames + cold.firstNamedFrame;
    const auto end   = begin + cold.namedFrameCount;

    const auto it = std::lower_bound(begin, end, nameHash, [](const CookedFrameName& entry, u32 hash) {
        return entry.nameHash < hash;
    });
    if (it != end && it->nameHash == nameHash) {
        return it->frameOffset;
    }
    return 0xFFFFu;
}

u32 AssetBank::EvictUnpinned(std::span<renderer::TextureHandle> outDestroyedTextures) noexcept {
    if (m_currentVramBytes <= m_cfg.vramBudgetBytes || outDestroyedTextures.empty()) {
        return 0;
    }

    u32 evictedCount = 0;
    while (m_currentVramBytes > m_cfg.vramBudgetBytes && evictedCount < outDestroyedTextures.size()) {
        u16 oldestSlot = 0;
        u32 oldestAge  = 0;

        for (u16 t = 1; t < m_highWaterTex; ++t) {
            const u16 slot = m_texMasterSlot[t];
            if (slot == 0) {
                continue;
            }
            const SlotMeta& meta = m_slots[slot];
            if (meta.state != AssetState::Ready || meta.scopeMask != 0) {
                continue;
            }
            const u32 age = m_currentFrame - meta.lastUsedFrame;
            if (age >= m_cfg.evictGraceFrames && age >= oldestAge) {
                oldestAge = age;
                oldestSlot = slot;
            }
        }

        if (oldestSlot == 0) {
            break;
        }

        SlotMeta& victim = m_slots[oldestSlot];
        const u16 tIdx = victim.payloadSlot;
        outDestroyedTextures[evictedCount++] = m_textures[tIdx].gpu;

        for (u16 i = 1; i < m_highWaterSlot; ++i) {
            SlotMeta& aMeta = m_slots[i];
            if (aMeta.kind != AssetKind::Atlas) {
                continue;
            }
            const u16 a = aMeta.payloadSlot;
            if (m_atlases[a].textureSlot != tIdx) {
                continue;
            }
            if (m_atlases[a].firstSprite != 0) {
                const u32 end = static_cast<u32>(m_atlases[a].firstSprite) + m_atlases[a].spriteCount;
                for (u32 s = m_atlases[a].firstSprite; s < end; ++s) {
                    m_sprites[s].gpuTexture = renderer::kWhiteTexture;
                }
            }
            m_atlases[a].textureSlot = 0;

            if (aMeta.scopeMask == 0) {
                RemoveHash(m_slotIds[i].value);
                m_slotIds[i] = AssetId{};
                CompleteSlot(aMeta, AssetState::Empty);
                aMeta.kind = AssetKind::None;
                if (aMeta.generation < 0xFFFFu) {
                    ++aMeta.generation;
                    m_atlases[a] = AtlasRecord{0, 1, m_freeAtlasHead, aMeta.generation};
                    m_freeAtlasHead = a;
                    aMeta.payloadSlot = m_freeSlotHead;
                    m_freeSlotHead = i;
                } else {
                    m_atlases[a] = m_atlases[0];
                }
            }
        }

        m_currentVramBytes -= m_texVramBytes[tIdx];
        m_texVramBytes[tIdx] = 0;
        m_textures[tIdx] = m_textures[0];
        m_texMasterSlot[tIdx] = 0;

        RemoveHash(m_slotIds[oldestSlot].value);
        m_slotIds[oldestSlot] = AssetId{};
        CompleteSlot(victim, AssetState::Empty);
        victim.kind = AssetKind::None;

        if (victim.generation < 0xFFFFu) {
            ++victim.generation;
            m_textures[tIdx].width = m_freeTexHead;
            m_freeTexHead = tIdx;
            victim.payloadSlot = m_freeSlotHead;
            m_freeSlotHead = oldestSlot;
        }
    }
    return evictedCount;
}

u64 AssetBank::ComputeAuditHash() const noexcept {
    u64 hash = 14695981039346656037ULL;
    auto mix = [&](const void* ptr, std::size_t len) noexcept {
        const auto* bytes = static_cast<const u8*>(ptr);
        for (std::size_t i = 0; i < len; ++i) {
            hash = (hash ^ bytes[i]) * 1099511628211ULL;
        }
    };

    for (u16 i = 1; i < m_highWaterSlot; ++i) {
        const SlotMeta& meta = m_slots[i];
        if (meta.state != AssetState::Ready) {
            continue;
        }
        mix(&m_slotIds[i].value, sizeof(u64));
        mix(&meta.kind, sizeof(AssetKind));
        mix(&meta.version, sizeof(u16));

        if (meta.kind == AssetKind::Blob) {
            const BlobRecord& b = m_blobs[meta.payloadSlot];
            mix(&b.size, sizeof(u32));
            mix(&b.schemaHash, sizeof(u32));
            if (b.data && b.size > 0) {
                mix(b.data, b.size);
            }
        } else if (meta.kind == AssetKind::Atlas) {
            const AtlasRecord& a = m_atlases[meta.payloadSlot];
            mix(&a.spriteCount, sizeof(u16));
            for (u16 s = 0; s < a.spriteCount; ++s) {
                const SpriteFrame& sp = m_sprites[a.firstSprite + s];
                mix(&sp.uv, sizeof(renderer::FRect));
                mix(&sp.width, sizeof(f32) * 2);
                mix(&sp.pivotX, sizeof(i16) * 2);
            }
        }
    }
    return hash;
}

Result<u32, AssetErrc> AssetBank::DecompressLz4(
    std::span<const u8> src,
    std::span<u8> dst) noexcept {
    const u8* ip = src.data();
    const u8* const iend = ip + src.size();
    u8* op = dst.data();
    u8* const oend = op + dst.size();

    while (ip < iend) {
        const u8 token = *ip++;
        std::size_t literalLen = token >> 4;

        if (literalLen == 15) {
            u8 s = 0;
            do {
                if (ip >= iend) return Err(AssetErrc::CorruptedPayload);
                s = *ip++;
                literalLen += s;
            } while (s == 255);
        }

        if (literalLen > static_cast<std::size_t>(iend - ip) ||
            literalLen > static_cast<std::size_t>(oend - op)) {
            return Err(AssetErrc::CorruptedPayload);
        }
        std::memcpy(op, ip, literalLen);
        ip += literalLen;
        op += literalLen;

        if (ip >= iend) {
            break;
        }

        if (static_cast<std::size_t>(iend - ip) < 2) {
            return Err(AssetErrc::CorruptedPayload);
        }
        const u16 offset = static_cast<u16>(ip[0] | (static_cast<u16>(ip[1]) << 8));
        ip += 2;

        if (offset == 0 || offset > static_cast<std::size_t>(op - dst.data())) {
            return Err(AssetErrc::CorruptedPayload);
        }

        std::size_t matchLen = (token & 0x0Fu) + 4u;
        if (matchLen == 19) {
            u8 s = 0;
            do {
                if (ip >= iend) return Err(AssetErrc::CorruptedPayload);
                s = *ip++;
                matchLen += s;
            } while (s == 255);
        }

        if (matchLen > static_cast<std::size_t>(oend - op)) {
            return Err(AssetErrc::CorruptedPayload);
        }

        const u8* match = op - offset;
        if (offset >= 8) {
            while (matchLen >= 8) {
                std::memcpy(op, match, 8);
                op += 8;
                match += 8;
                matchLen -= 8;
            }
        }
        while (matchLen-- > 0) {
            *op++ = *match++;
        }
    }

    return static_cast<u32>(op - dst.data());
}

} // namespace en::assets
