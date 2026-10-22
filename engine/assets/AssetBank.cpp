#include "assets/AssetBank.h"

namespace en {

namespace {

constexpr u8 kFallbackWhitePixel[4]{255U, 255U, 255U, 255U};
constexpr u8 kFallbackBlobByte{0U};
constexpr f32 kFallbackSilenceSample[2]{0.0f, 0.0f};

[[nodiscard]] constexpr u16 NextGenerationNonZero(u16 gen) noexcept {
    const u16 next = static_cast<u16>(gen + 1U);
    return (next == 0U) ? 1U : next;
}

[[nodiscard]] constexpr u32 ReadLeU32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8U) |
           (static_cast<u32>(p[2]) << 16U) |
           (static_cast<u32>(p[3]) << 24U);
}

} // namespace

Result<void, AssetErrc> AssetBank::Init(const AssetBankConfig& cfg) noexcept {
    Shutdown();
    if (cfg.arenaBytes < 65536U || cfg.maxSlots < 4U || cfg.maxSprites < 4U ||
        cfg.maxAtlases < 2U || cfg.maxTextures < 2U || cfg.maxBlobs < 2U || cfg.maxAudio < 2U) {
        return Err(AssetErrc::InvalidConfig);
    }
    if (!m_arena.Init(cfg.arenaBytes)) {
        return Err(AssetErrc::OutOfMemory);
    }

    // m_slots is carved first at offset 0 of m_arena, serving as the arena base address reference
    auto slots = m_arena.Carve<SlotMeta>(cfg.maxSlots);
    auto slotIds = m_arena.Carve<u64>(cfg.maxSlots);
    auto sprites = m_arena.Carve<SpriteFrame>(cfg.maxSprites);
    auto spriteGens = m_arena.Carve<u16>(cfg.maxSprites);
    auto spriteNames = m_arena.Carve<u32>(cfg.maxSprites);
    auto atlases = m_arena.Carve<AtlasRecord>(cfg.maxAtlases);
    auto textures = m_arena.Carve<TextureRecord>(cfg.maxTextures);
    auto texGens = m_arena.Carve<u16>(cfg.maxTextures);
    auto blobs = m_arena.Carve<BlobRecord>(cfg.maxBlobs);
    auto audio = m_arena.Carve<AudioRecord>(cfg.maxAudio);
    auto audioGens = m_arena.Carve<u16>(cfg.maxAudio);

    if (slots.empty() || slotIds.empty() || sprites.empty() || spriteGens.empty() ||
        spriteNames.empty() || atlases.empty() || textures.empty() || texGens.empty() ||
        blobs.empty() || audio.empty() || audioGens.empty() ||
        !m_idToSlot.Init(m_arena, static_cast<u32>(cfg.maxSlots) * 2U)) {
        Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }

    m_sprites = sprites.data();
    m_spriteGens = spriteGens.data();
    m_atlases = atlases.data();
    m_textures = textures.data();
    m_blobs = blobs.data();
    m_audio = audio.data();
    m_slots = slots.data();
    m_spriteNameHashes = spriteNames.data();
    m_texGens = texGens.data();
    m_audioGens = audioGens.data();
    m_slotAssetIds = slotIds.data();
    m_freePayloadHead = nullptr;

    m_maxSlots = cfg.maxSlots;
    m_maxSprites = cfg.maxSprites;
    m_maxAtlases = cfg.maxAtlases;
    m_maxTextures = cfg.maxTextures;
    m_maxBlobs = cfg.maxBlobs;
    m_maxAudio = cfg.maxAudio;

    m_vramBudgetBytes = cfg.vramBudgetBytes;
    m_currentVramBytes = 0U;
    m_activeScopesMask = 0U;
    m_currentFrame = 1U;
    m_scopeSimPending.fill(0U);

    m_freeSlotHead = kNullAssetSlot;
    m_freeTexHead = kNullAssetSlot;
    m_freeAtlasHead = kNullAssetSlot;
    m_freeBlobHead = kNullAssetSlot;
    m_freeAudioHead = kNullAssetSlot;
    m_pendingGpuTexCount = 0U;
    m_evictedCount = 0U;
    m_hotReloadCount = 0U;

    // Populate non-null Slot 0 fallbacks so hot-path queries never branch on asset readiness
    m_slots[0] = SlotMeta{0xFFFFFFFFU, 1U, 1U, 1U, 0U, AssetKind::None, AssetState::Ready};
    m_sprites[0] = SpriteFrame{};
    m_spriteGens[0] = 1U;
    m_spriteNameHashes[0] = 0U;

    m_textures[0] = TextureRecord{kFallbackWhitePixel, kWhiteTexture, 1U, 1U};
    m_texGens[0] = 1U;

    m_atlases[0] = AtlasRecord{0U, 1U, 0U, 1U};
    m_blobs[0] = BlobRecord{&kFallbackBlobByte, 0U, 1U, 1U};

    m_audio[0] = AudioRecord{kFallbackSilenceSample, 0U, 48000U, 2U, false};
    m_audioGens[0] = 1U;

    m_highWaterSlot = 1U;
    m_highWaterSprite = 1U;
    m_highWaterAtlas = 1U;
    m_highWaterTex = 1U;
    m_highWaterBlob = 1U;
    m_highWaterAudio = 1U;
    m_payloadBaseMarker = m_arena.SaveMarker();
    return {};
}

