#include "assets/AssetBank.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace en::assets {

namespace {

alignas(16) constexpr u8 kFallbackZeroBlob[64] = {0};
alignas(16) constexpr i16 kFallbackSilencePcm[64] = {0};

[[nodiscard]] constexpr u32 NextPowerOfTwo(u32 v) noexcept {
    if (v <= 16) {
        return 16;
    }
    return 1u << (32u - std::countl_zero(v - 1u));
}

} // namespace

Result<void, AssetErrc> AssetBank::Init(const AssetBankConfig& cfg) noexcept {
    Shutdown();
    m_cfg = cfg;

    m_slotCap = (cfg.maxSlots < 2) ? 2 : cfg.maxSlots;
    m_slots = std::make_unique<SlotMeta[]>(m_slotCap);
    m_slotIds = std::make_unique<AssetId[]>(m_slotCap);
    m_slotResidency = std::make_unique<u8[]>(m_slotCap);
    m_freeSlots = std::make_unique_for_overwrite<u16[]>(m_slotCap);

    m_freeSlotTop = 0;
    for (u16 i = m_slotCap - 1; i >= 1; --i) {
        m_slots[i].generation = 1;
        m_slots[i].version = 1;
        m_freeSlots[m_freeSlotTop++] = i;
    }
    m_slots[0].state = AssetState::Ready;
    m_slots[0].generation = 0;

    m_hashCap = NextPowerOfTwo(static_cast<u32>(m_slotCap) * 2u);
    m_hashMask = m_hashCap - 1u;
    m_hashTable = std::make_unique<HashSlot[]>(m_hashCap);

    m_texCap = (cfg.maxTextures < 2) ? 2 : cfg.maxTextures;
    m_textures = std::make_unique_for_overwrite<TextureRecord[]>(m_texCap);
    m_texGens = std::make_unique_for_overwrite<u16[]>(m_texCap);
    m_texVramBytes = std::make_unique_for_overwrite<u32[]>(m_texCap);
    m_texMasterSlot = std::make_unique_for_overwrite<u16[]>(m_texCap);
    m_freeTex = std::make_unique_for_overwrite<u16[]>(m_texCap);

    m_freeTexTop = 0;
    for (u16 i = m_texCap - 1; i >= 1; --i) {
        m_textures[i] = TextureRecord{};
        m_texGens[i] = 1;
        m_texVramBytes[i] = 0;
        m_texMasterSlot[i] = 0;
        m_freeTex[m_freeTexTop++] = i;
    }
    m_textures[0] = TextureRecord{renderer::kWhiteTexture, 1, 1};
    m_texGens[0] = 0;
    m_texVramBytes[0] = 0;
    m_texMasterSlot[0] = 0;

    m_atlasCap = (cfg.maxAtlases < 2) ? 2 : cfg.maxAtlases;
    m_atlases = std::make_unique_for_overwrite<AtlasRecord[]>(m_atlasCap);
    m_atlasGens = std::make_unique_for_overwrite<u16[]>(m_atlasCap);
    m_atlasCold = std::make_unique_for_overwrite<AtlasCold[]>(m_atlasCap);
    m_freeAtlases = std::make_unique_for_overwrite<u16[]>(m_atlasCap);

    m_freeAtlasTop = 0;
    for (u16 i = m_atlasCap - 1; i >= 1; --i) {
        m_atlases[i] = AtlasRecord{0, 1, 0, 0};
        m_atlasGens[i] = 1;
        m_atlasCold[i] = AtlasCold{};
        m_freeAtlases[m_freeAtlasTop++] = i;
    }
    m_atlases[0] = AtlasRecord{0, 1, 0, 0};
    m_atlasGens[0] = 0;
    m_atlasCold[0] = AtlasCold{};

    m_spriteCap = (cfg.maxSprites < 2) ? 2 : cfg.maxSprites;
    m_sprites = std::make_unique_for_overwrite<SpriteFrame[]>(m_spriteCap);
    m_spriteGens = std::make_unique_for_overwrite<u16[]>(m_spriteCap);
    m_sprites[0] = SpriteFrame{};
    m_spriteGens[0] = 0;
    m_spriteCount = 1;

    m_namedFrameCap = cfg.maxNamedFrames;
    m_namedFrames = std::make_unique_for_overwrite<CookedFrameName[]>(m_namedFrameCap);
    m_namedFrameCount = 0;

    m_blobCap = (cfg.maxBlobs < 2) ? 2 : cfg.maxBlobs;
    m_blobs = std::make_unique_for_overwrite<BlobRecord[]>(m_blobCap);
    m_blobGens = std::make_unique_for_overwrite<u16[]>(m_blobCap);
    m_freeBlobs = std::make_unique_for_overwrite<u16[]>(m_blobCap);

    m_freeBlobTop = 0;
    for (u16 i = m_blobCap - 1; i >= 1; --i) {
        m_blobs[i] = BlobRecord{kFallbackZeroBlob, 0, 0};
        m_blobGens[i] = 1;
        m_freeBlobs[m_freeBlobTop++] = i;
    }
    m_blobs[0] = BlobRecord{kFallbackZeroBlob, sizeof(kFallbackZeroBlob), 0};
    m_blobGens[0] = 0;

    m_audioCap = (cfg.maxAudio < 2) ? 2 : cfg.maxAudio;
    m_audio = std::make_unique_for_overwrite<AudioRecord[]>(m_audioCap);
    m_audioGens = std::make_unique_for_overwrite<u16[]>(m_audioCap);
    m_freeAudio = std::make_unique_for_overwrite<u16[]>(m_audioCap);

    m_freeAudioTop = 0;
    for (u16 i = m_audioCap - 1; i >= 1; --i) {
        m_audio[i] = AudioRecord{kFallbackSilencePcm, 0, 48000, 2, 0};
        m_audioGens[i] = 1;
        m_freeAudio[m_freeAudioTop++] = i;
    }
    m_audio[0] = AudioRecord{kFallbackSilencePcm, 32, 48000, 2, 0};
    m_audioGens[0] = 0;

    m_blobArenaCap = cfg.blobArenaBytes;
    if (m_blobArenaCap > 0) {
        m_blobArena = std::make_unique_for_overwrite<u8[]>(m_blobArenaCap);
    }
    m_blobArenaHead = 0;

    return {};
}

