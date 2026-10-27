#include "assets/AssetBank.h"
#include "assets/ImageLoader.h"

#include <cstring>

namespace en {

namespace {

constexpr u8 kFallbackWhitePixel[4]{255U, 255U, 255U, 255U};
constexpr u8 kFallbackBlobByte{0U};
constexpr f32 kFallbackSilenceSample[2]{0.0f, 0.0f};

[[nodiscard]] constexpr u16 NextGenerationNonZero(u16 gen) noexcept {
    const u16 next = static_cast<u16>(gen + 1U);
    return (next == 0U) ? 1U : next;
}

template <typename RecordT, typename LinkMemberT>
[[nodiscard]] bool AllocFromPool(
    AssetBank::PoolState& pool,
    RecordT* table,
    LinkMemberT RecordT::*linkField,
    u16& outIdx) noexcept {
    if (pool.freeHead != 0U) {
        outIdx = pool.freeHead;
        pool.freeHead = static_cast<u16>(table[outIdx].*linkField);
        return true;
    }
    if (pool.highWater < pool.max) {
        outIdx = pool.highWater++;
        return true;
    }
    return false;
}

template <typename RecordT, typename LinkMemberT>
void FreeToPool(
    AssetBank::PoolState& pool,
    RecordT& rec,
    LinkMemberT RecordT::*linkField,
    u16 idx) noexcept {
    rec = RecordT{};
    rec.*linkField = static_cast<LinkMemberT>(pool.freeHead);
    pool.freeHead = idx;
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

    // m_slots is carved first at offset 0 of m_arena, serving as the arena base address reference.
    auto slots = m_arena.Carve<SlotMeta>(cfg.maxSlots);
    auto sprites = m_arena.Carve<SpriteFrame>(cfg.maxSprites);
    auto atlases = m_arena.Carve<AtlasRecord>(cfg.maxAtlases);
    auto textures = m_arena.Carve<TextureRecord>(cfg.maxTextures);
    auto blobs = m_arena.Carve<BlobRecord>(cfg.maxBlobs);
    auto audio = m_arena.Carve<AudioRecord>(cfg.maxAudio);

    if (slots.empty() || sprites.empty() || atlases.empty() ||
        textures.empty() || blobs.empty() || audio.empty() ||
        !m_idToSlot.Init(m_arena, static_cast<u32>(cfg.maxSlots) * 2U)) {
        Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }

    m_slots = slots.data();
    m_sprites = sprites.data();
    m_atlases = atlases.data();
    m_textures = textures.data();
    m_blobs = blobs.data();
    m_audio = audio.data();

    m_vramBudgetBytes = cfg.vramBudgetBytes;
    m_slotPool.Reset(cfg.maxSlots);
    m_payloadPools[0].Reset(cfg.maxTextures);
    m_payloadPools[1].Reset(cfg.maxAtlases);
    m_payloadPools[2].Reset(cfg.maxBlobs);
    m_payloadPools[3].Reset(cfg.maxAudio);
    m_maxSprites = cfg.maxSprites;
    m_highWaterSprite = 1U;

    m_slots[0] = SlotMeta{
        .assetId = 0ULL,
        .scopeMask = 0xFFFFU,
        .lastUsedFrame = 1U,
        .generation = 1U,
        .payloadSlot = 0U,
        .kind = 0U,
    };
    m_sprites[0] = SpriteFrame{.generation = 1U};
    m_textures[0] = TextureRecord{
        .pixels = kFallbackWhitePixel,
        .gpu = kWhiteTexture,
        .width = 1U,
        .height = 1U,
    };
    m_atlases[0] = AtlasRecord{
        .firstSprite = 0U,
        .spriteCount = 1U,
        .texMasterSlot = 0U,
    };
    m_blobs[0] = BlobRecord{
        .data = &kFallbackBlobByte,
        .size = 0U,
    };
    m_audio[0] = AudioRecord{
        .pcm = kFallbackSilenceSample,
        .frameCount = 0U,
        .sampleRate = 48000U,
        .channels = 2U,
        .looping = false,
    };

    m_payloadBaseMarker = m_arena.SaveMarker();
    return {};
}

void AssetBank::Shutdown() noexcept {
    m_arena.Shutdown();
    m_slots = nullptr;
    m_sprites = nullptr;
    m_atlases = nullptr;
    m_textures = nullptr;
    m_blobs = nullptr;
    m_audio = nullptr;
    m_freePayloadHead = nullptr;
    m_currentVramBytes = 0U;
    m_payloadBaseMarker = 0U;
    m_evictedCount = 0U;
    m_slotPool = {};
    m_payloadPools = {};
    m_maxSprites = 0U;
    m_highWaterSprite = 0U;
    m_activeScopesMask = 0U;
    m_currentFrame = 1U;
}

Result<u8, AssetErrc> AssetBank::BeginScope() noexcept {
    if (m_activeScopesMask == 0xFFFFU) {
        return Err(AssetErrc::ScopeExhausted);
    }
    const u8 s = static_cast<u8>(std::countr_one(m_activeScopesMask));
    m_activeScopesMask |= static_cast<u16>(1U << s);
    return s;
}

void AssetBank::EndScope(u8 scope) noexcept {
    if (scope >= kMaxAssetScopes) return;
    const u16 bit = static_cast<u16>(1U << scope);
    if ((m_activeScopesMask & bit) == 0U) return;
    m_activeScopesMask &= static_cast<u16>(~bit);
    for (u16 i = 1U; i < m_slotPool.highWater; ++i) {
        m_slots[i].scopeMask &= static_cast<u16>(~bit);
    }
}

std::span<u8> AssetBank::AllocatePayloadSlice(usize bytes) noexcept {
    if (bytes == 0U || m_slots == nullptr) return {};
    const u32 alignedBytes = static_cast<u32>(AlignUp(bytes, AlignedArena::kCacheLineBytes));
    if (alignedBytes == 0U) return {};

    FreePayloadBlock* prev = nullptr;
    for (FreePayloadBlock* curr = m_freePayloadHead; curr != nullptr; prev = curr, curr = curr->next) {
        if (curr->byteSize < alignedBytes) continue;
        const u32 rem = curr->byteSize - alignedBytes;
        u8* rawPtr = nullptr;
        if (rem >= AlignedArena::kCacheLineBytes) {
            curr->byteSize = rem;
            rawPtr = reinterpret_cast<u8*>(curr) + rem;
        } else {
            if (prev != nullptr) prev->next = curr->next;
            else m_freePayloadHead = curr->next;
            rawPtr = reinterpret_cast<u8*>(curr);
        }
        return {rawPtr, bytes};
    }

    return m_arena.Carve<u8>(bytes);
}

void AssetBank::FreePayloadSlice(const void* ptr, usize bytes) noexcept {
    if (ptr == nullptr || bytes == 0U || m_slots == nullptr) return;
    const auto* basePtr = reinterpret_cast<const u8*>(m_slots);
    const auto* bytePtr = static_cast<const u8*>(ptr);
    if (bytePtr < basePtr) return;

    const usize offset = static_cast<usize>(bytePtr - basePtr);
    const u32 alignedBytes = static_cast<u32>(AlignUp(bytes, AlignedArena::kCacheLineBytes));
    if (offset < m_payloadBaseMarker || offset + alignedBytes > m_arena.UsedBytes()) return;

    u8* mutablePtr = const_cast<u8*>(bytePtr);
    auto* blk = reinterpret_cast<FreePayloadBlock*>(mutablePtr);
    blk->byteSize = alignedBytes;

    FreePayloadBlock* prev = nullptr;
    FreePayloadBlock* curr = m_freePayloadHead;
    while (curr != nullptr && reinterpret_cast<const u8*>(curr) < mutablePtr) {
        prev = curr;
        curr = curr->next;
    }

    blk->next = curr;
    if (prev != nullptr) prev->next = blk;
    else m_freePayloadHead = blk;

    if (curr != nullptr && (mutablePtr + blk->byteSize) == reinterpret_cast<u8*>(curr)) {
        blk->byteSize += curr->byteSize;
        blk->next = curr->next;
    }
    if (prev != nullptr && (reinterpret_cast<u8*>(prev) + prev->byteSize) == mutablePtr) {
        prev->byteSize += blk->byteSize;
        prev->next = blk->next;
        blk = prev;
    }

    const usize blkOff = static_cast<usize>(reinterpret_cast<const u8*>(blk) - basePtr);
    if (blkOff + blk->byteSize == m_arena.UsedBytes()) {
        if (m_freePayloadHead == blk) {
            m_freePayloadHead = nullptr;
        } else {
            FreePayloadBlock* p = m_freePayloadHead;
            while (p != nullptr && p->next != blk) p = p->next;
            if (p != nullptr) p->next = nullptr;
        }
        m_arena.RollbackToMarker(static_cast<u32>(blkOff));
    }
}

void AssetBank::PropagateAtlasGpuTexture(u16 texMasterSlot, TextureHandle gpu) noexcept {
    for (u16 a = 1U; a < m_payloadPools[1].highWater; ++a) {
        const AtlasRecord& ar = m_atlases[a];
        if (ar.spriteCount > 0U && ar.texMasterSlot == texMasterSlot) {
            for (u16 s = 0U; s < ar.spriteCount; ++s) {
                const u16 spIdx = static_cast<u16>(ar.firstSprite + s);
                if (spIdx < m_highWaterSprite) {
                    m_sprites[spIdx].gpuTexture = gpu;
                }
            }
        }
    }
}

void AssetBank::BindGpuTexture(TextureAssetHandle texHandle, TextureHandle gpu) noexcept {
    if (!gpu.IsValid()) return;
    const u16 texIdx = ResolvePayloadSlot(texHandle, AssetKind::Texture);
    if (texIdx == 0U) return;
    m_textures[texIdx].gpu = gpu;
    PropagateAtlasGpuTexture(texHandle.index, gpu);
}

void AssetBank::TouchAsset(AssetId id) noexcept {
    if (const u16* slotIdx = m_idToSlot.Find(id.value)) {
        if (*slotIdx > 0U && *slotIdx < m_slotPool.highWater) {
            m_slots[*slotIdx].lastUsedFrame = m_currentFrame;
        }
    }
}

void AssetBank::TouchAtlas(AtlasHandle h) noexcept {
    const u16 aIdx = ResolvePayloadSlot(h, AssetKind::Atlas);
    if (aIdx == 0U) return;
    m_slots[h.index].lastUsedFrame = m_currentFrame;
    const u16 texMaster = m_atlases[aIdx].texMasterSlot;
    if (texMaster > 0U && texMaster < m_slotPool.highWater) {
        m_slots[texMaster].lastUsedFrame = m_currentFrame;
    }
}

bool AssetBank::EnsurePayloadSlot(SlotMeta& meta) noexcept {
    if (meta.payloadSlot != 0U) return true;
    u16 payloadIdx = 0U;
    bool ok = false;
    switch (meta.Kind()) {
        case AssetKind::Texture:
            ok = AllocFromPool(m_payloadPools[0], m_textures, &TextureRecord::width, payloadIdx);
            break;
        case AssetKind::Atlas:
            ok = AllocFromPool(m_payloadPools[1], m_atlases, &AtlasRecord::firstSprite, payloadIdx);
            break;
        case AssetKind::Blob:
            ok = AllocFromPool(m_payloadPools[2], m_blobs, &BlobRecord::size, payloadIdx);
            break;
        case AssetKind::Audio:
            ok = AllocFromPool(m_payloadPools[3], m_audio, &AudioRecord::sampleRate, payloadIdx);
            break;
        default:
            return false;
    }
    if (!ok) return false;
    meta.payloadSlot = payloadIdx;
    return true;
}

u16 AssetBank::AcquireSlot(
    AssetId id,
    AssetKind kind,
    u8 scope,
    bool& outNeedsLoad) noexcept {
    outNeedsLoad = false;
    if (!id.IsValid() || scope >= kMaxAssetScopes || m_slots == nullptr) return 0U;
    const u16 scopeBit = static_cast<u16>(1U << scope);
    if ((m_activeScopesMask & scopeBit) == 0U) return 0U;

    if (const u16* existing = m_idToSlot.Find(id.value)) {
        const u16 slotIdx = *existing;
        SlotMeta& meta = m_slots[slotIdx];
        if (meta.Kind() != AssetKind::None && kind != AssetKind::None && meta.Kind() != kind) {
            return 0U;
        }
        meta.scopeMask |= scopeBit;
        meta.lastUsedFrame = m_currentFrame;
        return slotIdx;
    }

    if (m_slotPool.freeHead == 0U && m_slotPool.highWater >= m_slotPool.max) {
        return 0U;
    }

    u16 slotIdx = m_slotPool.freeHead;
    u16 retiredGen = 0U;
    const bool fromFreeList = (slotIdx != 0U);
    if (fromFreeList) {
        retiredGen = m_slots[slotIdx].generation;
        m_slotPool.freeHead = m_slots[slotIdx].payloadSlot;
    } else {
        slotIdx = m_slotPool.highWater++;
    }

    if (!m_idToSlot.InsertOrAssign(id.value, slotIdx)) {
        if (fromFreeList) {
            m_slots[slotIdx].payloadSlot = m_slotPool.freeHead;
            m_slotPool.freeHead = slotIdx;
        } else {
            --m_slotPool.highWater;
        }
        return 0U;
    }

    SlotMeta candidate{
        .assetId = id.value,
        .scopeMask = scopeBit,
        .lastUsedFrame = m_currentFrame,
        .generation = NextGenerationNonZero(retiredGen),
        .payloadSlot = 0U,
        .kind = static_cast<u16>(kind),
    };

    if (kind != AssetKind::None && !EnsurePayloadSlot(candidate)) {
        (void)m_idToSlot.Erase(id.value);
        if (fromFreeList) {
            m_slots[slotIdx].payloadSlot = m_slotPool.freeHead;
            m_slotPool.freeHead = slotIdx;
        } else {
            --m_slotPool.highWater;
        }
        return 0U;
    }

    m_slots[slotIdx] = candidate;
    outNeedsLoad = true;
    return slotIdx;
}

void AssetBank::MarkFailed(u16 masterSlot) noexcept {
    if (masterSlot > 0U && masterSlot < m_slotPool.highWater && m_slots[masterSlot].assetId != 0ULL) {
        u32 dummy = 0U;
        EvictSlotInternal(masterSlot, {}, dummy);
        if (m_evictedCount > 0U) --m_evictedCount;
    }
}

TextureAssetHandle AssetBank::CommitTexture(
    u16 masterSlot,
    u16 width,
    u16 height,
    std::span<const u8> rgbaPixels,
    TextureHandle gpu) noexcept {
    const u64 expectedBytes64 = static_cast<u64>(width) * static_cast<u64>(height) * 4ULL;
    if (masterSlot == 0U || masterSlot >= m_slotPool.highWater || width == 0U || height == 0U ||
        width > 16384U || height > 16384U || expectedBytes64 > 0x7FFFFFFFULL ||
        (!rgbaPixels.empty() && rgbaPixels.size() != static_cast<usize>(expectedBytes64))) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    if (meta.Kind() != AssetKind::None && meta.Kind() != AssetKind::Texture) {
        MarkFailed(masterSlot);
        return {};
    }
    meta.kind = static_cast<u16>(AssetKind::Texture);
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const u8* storedPixels = nullptr;
    if (!rgbaPixels.empty()) {
        std::span<u8> copied = AllocatePayloadSlice(rgbaPixels.size());
        if (copied.empty()) {
            MarkFailed(masterSlot);
            return {};
        }
        std::memcpy(copied.data(), rgbaPixels.data(), rgbaPixels.size());
        storedPixels = copied.data();
    }

    const u16 texIdx = meta.payloadSlot;
    const TextureHandle resolvedGpu = gpu.IsValid() ? gpu : kWhiteTexture;
    m_textures[texIdx] = TextureRecord{
        .pixels = storedPixels,
        .gpu = resolvedGpu,
        .width = width,
        .height = height,
    };

    if (resolvedGpu != kWhiteTexture) {
        PropagateAtlasGpuTexture(masterSlot, resolvedGpu);
    }

    const u64 nextVram = static_cast<u64>(m_currentVramBytes) + expectedBytes64;
    m_currentVramBytes = static_cast<u32>(std::min<u64>(nextVram, 0xFFFFFFFFULL));
    meta.lastUsedFrame = m_currentFrame;
    return TextureAssetHandle{masterSlot, meta.generation};
}

AtlasHandle AssetBank::CommitAtlas(
    u16 masterSlot,
    TextureAssetHandle texHandle,
    std::span<const SpriteFrame> frames) noexcept {
    if (masterSlot == 0U || masterSlot >= m_slotPool.highWater ||
        m_highWaterSprite + frames.size() > m_maxSprites) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = static_cast<u16>(AssetKind::Atlas);
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const u16 texIdx = ResolvePayloadSlot(texHandle, AssetKind::Texture);
    const u16 resolvedTexMaster = (texIdx != 0U) ? texHandle.index : 0U;
    const TextureRecord& texRec = m_textures[texIdx];
    const u16 firstSprite = m_highWaterSprite;
    for (const SpriteFrame& f : frames) {
        m_sprites[m_highWaterSprite++] = SpriteFrame{
            .uv = f.uv,
            .width = f.width,
            .height = f.height,
            .pivot = f.pivot,
            .gpuTexture = texRec.gpu,
            .generation = meta.generation,
        };
    }

    m_atlases[meta.payloadSlot] = AtlasRecord{
        .firstSprite = firstSprite,
        .spriteCount = static_cast<u16>(frames.size()),
        .texMasterSlot = resolvedTexMaster,
    };

    meta.lastUsedFrame = m_currentFrame;
    return AtlasHandle{masterSlot, meta.generation};
}

AtlasHandle AssetBank::CommitGridAtlas(
    u16 masterSlot,
    TextureAssetHandle texHandle,
    u16 cols,
    u16 rows,
    Vec2 pivot) noexcept {
    const u32 totalFrames = static_cast<u32>(cols) * static_cast<u32>(rows);
    if (masterSlot == 0U || masterSlot >= m_slotPool.highWater || cols == 0U || rows == 0U ||
        m_highWaterSprite + totalFrames > m_maxSprites) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = static_cast<u16>(AssetKind::Atlas);
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const u16 texIdx = ResolvePayloadSlot(texHandle, AssetKind::Texture);
    const u16 resolvedTexMaster = (texIdx != 0U) ? texHandle.index : 0U;
    const TextureRecord& texRec = m_textures[texIdx];
    const f32 invCols = 1.0f / static_cast<f32>(cols);
    const f32 invRows = 1.0f / static_cast<f32>(rows);
    const f32 cellW = static_cast<f32>(texRec.width > 0U ? texRec.width : cols) * invCols;
    const f32 cellH = static_cast<f32>(texRec.height > 0U ? texRec.height : rows) * invRows;

    const u16 firstSprite = m_highWaterSprite;
    for (u16 r = 0U; r < rows; ++r) {
        for (u16 c = 0U; c < cols; ++c) {
            m_sprites[m_highWaterSprite++] = SpriteFrame{
                .uv = Rect{static_cast<f32>(c) * invCols, static_cast<f32>(r) * invRows, invCols, invRows},
                .width = cellW,
                .height = cellH,
                .pivot = pivot,
                .gpuTexture = texRec.gpu,
                .generation = meta.generation,
            };
        }
    }

    m_atlases[meta.payloadSlot] = AtlasRecord{
        .firstSprite = firstSprite,
        .spriteCount = static_cast<u16>(totalFrames),
        .texMasterSlot = resolvedTexMaster,
    };

    meta.lastUsedFrame = m_currentFrame;
    return AtlasHandle{masterSlot, meta.generation};
}

BlobHandle AssetBank::CommitBlob(
    u16 masterSlot,
    std::span<const u8> payload) noexcept {
    if (masterSlot == 0U || masterSlot >= m_slotPool.highWater) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = static_cast<u16>(AssetKind::Blob);
    if (!EnsurePayloadSlot(meta)) {
        MarkFailed(masterSlot);
        return {};
    }

    const u8* storedPtr = &kFallbackBlobByte;
    if (!payload.empty()) {
        std::span<u8> copied = AllocatePayloadSlice(payload.size());
        if (copied.empty()) {
            MarkFailed(masterSlot);
            return {};
        }
        std::memcpy(copied.data(), payload.data(), payload.size());
        storedPtr = copied.data();
    }

    m_blobs[meta.payloadSlot] = BlobRecord{
        .data = storedPtr,
        .size = static_cast<u32>(payload.size()),
    };

    meta.lastUsedFrame = m_currentFrame;
    return BlobHandle{masterSlot, meta.generation};
}

AudioHandle AssetBank::CommitAudio(
    u16 masterSlot,
    std::span<const f32> pcmSamples,
    u32 sampleRate,
    u8 channels,
    bool looping) noexcept {
    if (masterSlot == 0U || masterSlot >= m_slotPool.highWater || channels == 0U) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    meta.kind = static_cast<u16>(AssetKind::Audio);
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

    m_audio[meta.payloadSlot] = AudioRecord{
        .pcm = storedPcm,
        .frameCount = static_cast<u32>(pcmSamples.size() / channels),
        .sampleRate = static_cast<u16>(std::clamp(sampleRate, 8000U, 65535U)),
        .channels = channels,
        .looping = looping,
    };

    meta.lastUsedFrame = m_currentFrame;
    return AudioHandle{masterSlot, meta.generation};
}

TextureAssetHandle AssetBank::LoadTextureFromDisk(
    AssetId id,
    u8 scope,
    std::string_view primaryPngPath,
    std::string_view fallbackBmpPath,
    u16 width,
    u16 height,
    TextureHandle gpu) noexcept {
    bool needsLoad = false;
    const u16 slot = AcquireSlot(id, AssetKind::Texture, scope, needsLoad);
    if (slot == 0U) return {};
    if (!needsLoad) {
        return TextureAssetHandle{slot, m_slots[slot].generation};
    }

    const usize byteSize = static_cast<usize>(width) * height * 4U;
    std::span<u8> pixels = AllocatePayloadSlice(byteSize);
    if (pixels.empty() ||
        !ImageLoader::LoadRgbaImageFromDisk(primaryPngPath, fallbackBmpPath, width, height, pixels)) {
        if (!pixels.empty()) FreePayloadSlice(pixels.data(), byteSize);
        MarkFailed(slot);
        return {};
    }

    SlotMeta& meta = m_slots[slot];
    const u16 texIdx = meta.payloadSlot;
    const TextureHandle resolvedGpu = gpu.IsValid() ? gpu : kWhiteTexture;
    m_textures[texIdx] = TextureRecord{
        .pixels = pixels.data(),
        .gpu = resolvedGpu,
        .width = width,
        .height = height,
    };
    if (resolvedGpu != kWhiteTexture) {
        PropagateAtlasGpuTexture(slot, resolvedGpu);
    }
    const u64 nextVram = static_cast<u64>(m_currentVramBytes) + byteSize;
    m_currentVramBytes = static_cast<u32>(std::min<u64>(nextVram, 0xFFFFFFFFULL));
    meta.lastUsedFrame = m_currentFrame;
    return TextureAssetHandle{slot, meta.generation};
}

AtlasHandle AssetBank::RegisterGridAtlas(
    AssetId id,
    u8 scope,
    TextureAssetHandle texHandle,
    u16 cols,
    u16 rows,
    Vec2 pivot) noexcept {
    bool needsLoad = false;
    const u16 slot = AcquireSlot(id, AssetKind::Atlas, scope, needsLoad);
    if (slot == 0U) return {};
    if (!needsLoad) {
        return AtlasHandle{slot, m_slots[slot].generation};
    }
    return CommitGridAtlas(slot, texHandle, cols, rows, pivot);
}

BlobHandle AssetBank::RegisterBlob(
    AssetId id,
    u8 scope,
    std::span<const u8> payload) noexcept {
    bool needsLoad = false;
    const u16 slot = AcquireSlot(id, AssetKind::Blob, scope, needsLoad);
    if (slot == 0U) return {};
    if (!needsLoad) {
        return BlobHandle{slot, m_slots[slot].generation};
    }
    return CommitBlob(slot, payload);
}

AudioHandle AssetBank::RegisterAudio(
    AssetId id,
    u8 scope,
    std::span<const f32> pcmSamples,
    u32 sampleRate,
    u8 channels,
    bool looping) noexcept {
    bool needsLoad = false;
    const u16 slot = AcquireSlot(id, AssetKind::Audio, scope, needsLoad);
    if (slot == 0U) return {};
    if (!needsLoad) {
        return AudioHandle{slot, m_slots[slot].generation};
    }
    return CommitAudio(slot, pcmSamples, sampleRate, channels, looping);
}

void AssetBank::EvictSlotInternal(
    u16 slotIdx,
    std::span<TextureHandle> outDestroyedGpuTextures,
    u32& ioGpuDestroyCount) noexcept {
    SlotMeta& meta = m_slots[slotIdx];
    const u16 payloadIdx = meta.payloadSlot;

    switch (meta.Kind()) {
        case AssetKind::Texture:
            if (payloadIdx > 0U && payloadIdx < m_payloadPools[0].highWater) {
                TextureRecord& tex = m_textures[payloadIdx];
                if (tex.pixels != nullptr) {
                    const u64 bytes64 = static_cast<u64>(tex.width) * static_cast<u64>(tex.height) * 4ULL;
                    const u32 bytes = static_cast<u32>(std::min<u64>(bytes64, 0xFFFFFFFFULL));
                    m_currentVramBytes = (m_currentVramBytes >= bytes) ? (m_currentVramBytes - bytes) : 0U;
                    if (tex.pixels != kFallbackWhitePixel && bytes64 > 0ULL) {
                        FreePayloadSlice(tex.pixels, static_cast<usize>(bytes64));
                    }
                }
                for (u16 a = 1U; a < m_payloadPools[1].highWater; ++a) {
                    AtlasRecord& ar = m_atlases[a];
                    if (ar.spriteCount > 0U && ar.texMasterSlot == slotIdx) {
                        for (u16 s = 0U; s < ar.spriteCount; ++s) {
                            const u16 spIdx = static_cast<u16>(ar.firstSprite + s);
                            if (spIdx < m_highWaterSprite) m_sprites[spIdx].gpuTexture = kWhiteTexture;
                        }
                        ar.texMasterSlot = 0U;
                    }
                }
                if (tex.gpu.IsValid() && tex.gpu != kWhiteTexture) {
                    if (m_gpuDestroyFn != nullptr) {
                        m_gpuDestroyFn(tex.gpu, m_gpuDestroyUserCtx);
                    } else if (ioGpuDestroyCount < outDestroyedGpuTextures.size()) {
                        outDestroyedGpuTextures[ioGpuDestroyCount++] = tex.gpu;
                    }
                }
                FreeToPool(m_payloadPools[0], tex, &TextureRecord::width, payloadIdx);
            }
            break;
        case AssetKind::Atlas:
            if (payloadIdx > 0U && payloadIdx < m_payloadPools[1].highWater) {
                AtlasRecord& ar = m_atlases[payloadIdx];
                for (u16 s = 0U; s < ar.spriteCount; ++s) {
                    const u16 spIdx = static_cast<u16>(ar.firstSprite + s);
                    if (spIdx < m_highWaterSprite) {
                        m_sprites[spIdx].generation = 0U;
                    }
                }
                while (m_highWaterSprite > 1U && m_sprites[m_highWaterSprite - 1U].generation == 0U) {
                    --m_highWaterSprite;
                }
                FreeToPool(m_payloadPools[1], ar, &AtlasRecord::firstSprite, payloadIdx);
            }
            break;
        case AssetKind::Blob:
            if (payloadIdx > 0U && payloadIdx < m_payloadPools[2].highWater) {
                BlobRecord& br = m_blobs[payloadIdx];
                if (br.data != nullptr && br.data != &kFallbackBlobByte && br.size > 0U) {
                    FreePayloadSlice(br.data, br.size);
                }
                FreeToPool(m_payloadPools[2], br, &BlobRecord::size, payloadIdx);
            }
            break;
        case AssetKind::Audio:
            if (payloadIdx > 0U && payloadIdx < m_payloadPools[3].highWater) {
                AudioRecord& au = m_audio[payloadIdx];
                const usize pcmBytes = static_cast<usize>(au.TotalSamples()) * sizeof(f32);
                if (au.pcm != nullptr && au.pcm != kFallbackSilenceSample && pcmBytes > 0U) {
                    FreePayloadSlice(au.pcm, pcmBytes);
                }
                FreeToPool(m_payloadPools[3], au, &AudioRecord::sampleRate, payloadIdx);
            }
            break;
        default:
            break;
    }

    if (meta.assetId != 0ULL) {
        (void)m_idToSlot.Erase(meta.assetId);
        meta.assetId = 0ULL;
    }

    meta.scopeMask = 0U;
    meta.lastUsedFrame = 0U;
    meta.kind = 0U;
    meta.payloadSlot = m_slotPool.freeHead;
    m_slotPool.freeHead = slotIdx;
    ++m_evictedCount;
}

u32 AssetBank::EvictUnpinned(std::span<TextureHandle> outDestroyedGpuTextures) noexcept {
    u32 evicted = 0U;
    u32 gpuDestroyed = 0U;
    for (u16 i = 1U; i < m_slotPool.highWater; ++i) {
        const SlotMeta& meta = m_slots[i];
        if (meta.assetId != 0ULL && (meta.scopeMask & m_activeScopesMask) == 0U) {
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
        u16 oldestFrame = 0xFFFFU;
        for (u16 i = 1U; i < m_slotPool.highWater; ++i) {
            const SlotMeta& meta = m_slots[i];
            if (meta.assetId != 0ULL &&
                meta.Kind() == AssetKind::Texture &&
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

} // namespace en