void AssetBank::Shutdown() noexcept {
    m_arena.Shutdown();
    m_sprites = nullptr;
    m_spriteGens = nullptr;
    m_atlases = nullptr;
    m_textures = nullptr;
    m_blobs = nullptr;
    m_audio = nullptr;
    m_slots = nullptr;
    m_spriteNameHashes = nullptr;
    m_texGens = nullptr;
    m_audioGens = nullptr;
    m_slotAssetIds = nullptr;
    m_freePayloadHead = nullptr;
    m_activeScopesMask = 0U;
    m_highWaterSprite = 0U;
    m_highWaterAtlas = 0U;
    m_highWaterTex = 0U;
    m_highWaterBlob = 0U;
    m_highWaterAudio = 0U;
    m_highWaterSlot = 0U;
    m_scopeSimPending.fill(0U);
    m_currentVramBytes = 0U;
    m_pendingGpuTexCount = 0U;
    m_freeSlotHead = kNullAssetSlot;
    m_freeTexHead = kNullAssetSlot;
    m_freeAtlasHead = kNullAssetSlot;
    m_freeBlobHead = kNullAssetSlot;
    m_freeAudioHead = kNullAssetSlot;
}

Result<u8, AssetErrc> AssetBank::BeginScope() noexcept {
    if (m_activeScopesMask == 0xFFFFFFFFU) {
        return Err(AssetErrc::ScopeExhausted);
    }
    const u8 s = static_cast<u8>(std::countr_one(m_activeScopesMask));
    m_activeScopesMask |= (1U << s);
    m_scopeSimPending[s] = 0U;
    return s;
}

void AssetBank::EndScope(u8 scope) noexcept {
    if (scope >= kMaxAssetScopes) return;
    const u32 bit = (1U << scope);
    if ((m_activeScopesMask & bit) == 0U) return;
    m_activeScopesMask &= ~bit;
    m_scopeSimPending[scope] = 0U;
    for (u16 i = 1U; i < m_highWaterSlot; ++i) {
        SlotMeta& m = m_slots[i];
        m.scopeMask &= ~bit;
        if (m.state == AssetState::Loading) {
            m.lastUsedFrame &= ~bit;
        }
    }
}

std::span<u8> AssetBank::AllocatePayloadSlice(usize bytes) noexcept {
    if (bytes == 0U || m_slots == nullptr) return {};
    const u32 alignedBytes = static_cast<u32>(AlignUp(bytes, AlignedArena::kCacheLineBytes));
    if (alignedBytes == 0U) return {};

    // 1. Check coalesced intrusive free-block list before bumping the arena cursor
    FreePayloadBlock* prev = nullptr;
    FreePayloadBlock* curr = m_freePayloadHead;
    while (curr != nullptr) {
        if (curr->byteSize >= alignedBytes) {
            const u32 rem = curr->byteSize - alignedBytes;
            u8* rawPtr = nullptr;
            if (rem >= AlignedArena::kCacheLineBytes) {
                curr->byteSize = rem;
                rawPtr = reinterpret_cast<u8*>(curr) + rem;
            } else {
                if (prev != nullptr) {
                    prev->next = curr->next;
                } else {
                    m_freePayloadHead = curr->next;
                }
                rawPtr = reinterpret_cast<u8*>(curr);
            }
            detail::ZeroAlignedBlocks(rawPtr, alignedBytes);
            return std::span<u8>(rawPtr, bytes);
        }
        prev = curr;
        curr = curr->next;
    }

    // 2. Carve from the top of the 64B-aligned arena
    return m_arena.Carve<u8>(bytes);
}

void AssetBank::FreePayloadSlice(const void* ptr, usize bytes) noexcept {
    if (ptr == nullptr || bytes == 0U || m_slots == nullptr) return;
    const auto* basePtr = reinterpret_cast<const u8*>(m_slots);
    const auto* bytePtr = static_cast<const u8*>(ptr);
    if (bytePtr < basePtr) return;

    const usize offset = static_cast<usize>(bytePtr - basePtr);
    const u32 alignedBytes = static_cast<u32>(AlignUp(bytes, AlignedArena::kCacheLineBytes));
    if (offset < m_payloadBaseMarker || offset + alignedBytes > m_arena.UsedBytes()) {
        // Slice points to external zero-copy pack memory rather than m_arena
        return;
    }

    // Fast path: slice is at the very top of m_arena -> rewind arena cursor directly
    if (offset + alignedBytes == m_arena.UsedBytes()) {
        u32 newTop = static_cast<u32>(offset);
        // Also peel off any coalesced free block now touching the new top of m_arena
        FreePayloadBlock* prev = nullptr;
        FreePayloadBlock* curr = m_freePayloadHead;
        while (curr != nullptr && curr->next != nullptr) {
            prev = curr;
            curr = curr->next;
        }
        if (curr != nullptr) {
            const usize blkOff = static_cast<usize>(reinterpret_cast<const u8*>(curr) - basePtr);
            if (blkOff + curr->byteSize == newTop) {
                if (prev != nullptr) {
                    prev->next = nullptr;
                } else {
                    m_freePayloadHead = nullptr;
                }
                newTop = static_cast<u32>(blkOff);
            }
        }
        m_arena.RollbackToMarker(newTop);
        return;
    }

    // General path: insert into address-sorted intrusive free list and coalesce adjacent blocks
    u8* mutablePtr = const_cast<u8*>(bytePtr);
    detail::ZeroAlignedBlocks(mutablePtr, alignedBytes);
    auto* blk = reinterpret_cast<FreePayloadBlock*>(mutablePtr);
    blk->byteSize = alignedBytes;
    blk->reserved = 0U;

    FreePayloadBlock* prev = nullptr;
    FreePayloadBlock* curr = m_freePayloadHead;
    while (curr != nullptr && reinterpret_cast<const u8*>(curr) < mutablePtr) {
        prev = curr;
        curr = curr->next;
    }

    blk->next = curr;
    if (prev != nullptr) {
        prev->next = blk;
    } else {
        m_freePayloadHead = blk;
    }

    // Coalesce with next block if contiguous
    if (curr != nullptr && (mutablePtr + blk->byteSize) == reinterpret_cast<u8*>(curr)) {
        blk->byteSize += curr->byteSize;
        blk->next = curr->next;
        curr->next = nullptr;
        curr->byteSize = 0U;
    }

    // Coalesce with previous block if contiguous
    if (prev != nullptr && (reinterpret_cast<u8*>(prev) + prev->byteSize) == mutablePtr) {
        prev->byteSize += blk->byteSize;
        prev->next = blk->next;
        blk->next = nullptr;
        blk->byteSize = 0U;
        blk = prev;
    }

    // If coalescing reached the top of m_arena, rewind the arena cursor
    const usize blkOff = static_cast<usize>(reinterpret_cast<const u8*>(blk) - basePtr);
    if (blkOff + blk->byteSize == m_arena.UsedBytes()) {
        if (m_freePayloadHead == blk) {
            m_freePayloadHead = nullptr;
        } else {
            FreePayloadBlock* p = m_freePayloadHead;
            while (p != nullptr && p->next != blk) {
                p = p->next;
            }
            if (p != nullptr) p->next = nullptr;
        }
        m_arena.RollbackToMarker(static_cast<u32>(blkOff));
    }
}