void AssetBank::Shutdown() noexcept {
    m_slots.reset();
    m_slotIds.reset();
    m_slotResidency.reset();
    m_freeSlots.reset();
    m_hashTable.reset();
    m_textures.reset();
    m_texGens.reset();
    m_texVramBytes.reset();
    m_texMasterSlot.reset();
    m_freeTex.reset();
    m_atlases.reset();
    m_atlasGens.reset();
    m_atlasCold.reset();
    m_freeAtlases.reset();
    m_sprites.reset();
    m_spriteGens.reset();
    m_namedFrames.reset();
    m_blobs.reset();
    m_blobGens.reset();
    m_freeBlobs.reset();
    m_audio.reset();
    m_audioGens.reset();
    m_freeAudio.reset();
    m_blobArena.reset();

    m_slotCap = 0;
    m_activeScopesMask = 0;
    m_currentVramBytes = 0;
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
    const u32 clearMask = ~(1u << scope);
    m_activeScopesMask &= clearMask;

    for (u16 i = 1; i < m_slotCap; ++i) {
        if (m_slots[i].scopeMask & (1u << scope)) {
            m_slots[i].scopeMask &= clearMask;
            m_slots[i].lastUsedFrame = m_currentFrame;
        }
    }
}

void AssetBank::InsertHash(u64 key, u16 slotIndex) noexcept {
    // Multiplicative Fibonacci mix for uniform bucket distribution.
    u32 idx = static_cast<u32>((key * 11400714819323198485ULL) >> 32) & m_hashMask;
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
    u32 idx = static_cast<u32>((key * 11400714819323198485ULL) >> 32) & m_hashMask;
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
    u32 idx = static_cast<u32>((id.value * 11400714819323198485ULL) >> 32) & m_hashMask;
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
        meta.lastUsedFrame = m_currentFrame;
        if (residency == Residency::Sim) {
            m_slotResidency[existing] = static_cast<u8>(Residency::Sim);
        }
        if (meta.kind == AssetKind::Atlas) {
            const u16 texPayload = m_atlases[meta.payloadSlot].textureSlot;
            if (texPayload != 0) {
                const u16 texMaster = m_texMasterSlot[texPayload];
                m_slots[texMaster].scopeMask |= scopeBit;
                m_slots[texMaster].lastUsedFrame = m_currentFrame;
            }
        }
        return existing;
    }

    if (m_freeSlotTop == 0) {
        return Err(AssetErrc::PoolExhausted);
    }

    u16 payloadIdx = 0;
    switch (kind) {
        case AssetKind::Texture:
            if (m_freeTexTop == 0) return Err(AssetErrc::PoolExhausted);
            payloadIdx = m_freeTex[--m_freeTexTop];
            m_textures[payloadIdx] = m_textures[0];
            break;
        case AssetKind::Atlas:
            if (m_freeAtlasTop == 0) return Err(AssetErrc::PoolExhausted);
            payloadIdx = m_freeAtlases[--m_freeAtlasTop];
            m_atlases[payloadIdx] = m_atlases[0];
            break;
        case AssetKind::Blob:
            if (m_freeBlobTop == 0) return Err(AssetErrc::PoolExhausted);
            payloadIdx = m_freeBlobs[--m_freeBlobTop];
            m_blobs[payloadIdx] = m_blobs[0];
            break;
        case AssetKind::Audio:
            if (m_freeAudioTop == 0) return Err(AssetErrc::PoolExhausted);
            payloadIdx = m_freeAudio[--m_freeAudioTop];
            m_audio[payloadIdx] = m_audio[0];
            break;
        default:
            return Err(AssetErrc::NotFound);
    }

    const u16 slot = m_freeSlots[--m_freeSlotTop];
    SlotMeta& meta = m_slots[slot];
    meta.scopeMask = scopeBit;
    meta.lastUsedFrame = m_currentFrame;
    meta.payloadSlot = payloadIdx;
    meta.kind = kind;
    meta.state = AssetState::Loading;

    if (kind == AssetKind::Texture) {
        m_texMasterSlot[payloadIdx] = slot;
        m_texGens[payloadIdx] = meta.generation;
    } else if (kind == AssetKind::Atlas) {
        m_atlasCold[payloadIdx].masterSlot = slot;
        m_atlasGens[payloadIdx] = meta.generation;
    } else if (kind == AssetKind::Blob) {
        m_blobGens[payloadIdx] = meta.generation;
    } else if (kind == AssetKind::Audio) {
        m_audioGens[payloadIdx] = meta.generation;
    }

    m_slotIds[slot] = id;
    m_slotResidency[slot] = static_cast<u8>(residency);
    InsertHash(id.value, slot);

    outNeedsLoad = true;
    return slot;
}