SpriteHandle AssetBank::FindNamedFrame(AtlasHandle h, u32 nameHash) const noexcept {
    if (nameHash != 0U && h.index < m_highWaterAtlas && m_atlases[h.index].generation == h.generation) {
        const AtlasRecord& rec = m_atlases[h.index];
        for (u16 s = 0U; s < rec.spriteCount; ++s) {
            const u16 idx = static_cast<u16>(rec.firstSprite + s);
            if (idx < m_highWaterSprite && m_spriteNameHashes[idx] == nameHash) {
                return SpriteHandle{idx, m_spriteGens[idx]};
            }
        }
    }
    return SpriteHandle{0U, 1U};
}

void AssetBank::PropagateAtlasGpuTexture(u16 texSlot, TextureHandle gpu) noexcept {
    for (u16 a = 1U; a < m_highWaterAtlas; ++a) {
        const AtlasRecord& ar = m_atlases[a];
        if (ar.generation != 0U && ar.textureSlot == texSlot) {
            for (u16 s = 0U; s < ar.spriteCount; ++s) {
                const u16 spIdx = static_cast<u16>(ar.firstSprite + s);
                if (spIdx < m_highWaterSprite) {
                    m_sprites[spIdx].gpuTexture = gpu;
                }
            }
        }
    }
}

void AssetBank::BindGpuTexture(u16 texSlot, TextureHandle gpu) noexcept {
    if (texSlot == 0U || texSlot >= m_highWaterTex || !gpu.IsValid()) return;
    TextureRecord& tr = m_textures[texSlot];
    if (tr.gpu == kWhiteTexture && tr.pixels != nullptr && m_pendingGpuTexCount > 0U) {
        --m_pendingGpuTexCount;
    }
    tr.gpu = gpu;
    PropagateAtlasGpuTexture(texSlot, gpu);
}

const SlotMeta* AssetBank::FindSlotByAssetId(AssetId id) const noexcept {
    const u16* slotIdx = m_idToSlot.Find(id.value);
    if (!slotIdx || *slotIdx >= m_highWaterSlot) return nullptr;
    return &m_slots[*slotIdx];
}

u16 AssetBank::GetAssetVersion(AssetId id) const noexcept {
    const SlotMeta* meta = FindSlotByAssetId(id);
    return meta ? meta->version : 0U;
}

TextureAssetHandle AssetBank::FindTextureHandle(AssetId id) const noexcept {
    const SlotMeta* m = FindSlotByAssetId(id);
    return (m && m->kind == AssetKind::Texture && m->payloadSlot != 0U && m->state != AssetState::Failed)
               ? TextureAssetHandle{m->payloadSlot, m->generation}
               : TextureAssetHandle{};
}

AtlasHandle AssetBank::FindAtlasHandle(AssetId id) const noexcept {
    const SlotMeta* m = FindSlotByAssetId(id);
    return (m && m->kind == AssetKind::Atlas && m->payloadSlot != 0U && m->state != AssetState::Failed)
               ? AtlasHandle{m->payloadSlot, m->generation}
               : AtlasHandle{};
}

BlobHandle AssetBank::FindBlobHandle(AssetId id) const noexcept {
    const SlotMeta* m = FindSlotByAssetId(id);
    return (m && m->kind == AssetKind::Blob && m->payloadSlot != 0U && m->state != AssetState::Failed)
               ? BlobHandle{m->payloadSlot, m->generation}
               : BlobHandle{};
}

AudioHandle AssetBank::FindAudioHandle(AssetId id) const noexcept {
    const SlotMeta* m = FindSlotByAssetId(id);
    return (m && m->kind == AssetKind::Audio && m->payloadSlot != 0U && m->state != AssetState::Failed)
               ? AudioHandle{m->payloadSlot, m->generation}
               : AudioHandle{};
}

void AssetBank::TouchAsset(AssetId id) noexcept {
    if (const u16* slotIdx = m_idToSlot.Find(id.value)) {
        if (*slotIdx > 0U && *slotIdx < m_highWaterSlot) {
            SlotMeta& m = m_slots[*slotIdx];
            if (m.state == AssetState::Ready) {
                m.lastUsedFrame = m_currentFrame;
            }
        }
    }
}

bool AssetBank::EnsurePayloadSlot(SlotMeta& meta) noexcept {
    if (meta.payloadSlot != 0U) return true;
    u16 payloadIdx = 0U;
    u16 nextGen = 1U;
    switch (meta.kind) {
        case AssetKind::Texture:
            if (m_freeTexHead != kNullAssetSlot) {
                payloadIdx = m_freeTexHead;
                m_freeTexHead = m_textures[payloadIdx].width;
                nextGen = NextGenerationNonZero(m_textures[payloadIdx].height);
            } else if (m_highWaterTex < m_maxTextures) {
                payloadIdx = m_highWaterTex++;
            } else {
                return false;
            }
            m_texGens[payloadIdx] = 0U;
            break;
        case AssetKind::Atlas:
            if (m_freeAtlasHead != kNullAssetSlot) {
                payloadIdx = m_freeAtlasHead;
                m_freeAtlasHead = m_atlases[payloadIdx].textureSlot;
                nextGen = NextGenerationNonZero(m_atlases[payloadIdx].firstSprite);
            } else if (m_highWaterAtlas < m_maxAtlases) {
                payloadIdx = m_highWaterAtlas++;
            } else {
                return false;
            }
            m_atlases[payloadIdx].generation = 0U;
            break;
        case AssetKind::Blob:
            if (m_freeBlobHead != kNullAssetSlot) {
                payloadIdx = m_freeBlobHead;
                m_freeBlobHead = static_cast<u16>(m_blobs[payloadIdx].size);
                nextGen = NextGenerationNonZero(m_blobs[payloadIdx].version);
            } else if (m_highWaterBlob < m_maxBlobs) {
                payloadIdx = m_highWaterBlob++;
            } else {
                return false;
            }
            m_blobs[payloadIdx].generation = 0U;
            break;
        case AssetKind::Audio:
            if (m_freeAudioHead != kNullAssetSlot) {
                payloadIdx = m_freeAudioHead;
                m_freeAudioHead = m_audio[payloadIdx].sampleRate;
                nextGen = NextGenerationNonZero(static_cast<u16>(m_audio[payloadIdx].frameCount));
            } else if (m_highWaterAudio < m_maxAudio) {
                payloadIdx = m_highWaterAudio++;
            } else {
                return false;
            }
            m_audioGens[payloadIdx] = 0U;
            break;
        default:
            return false;
    }
    meta.payloadSlot = payloadIdx;
    meta.generation = nextGen;
    return true;
}

u16 AssetBank::AcquireSlot(
    AssetId id,
    AssetKind kind,
    u8 scope,
    AssetResidency residency,
    bool& outNeedsLoad) noexcept {
    outNeedsLoad = false;
    if (!id.IsValid() || scope >= kMaxAssetScopes || m_slots == nullptr) return 0U;
    const u32 scopeBit = (1U << scope);
    if ((m_activeScopesMask & scopeBit) == 0U) return 0U;

    if (const u16* existing = m_idToSlot.Find(id.value)) {
        const u16 slotIdx = *existing;
        SlotMeta& meta = m_slots[slotIdx];
        meta.scopeMask |= scopeBit;

        if (meta.state == AssetState::Ready) {
            meta.lastUsedFrame = m_currentFrame;
            return slotIdx;
        }
        if (meta.state == AssetState::Loading) {
            if (residency != AssetResidency::Render && (meta.lastUsedFrame & scopeBit) == 0U) {
                meta.lastUsedFrame |= scopeBit;
                ++m_scopeSimPending[scope];
            }
            return slotIdx;
        }
        // Slot was previously Failed: ensure payloadSlot is reserved and transition back to Loading
        meta.kind = kind;
        if (!EnsurePayloadSlot(meta)) return 0U;
        meta.state = AssetState::Loading;
        meta.lastUsedFrame = (residency != AssetResidency::Render) ? scopeBit : 0U;
        if (residency != AssetResidency::Render) {
            ++m_scopeSimPending[scope];
        }
        outNeedsLoad = true;
        return slotIdx;
    }

    // Verify master slot availability before reserving a typed payload slot
    if (m_freeSlotHead == kNullAssetSlot && m_highWaterSlot >= m_maxSlots) {
        return 0U;
    }

    SlotMeta candidate{};
    candidate.scopeMask = scopeBit;
    candidate.lastUsedFrame = (residency != AssetResidency::Render) ? scopeBit : 0U;
    candidate.generation = 1U;
    candidate.version = 1U;
    candidate.payloadSlot = 0U;
    candidate.kind = kind;
    candidate.state = AssetState::Loading;

    if (kind != AssetKind::None && !EnsurePayloadSlot(candidate)) {
        return 0U;
    }

    u16 slotIdx = 0U;
    if (m_freeSlotHead != kNullAssetSlot) {
        slotIdx = m_freeSlotHead;
        m_freeSlotHead = m_slots[slotIdx].payloadSlot;
        if (candidate.payloadSlot == 0U) {
            candidate.generation = NextGenerationNonZero(m_slots[slotIdx].generation);
        }
    } else {
        slotIdx = m_highWaterSlot++;
    }

    m_slots[slotIdx] = candidate;
    if (residency != AssetResidency::Render) {
        ++m_scopeSimPending[scope];
    }

    m_slotAssetIds[slotIdx] = id.value;
    (void)m_idToSlot.InsertOrAssign(id.value, slotIdx);
    outNeedsLoad = true;
    return slotIdx;
}