Result<void, AssetErrc> AssetBank::CommitTexture(
    u16 slot,
    renderer::TextureHandle gpuHandle,
    u16 width,
    u16 height,
    u32 vramBytes) noexcept {
    if (slot == 0 || slot >= m_slotCap || m_slots[slot].kind != AssetKind::Texture) {
        return Err(AssetErrc::NotFound);
    }

    SlotMeta& meta = m_slots[slot];
    const u16 tIdx = meta.payloadSlot;

    m_currentVramBytes -= m_texVramBytes[tIdx];
    m_textures[tIdx] = TextureRecord{gpuHandle, width, height};
    m_texVramBytes[tIdx] = vramBytes;
    m_currentVramBytes += vramBytes;

    meta.state = AssetState::Ready;
    ++meta.version;

    // Propagate the resolved GPU TextureHandle directly into every SpriteFrame owned by dependent atlases.
    for (u16 a = 1; a < m_atlasCap; ++a) {
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
    if (slot == 0 || slot >= m_slotCap || m_slots[slot].kind != AssetKind::Atlas) {
        return Err(AssetErrc::NotFound);
    }
    if (rawPayload.size() < sizeof(CookedAtlasHeader)) {
        return Err(AssetErrc::CorruptedHeader);
    }

    CookedAtlasHeader hdr{};
    std::memcpy(&hdr, rawPayload.data(), sizeof(CookedAtlasHeader));

    const std::size_t framesBytes = static_cast<std::size_t>(hdr.spriteCount) * sizeof(SpriteFrame);
    const std::size_t namesBytes  = static_cast<std::size_t>(hdr.namedFrameCount) * sizeof(CookedFrameName);
    if (hdr.spriteCount == 0 || rawPayload.size() < sizeof(CookedAtlasHeader) + framesBytes + namesBytes) {
        return Err(AssetErrc::CorruptedPayload);
    }

    SlotMeta& meta = m_slots[slot];
    const u16 aIdx = meta.payloadSlot;
    AtlasRecord& rec = m_atlases[aIdx];

    // Reuse existing sprite slice on hot-reload if frame count matches; otherwise allocate from flat pool.
    u16 firstSprite = rec.firstSprite;
    if (firstSprite == 0 || rec.spriteCount != hdr.spriteCount) {
        if (m_spriteCount + hdr.spriteCount > m_spriteCap) {
            return Err(AssetErrc::PoolExhausted);
        }
        firstSprite = static_cast<u16>(m_spriteCount);
        m_spriteCount += hdr.spriteCount;
    }

    const u16 texPayload = (backingTextureSlot < m_slotCap && m_slots[backingTextureSlot].kind == AssetKind::Texture)
                               ? m_slots[backingTextureSlot].payloadSlot
                               : 0u;
    if (backingTextureSlot != 0) {
        m_slots[backingTextureSlot].scopeMask |= meta.scopeMask;
    }

    const renderer::TextureHandle resolvedGpu = m_textures[texPayload].gpu;
    const u8* framesSrc = rawPayload.data() + sizeof(CookedAtlasHeader);
    std::memcpy(&m_sprites[firstSprite], framesSrc, framesBytes);

    for (u16 i = 0; i < hdr.spriteCount; ++i) {
        m_sprites[firstSprite + i].gpuTexture = resolvedGpu;
        m_spriteGens[firstSprite + i] = meta.generation;
    }

    if (hdr.namedFrameCount > 0) {
        if (m_namedFrameCount + hdr.namedFrameCount > m_namedFrameCap) {
            return Err(AssetErrc::PoolExhausted);
        }
        const u32 firstNamed = m_namedFrameCount;
        std::memcpy(&m_namedFrames[firstNamed], framesSrc + framesBytes, namesBytes);
        m_atlasCold[aIdx].firstNamedFrame = firstNamed;
        m_atlasCold[aIdx].namedFrameCount = hdr.namedFrameCount;
        m_namedFrameCount += hdr.namedFrameCount;
    }

    rec.firstSprite = firstSprite;
    rec.spriteCount = hdr.spriteCount;
    rec.textureSlot = texPayload;

    meta.state = AssetState::Ready;
    ++meta.version;
    return {};
}

Result<const u8*, AssetErrc> AssetBank::ResolveArenaPayload(
    std::span<const u8> rawPayload,
    bool isMemoryMapped) noexcept {
    if (isMemoryMapped || rawPayload.empty()) {
        return rawPayload.data();
    }
    const u32 alignedHead = (m_blobArenaHead + 15u) & ~15u;
    if (alignedHead + rawPayload.size() > m_blobArenaCap) {
        return Err(AssetErrc::PoolExhausted);
    }
    std::memcpy(&m_blobArena[alignedHead], rawPayload.data(), rawPayload.size());
    m_blobArenaHead = alignedHead + static_cast<u32>(rawPayload.size());
    return &m_blobArena[alignedHead];
}

Result<void, AssetErrc> AssetBank::CommitBlob(
    u16 slot,
    std::span<const u8> rawPayload,
    u32 schemaHash,
    bool isMemoryMapped) noexcept {
    if (slot == 0 || slot >= m_slotCap || m_slots[slot].kind != AssetKind::Blob) {
        return Err(AssetErrc::NotFound);
    }

    auto ptrRes = ResolveArenaPayload(rawPayload, isMemoryMapped);
    if (!ptrRes) {
        return Err(ptrRes.error());
    }

    SlotMeta& meta = m_slots[slot];
    m_blobs[meta.payloadSlot] = BlobRecord{
        *ptrRes ? *ptrRes : kFallbackZeroBlob,
        static_cast<u32>(rawPayload.size()),
        schemaHash};
    meta.state = AssetState::Ready;
    ++meta.version;
    return {};
}

Result<void, AssetErrc> AssetBank::CommitAudio(
    u16 slot,
    std::span<const u8> rawPayload,
    u16 sampleRate,
    u8 channels,
    bool isMemoryMapped) noexcept {
    if (slot == 0 || slot >= m_slotCap || m_slots[slot].kind != AssetKind::Audio) {
        return Err(AssetErrc::NotFound);
    }

    auto ptrRes = ResolveArenaPayload(rawPayload, isMemoryMapped);
    if (!ptrRes) {
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
    meta.state = AssetState::Ready;
    ++meta.version;
    return {};
}

void AssetBank::MarkFailed(u16 slot) noexcept {
    if (slot != 0 && slot < m_slotCap) {
        m_slots[slot].state = AssetState::Failed;
    }
}

u16 AssetBank::FindNamedFrame(AtlasHandle h, u32 nameHash) const noexcept {
    const u32 aIdx = (h.index < m_atlasCap && m_atlasGens[h.index] == h.generation) ? h.index : 0u;
    if (aIdx == 0) {
        return 0xFFFFu;
    }
    const AtlasCold& cold = m_atlasCold[aIdx];
    const auto begin = m_namedFrames.get() + cold.firstNamedFrame;
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

        for (u16 i = 1; i < m_slotCap; ++i) {
            const SlotMeta& meta = m_slots[i];
            if (meta.kind != AssetKind::Texture || meta.state != AssetState::Ready || meta.scopeMask != 0) {
                continue;
            }
            const u32 age = m_currentFrame - meta.lastUsedFrame;
            if (age >= m_cfg.evictGraceFrames && age >= oldestAge) {
                oldestAge = age;
                oldestSlot = i;
            }
        }

        if (oldestSlot == 0) {
            break;
        }

        SlotMeta& victim = m_slots[oldestSlot];
        const u16 tIdx = victim.payloadSlot;
        outDestroyedTextures[evictedCount++] = m_textures[tIdx].gpu;

        // Scrub dependent atlas frames and evict unpinned dependent atlases so future re-pins reload cleanly.
        for (u16 a = 1; a < m_atlasCap; ++a) {
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

            const u16 atlasMaster = m_atlasCold[a].masterSlot;
            if (atlasMaster != 0 && m_slots[atlasMaster].scopeMask == 0) {
                SlotMeta& aMeta = m_slots[atlasMaster];
                RemoveHash(m_slotIds[atlasMaster].value);
                m_slotIds[atlasMaster] = AssetId{};
                aMeta.state = AssetState::Empty;
                aMeta.kind  = AssetKind::None;
                if (aMeta.generation < 0xFFFFu) {
                    ++aMeta.generation;
                    m_atlasGens[a] = aMeta.generation;
                    m_freeAtlases[m_freeAtlasTop++] = a;
                    m_freeSlots[m_freeSlotTop++]    = atlasMaster;
                }
            }
        }

        m_currentVramBytes -= m_texVramBytes[tIdx];
        m_texVramBytes[tIdx] = 0;
        m_textures[tIdx] = m_textures[0];

        RemoveHash(m_slotIds[oldestSlot].value);
        m_slotIds[oldestSlot] = AssetId{};
        victim.state = AssetState::Empty;
        victim.kind = AssetKind::None;

        if (victim.generation < 0xFFFFu) {
            ++victim.generation;
            m_texGens[tIdx] = victim.generation;
            m_freeTex[m_freeTexTop++] = tIdx;
            m_freeSlots[m_freeSlotTop++] = oldestSlot;
        }
    }
    return evictedCount;
}

bool AssetBank::IsScopeSimReady(ScopeId scope) const noexcept {
    if (scope >= kMaxScopes) {
        return true;
    }
    const u32 bit = (1u << scope);
    for (u16 i = 1; i < m_slotCap; ++i) {
        if ((m_slots[i].scopeMask & bit) &&
            m_slotResidency[i] == static_cast<u8>(Residency::Sim) &&
            m_slots[i].state == AssetState::Loading) {
            return false;
        }
    }
    return true;
}

u64 AssetBank::ComputeAuditHash() const noexcept {
    u64 hash = 14695981039346656037ULL;
    auto mix = [&](const void* ptr, std::size_t len) noexcept {
        const auto* bytes = static_cast<const u8*>(ptr);
        for (std::size_t i = 0; i < len; ++i) {
            hash = (hash ^ bytes[i]) * 1099511628211ULL;
        }
    };

    for (u16 i = 1; i < m_slotCap; ++i) {
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

        if (ip + literalLen > iend || op + literalLen > oend) {
            return Err(AssetErrc::CorruptedPayload);
        }
        std::memcpy(op, ip, literalLen);
        ip += literalLen;
        op += literalLen;

        if (ip >= iend) {
            break;
        }

        if (ip + 2 > iend) {
            return Err(AssetErrc::CorruptedPayload);
        }
        const u16 offset = static_cast<u16>(ip[0] | (static_cast<u16>(ip[1]) << 8));
        ip += 2;

        if (offset == 0 || op - offset < dst.data()) {
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

        if (op + matchLen > oend) {
            return Err(AssetErrc::CorruptedPayload);
        }

        const u8* match = op - offset;
        if (offset >= 8 && op + matchLen <= oend) {
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