void AssetBank::CompleteSlot(SlotMeta& meta, AssetState finalState) noexcept {
    if (meta.state == AssetState::Loading) {
        u32 pendingMask = meta.lastUsedFrame;
        while (pendingMask != 0U) {
            const u8 s = static_cast<u8>(std::countr_zero(pendingMask));
            pendingMask &= (pendingMask - 1U);
            if (m_scopeSimPending[s] > 0U) {
                --m_scopeSimPending[s];
            }
        }
    }
    meta.state = finalState;
    meta.lastUsedFrame = m_currentFrame;
}

void AssetBank::MarkFailed(u16 masterSlot) noexcept {
    if (masterSlot > 0U && masterSlot < m_highWaterSlot) {
        CompleteSlot(m_slots[masterSlot], AssetState::Failed);
    }
}

TextureAssetHandle AssetBank::CommitTexture(
    u16 masterSlot,
    u16 width,
    u16 height,
    std::span<const u8> rgbaPixels,
    TextureHandle gpu,
    bool zeroCopyPixels) noexcept {
    if (masterSlot == 0U || masterSlot >= m_highWaterSlot || width == 0U || height == 0U) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = AssetKind::Texture;
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const u8* storedPixels = nullptr;
    if (!rgbaPixels.empty()) {
        if (zeroCopyPixels) {
            storedPixels = rgbaPixels.data();
        } else {
            std::span<u8> copied = AllocatePayloadSlice(rgbaPixels.size());
            if (copied.empty()) {
                MarkFailed(masterSlot);
                return {};
            }
            std::memcpy(copied.data(), rgbaPixels.data(), rgbaPixels.size());
            storedPixels = copied.data();
        }
    }

    const u16 texIdx = meta.payloadSlot;
    TextureRecord& rec = m_textures[texIdx];
    rec.pixels = storedPixels;
    rec.gpu = gpu.IsValid() ? gpu : kWhiteTexture;
    rec.width = width;
    rec.height = height;
    m_texGens[texIdx] = meta.generation;

    if (rec.gpu == kWhiteTexture && rec.pixels != nullptr) {
        ++m_pendingGpuTexCount;
    } else if (rec.gpu != kWhiteTexture) {
        PropagateAtlasGpuTexture(texIdx, rec.gpu);
    }

    m_currentVramBytes += static_cast<u32>(width) * static_cast<u32>(height) * 4U;
    CompleteSlot(meta, AssetState::Ready);
    return TextureAssetHandle{texIdx, meta.generation};
}

AtlasHandle AssetBank::CommitAtlas(
    u16 masterSlot,
    TextureAssetHandle texHandle,
    std::span<const CookedAtlasSpriteEntry> entries) noexcept {
    if (masterSlot == 0U || masterSlot >= m_highWaterSlot ||
        m_highWaterSprite + entries.size() > m_maxSprites) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = AssetKind::Atlas;
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const TextureRecord& texRec = GetTexture(texHandle);
    const u16 firstSprite = m_highWaterSprite;
    for (const CookedAtlasSpriteEntry& e : entries) {
        const u16 spIdx = m_highWaterSprite++;
        m_sprites[spIdx] = SpriteFrame{
            Rect{e.u0, e.v0, e.u1 - e.u0, e.v1 - e.v0},
            e.width,
            e.height,
            e.pivotX,
            e.pivotY,
            texRec.gpu,
        };
        m_spriteGens[spIdx] = meta.generation;
        m_spriteNameHashes[spIdx] = e.nameHash;
    }

    const u16 atlasIdx = meta.payloadSlot;
    AtlasRecord& rec = m_atlases[atlasIdx];
    rec.firstSprite = firstSprite;
    rec.spriteCount = static_cast<u16>(entries.size());
    rec.textureSlot = texHandle.index;
    rec.generation = meta.generation;

    CompleteSlot(meta, AssetState::Ready);
    return AtlasHandle{atlasIdx, meta.generation};
}

AtlasHandle AssetBank::CommitGridAtlas(
    u16 masterSlot,
    TextureAssetHandle texHandle,
    u16 cols,
    u16 rows,
    Vec2 pivot) noexcept {
    const u32 totalFrames = static_cast<u32>(cols) * static_cast<u32>(rows);
    if (masterSlot == 0U || masterSlot >= m_highWaterSlot || cols == 0U || rows == 0U ||
        m_highWaterSprite + totalFrames > m_maxSprites) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = AssetKind::Atlas;
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const TextureRecord& texRec = GetTextureBySlot(texHandle.index);
    const f32 invCols = 1.0f / static_cast<f32>(cols);
    const f32 invRows = 1.0f / static_cast<f32>(rows);
    const f32 cellW = static_cast<f32>(texRec.width > 0U ? texRec.width : cols) * invCols;
    const f32 cellH = static_cast<f32>(texRec.height > 0U ? texRec.height : rows) * invRows;
    const UnitPivotU16 px{pivot.x};
    const UnitPivotU16 py{pivot.y};

    const u16 firstSprite = m_highWaterSprite;
    for (u16 r = 0U; r < rows; ++r) {
        for (u16 c = 0U; c < cols; ++c) {
            const u16 spIdx = m_highWaterSprite++;
            m_sprites[spIdx] = SpriteFrame{
                Rect{static_cast<f32>(c) * invCols, static_cast<f32>(r) * invRows, invCols, invRows},
                cellW,
                cellH,
                px,
                py,
                texRec.gpu,
            };
            m_spriteGens[spIdx] = meta.generation;
            m_spriteNameHashes[spIdx] = 0U;
        }
    }

    const u16 atlasIdx = meta.payloadSlot;
    AtlasRecord& rec = m_atlases[atlasIdx];
    rec.firstSprite = firstSprite;
    rec.spriteCount = static_cast<u16>(totalFrames);
    rec.textureSlot = texHandle.index;
    rec.generation = meta.generation;

    CompleteSlot(meta, AssetState::Ready);
    return AtlasHandle{atlasIdx, meta.generation};
}

BlobHandle AssetBank::CommitBlob(
    u16 masterSlot,
    std::span<const u8> payload,
    bool zeroCopyPayload) noexcept {
    if (masterSlot == 0U || masterSlot >= m_highWaterSlot) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = AssetKind::Blob;
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const u8* storedPtr = &kFallbackBlobByte;
    if (!payload.empty()) {
        if (zeroCopyPayload) {
            storedPtr = payload.data();
        } else {
            std::span<u8> copied = AllocatePayloadSlice(payload.size());
            if (copied.empty()) {
                MarkFailed(masterSlot);
                return {};
            }
            std::memcpy(copied.data(), payload.data(), payload.size());
            storedPtr = copied.data();
        }
    }

    const u16 blobIdx = meta.payloadSlot;
    BlobRecord& rec = m_blobs[blobIdx];
    rec.data = storedPtr;
    rec.size = static_cast<u32>(payload.size());
    rec.generation = meta.generation;
    rec.version = meta.version;

    CompleteSlot(meta, AssetState::Ready);
    return BlobHandle{blobIdx, meta.generation};
}

bool AssetBank::HotReloadBlob(AssetId id, std::span<const u8> newPayload) noexcept {
    const u16* slotIdx = m_idToSlot.Find(id.value);
    if (!slotIdx || *slotIdx == 0U || *slotIdx >= m_highWaterSlot) return false;
    SlotMeta& meta = m_slots[*slotIdx];
    if (meta.kind != AssetKind::Blob || meta.payloadSlot == 0U || meta.payloadSlot >= m_highWaterBlob) {
        return false;
    }
    BlobRecord& rec = m_blobs[meta.payloadSlot];
    const u8* oldData = rec.data;
    const u32 oldSize = rec.size;

    if (!newPayload.empty()) {
        // Reclaim previous arena slice first so equal/smaller hot-reloads reuse memory in-place!
        if (oldData != nullptr && oldData != &kFallbackBlobByte && oldSize > 0U) {
            FreePayloadSlice(oldData, oldSize);
        }
        std::span<u8> copied = AllocatePayloadSlice(newPayload.size());
        if (copied.empty()) return false;
        std::memcpy(copied.data(), newPayload.data(), newPayload.size());
        rec.data = copied.data();
    } else {
        if (oldData != nullptr && oldData != &kFallbackBlobByte && oldSize > 0U) {
            FreePayloadSlice(oldData, oldSize);
        }
        rec.data = &kFallbackBlobByte;
    }
    rec.size = static_cast<u32>(newPayload.size());
    ++meta.version;
    rec.version = meta.version;
    meta.lastUsedFrame = m_currentFrame;
    ++m_hotReloadCount;
    return true;
}

AudioHandle AssetBank::CommitAudio(
    u16 masterSlot,
    std::span<const f32> pcmSamples,
    u32 sampleRate,
    u8 channels,
    bool looping) noexcept {
    if (masterSlot == 0U || masterSlot >= m_highWaterSlot || channels == 0U) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = AssetKind::Audio;
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const f32* storedPcm = kFallbackSilenceSample;
    if (!pcmSamples.empty()) {
        std::span<u8> rawSlice = AllocatePayloadSlice(pcmSamples.size_bytes());
        if (rawSlice.empty()) {
            MarkFailed(masterSlot);
            return {};
        }
        std::memcpy(rawSlice.data(), pcmSamples.data(), pcmSamples.size_bytes());
        storedPcm = reinterpret_cast<const f32*>(rawSlice.data());
    }

    const u16 audIdx = meta.payloadSlot;
    AudioRecord& rec = m_audio[audIdx];
    rec.pcm = storedPcm;
    rec.frameCount = static_cast<u32>(pcmSamples.size() / channels);
    rec.sampleRate = static_cast<u16>(std::clamp(sampleRate, 8000U, 65535U));
    rec.channels = channels;
    rec.looping = looping;
    m_audioGens[audIdx] = meta.generation;

    CompleteSlot(meta, AssetState::Ready);
    return AudioHandle{audIdx, meta.generation};
}

void AssetBank::EvictSlotInternal(
    u16 slotIdx,
    std::span<TextureHandle> outDestroyedGpuTextures,
    u32& ioGpuDestroyCount) noexcept {
    SlotMeta& meta = m_slots[slotIdx];
    const u16 payloadIdx = meta.payloadSlot;

    switch (meta.kind) {
        case AssetKind::Texture:
            if (payloadIdx > 0U && payloadIdx < m_highWaterTex) {
                TextureRecord& tex = m_textures[payloadIdx];
                const u32 bytes = static_cast<u32>(tex.width) * static_cast<u32>(tex.height) * 4U;
                if (meta.state == AssetState::Ready) {
                    m_currentVramBytes = (m_currentVramBytes >= bytes) ? (m_currentVramBytes - bytes) : 0U;
                }
                if (tex.gpu == kWhiteTexture && tex.pixels != nullptr && m_pendingGpuTexCount > 0U) {
                    --m_pendingGpuTexCount;
                }
                if (tex.gpu.IsValid() && tex.gpu != kWhiteTexture &&
                    ioGpuDestroyCount < outDestroyedGpuTextures.size()) {
                    outDestroyedGpuTextures[ioGpuDestroyCount++] = tex.gpu;
                }
                if (tex.pixels != nullptr && tex.pixels != kFallbackWhitePixel && bytes > 0U) {
                    FreePayloadSlice(tex.pixels, bytes);
                }
                m_texGens[payloadIdx] = 0U;
                tex.pixels = nullptr;
                tex.gpu = kWhiteTexture;
                tex.height = meta.generation;
                tex.width = m_freeTexHead;
                m_freeTexHead = payloadIdx;
            }
            break;
        case AssetKind::Atlas:
            if (payloadIdx > 0U && payloadIdx < m_highWaterAtlas) {
                AtlasRecord& ar = m_atlases[payloadIdx];
                for (u16 s = 0U; s < ar.spriteCount; ++s) {
                    const u16 spIdx = static_cast<u16>(ar.firstSprite + s);
                    if (spIdx < m_highWaterSprite) {
                        m_spriteGens[spIdx] = 0U;
                        m_spriteNameHashes[spIdx] = 0U;
                    }
                }
                // Rewind m_highWaterSprite across any trailing evicted sprite ranges
                while (m_highWaterSprite > 1U && m_spriteGens[m_highWaterSprite - 1U] == 0U) {
                    --m_highWaterSprite;
                }
                ar.generation = 0U;
                ar.spriteCount = 0U;
                ar.firstSprite = meta.generation;
                ar.textureSlot = m_freeAtlasHead;
                m_freeAtlasHead = payloadIdx;
            }
            break;
        case AssetKind::Blob:
            if (payloadIdx > 0U && payloadIdx < m_highWaterBlob) {
                BlobRecord& br = m_blobs[payloadIdx];
                if (br.data != nullptr && br.data != &kFallbackBlobByte && br.size > 0U) {
                    FreePayloadSlice(br.data, br.size);
                }
                br.data = nullptr;
                br.generation = 0U;
                br.version = meta.generation;
                br.size = m_freeBlobHead;
                m_freeBlobHead = payloadIdx;
            }
            break;
        case AssetKind::Audio:
            if (payloadIdx > 0U && payloadIdx < m_highWaterAudio) {
                AudioRecord& au = m_audio[payloadIdx];
                const usize pcmBytes = static_cast<usize>(au.TotalSamples()) * sizeof(f32);
                if (au.pcm != nullptr && au.pcm != kFallbackSilenceSample && pcmBytes > 0U) {
                    FreePayloadSlice(au.pcm, pcmBytes);
                }
                m_audioGens[payloadIdx] = 0U;
                au.pcm = nullptr;
                au.frameCount = meta.generation;
                au.sampleRate = m_freeAudioHead;
                m_freeAudioHead = payloadIdx;
            }
            break;
        default:
            break;
    }

    if (m_slotAssetIds[slotIdx] != 0ULL) {
        (void)m_idToSlot.Erase(m_slotAssetIds[slotIdx]);
        m_slotAssetIds[slotIdx] = 0ULL;
    }

    meta.scopeMask = 0U;
    meta.lastUsedFrame = 0U;
    meta.state = AssetState::Unloaded;
    meta.payloadSlot = m_freeSlotHead;
    m_freeSlotHead = slotIdx;
    ++m_evictedCount;
}

u32 AssetBank::EvictUnpinned(std::span<TextureHandle> outDestroyedGpuTextures) noexcept {
    u32 evicted = 0U;
    u32 gpuDestroyed = 0U;
    for (u16 i = 1U; i < m_highWaterSlot; ++i) {
        const SlotMeta& meta = m_slots[i];
        if ((meta.state == AssetState::Ready || meta.state == AssetState::Failed) &&
            (meta.scopeMask & m_activeScopesMask) == 0U) {
            EvictSlotInternal(i, outDestroyedGpuTextures, gpuDestroyed);
            ++evicted;
        }
    }
    return evicted;
}

u32 AssetBank::EvictLruToVramBudget(
    std::span<TextureHandle> outDestroyedGpuTextures,
    u32 targetVramBytes) noexcept {
    const u32 goalBytes = (targetVramBytes == 0xFFFFFFFFU) ? m_vramBudgetBytes : targetVramBytes;
    u32 gpuDestroyed = 0U;
    u32 evicted = 0U;

    while (m_currentVramBytes > goalBytes) {
        u16 oldestSlot = 0U;
        u32 oldestFrame = 0xFFFFFFFFU;
        for (u16 i = 1U; i < m_highWaterSlot; ++i) {
            const SlotMeta& meta = m_slots[i];
            if (meta.state == AssetState::Ready &&
                meta.kind == AssetKind::Texture &&
                (meta.scopeMask & m_activeScopesMask) == 0U &&
                meta.lastUsedFrame <= oldestFrame) {
                oldestFrame = meta.lastUsedFrame;
                oldestSlot = i;
            }
        }
        if (oldestSlot == 0U) break;
        EvictSlotInternal(oldestSlot, outDestroyedGpuTextures, gpuDestroyed);
        ++evicted;
    }
    return evicted;
}

Result<usize, AssetErrc> AssetBank::DecompressLz4(
    std::span<const u8> src,
    std::span<u8> dst) noexcept {
    usize sp = 0U;
    usize dp = 0U;
    while (sp < src.size()) {
        const u8 token = src[sp++];
        usize litLen = token >> 4U;
        if (litLen == 15U) {
            for (;;) {
                if (sp >= src.size()) return Err(AssetErrc::DecompressionFailed);
                const u8 s = src[sp++];
                litLen += s;
                if (s != 255U) break;
            }
        }
        if (sp + litLen > src.size() || dp + litLen > dst.size()) {
            return Err(AssetErrc::DecompressionFailed);
        }
        if (litLen > 0U) {
            std::memcpy(dst.data() + dp, src.data() + sp, litLen);
            sp += litLen;
            dp += litLen;
        }
        if (sp == src.size()) break;

        if (sp + 2U > src.size()) return Err(AssetErrc::DecompressionFailed);
        const u16 offset = static_cast<u16>(src[sp]) | (static_cast<u16>(src[sp + 1U]) << 8U);
        sp += 2U;
        if (offset == 0U || offset > dp) return Err(AssetErrc::DecompressionFailed);

        usize matchLen = (token & 0x0FU) + 4U;
        if ((token & 0x0FU) == 15U) {
            for (;;) {
                if (sp >= src.size()) return Err(AssetErrc::DecompressionFailed);
                const u8 s = src[sp++];
                matchLen += s;
                if (s != 255U) break;
            }
        }
        if (dp + matchLen > dst.size()) return Err(AssetErrc::DecompressionFailed);
        // Byte-by-byte copy intentionally supports overlapping RLE matches where offset < matchLen
        for (usize i = 0U; i < matchLen; ++i) {
            dst[dp + i] = dst[dp - offset + i];
        }
        dp += matchLen;
    }
    return dp;
}

Result<usize, AssetErrc> AssetBank::CompressLz4(
    std::span<const u8> src,
    std::span<u8> dst) noexcept {
    const usize n = src.size();
    if (n == 0U) return 0U;

    std::array<u16, 256> hashTable{};
    hashTable.fill(0xFFFFU);

    usize anchor = 0U;
    usize ip = 0U;
    usize dp = 0U;

    const auto emitLength = [&](usize len) noexcept -> bool {
        while (len >= 255U) {
            if (dp >= dst.size()) return false;
            dst[dp++] = 255U;
            len -= 255U;
        }
        if (dp >= dst.size()) return false;
        dst[dp++] = static_cast<u8>(len);
        return true;
    };

    if (n >= 12U && n <= 65535U) {
        const usize matchLimit = n - 5U;
        while (ip + 4U <= matchLimit) {
            const u32 seq = ReadLeU32(src.data() + ip);
            const u8 h = static_cast<u8>((seq * 2654435761U) >> 24U);
            const u16 ref = hashTable[h];
            hashTable[h] = static_cast<u16>(ip);

            if (ref != 0xFFFFU && ip > ref && (ip - ref) <= 65535U &&
                ReadLeU32(src.data() + ref) == seq) {
                const usize litLen = ip - anchor;
                usize matchLen = 4U;
                while (ip + matchLen < matchLimit && src[ref + matchLen] == src[ip + matchLen]) {
                    ++matchLen;
                }

                const u8 litNib = static_cast<u8>(std::min<usize>(litLen, 15U));
                const u8 matNib = static_cast<u8>(std::min<usize>(matchLen - 4U, 15U));
                if (dp >= dst.size()) return Err(AssetErrc::OutOfMemory);
                dst[dp++] = static_cast<u8>((litNib << 4U) | matNib);

                if (litLen >= 15U && !emitLength(litLen - 15U)) return Err(AssetErrc::OutOfMemory);
                if (dp + litLen + 2U > dst.size()) return Err(AssetErrc::OutOfMemory);
                if (litLen > 0U) {
                    std::memcpy(dst.data() + dp, src.data() + anchor, litLen);
                    dp += litLen;
                }

                const u16 offset = static_cast<u16>(ip - ref);
                dst[dp++] = static_cast<u8>(offset & 0xFFU);
                dst[dp++] = static_cast<u8>((offset >> 8U) & 0xFFU);

                if (matchLen - 4U >= 15U && !emitLength(matchLen - 19U)) {
                    return Err(AssetErrc::OutOfMemory);
                }

                ip += matchLen;
                anchor = ip;
                continue;
            }
            ++ip;
        }
    }

    // Emit trailing literals
    const usize lastLit = n - anchor;
    const u8 litNib = static_cast<u8>(std::min<usize>(lastLit, 15U));
    if (dp >= dst.size()) return Err(AssetErrc::OutOfMemory);
    dst[dp++] = static_cast<u8>(litNib << 4U);
    if (lastLit >= 15U && !emitLength(lastLit - 15U)) return Err(AssetErrc::OutOfMemory);
    if (dp + lastLit > dst.size()) return Err(AssetErrc::OutOfMemory);
    if (lastLit > 0U) {
        std::memcpy(dst.data() + dp, src.data() + anchor, lastLit);
        dp += lastLit;
    }
    return dp;
}

u64 AssetBank::ComputeAuditHash() const noexcept {
    const u64 hdr = static_cast<u64>(m_highWaterSlot) |
                    (static_cast<u64>(m_highWaterSprite) << 16U) |
                    (static_cast<u64>(m_highWaterBlob) << 32U) |
                    (static_cast<u64>(m_activeScopesMask) << 48U);
    u64 h = Mix64(hdr);
    for (u16 i = 1U; i < m_highWaterSlot; ++i) {
        const SlotMeta& m = m_slots[i];
        const u64 packedMeta = static_cast<u64>(m.scopeMask) |
                               (static_cast<u64>(m.generation) << 32U) |
                               (static_cast<u64>(m.version) << 48U) |
                               (static_cast<u64>(m.state) << 60U);
        h = HashCombine64(h, packedMeta);
    }
    return h;
}

} // namespace en
