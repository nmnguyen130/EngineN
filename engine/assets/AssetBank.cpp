#include "assets/AssetBank.h"
#include <algorithm>

namespace en {

Result<void, AssetErrc> AssetBank::Init(const AssetBankConfig& cfg) noexcept {
    Shutdown();
    if (cfg.arenaBytes < 65536 || cfg.maxSlots < 4) return Err(AssetErrc::InvalidConfig);
    if (!m_arena.Init(cfg.arenaBytes)) return Err(AssetErrc::OutOfMemory);

    m_slots = m_arena.Carve<SlotMeta>(cfg.maxSlots);
    m_sprites = m_arena.Carve<SpriteFrame>(cfg.maxSprites);
    m_spriteGens = m_arena.Carve<u16>(cfg.maxSprites);
    m_atlases = m_arena.Carve<AtlasRecord>(cfg.maxAtlases);
    m_textures = m_arena.Carve<TextureRecord>(cfg.maxTextures);
    m_blobs = m_arena.Carve<BlobRecord>(cfg.maxBlobs);
    m_audio = m_arena.Carve<AudioRecord>(cfg.maxAudio);

    if (m_slots.empty() || m_sprites.empty() || m_atlases.empty() ||
        m_textures.empty() || m_blobs.empty() || m_audio.empty() ||
        !m_idToSlot.Init(m_arena, cfg.maxSlots * 2U)) {
        Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }

    m_vramBudgetBytes = cfg.vramBudgetBytes;
    m_currentVramBytes = 0;
    m_activeScopesMask = 0;
    m_currentFrame = 1;
    m_scopeSimPending.fill(0);

    m_sprites[0] = SpriteFrame{};
    m_spriteGens[0] = 1;
    m_textures[0] = TextureRecord{{}, kWhiteGpuTexture, 1, 1, 1, 4, AssetState::Ready};
    m_atlases[0] = AtlasRecord{0, 1, 0, 1};
    m_blobs[0] = BlobRecord{nullptr, 0, 0, 1, 1, 0};
    m_audio[0] = AudioRecord{nullptr, 0, 48000, 1, 2, false, 0};

    m_highWaterSlot = 1;
    m_highWaterSprite = 1;
    m_highWaterAtlas = 1;
    m_highWaterTex = 1;
    m_highWaterBlob = 1;
    m_highWaterAudio = 1;
    return {};
}

void AssetBank::Shutdown() noexcept {
    m_arena.Shutdown();
    m_activeScopesMask = 0;
    m_currentVramBytes = 0;
}

Result<ScopeId, AssetErrc> AssetBank::BeginScope() noexcept {
    if (m_activeScopesMask == 0xFFFFFFFFU) return Err(AssetErrc::ScopeExhausted);
    const ScopeId s = static_cast<ScopeId>(std::countr_one(m_activeScopesMask));
    m_activeScopesMask |= (1U << s);
    m_scopeSimPending[s] = 0;
    return s;
}

void AssetBank::EndScope(ScopeId scope) noexcept {
    if (scope >= kMaxAssetScopes) return;
    const u32 bit = (1U << scope);
    if ((m_activeScopesMask & bit) == 0) return;
    m_activeScopesMask &= ~bit;
    m_scopeSimPending[scope] = 0;
    for (u16 i = 1; i < m_highWaterSlot; ++i) {
        SlotMeta& m = m_slots[i];
        m.scopeMask &= ~bit;
        if (m.state == AssetState::Loading) {
            m.lastUsedFrame &= ~bit;
        }
    }
}

const SpriteFrame& AssetBank::GetSprite(SpriteHandle h) const noexcept {
    if (h.spriteIndex < m_highWaterSprite && m_spriteGens[h.spriteIndex] == h.generation) {
        return m_sprites[h.spriteIndex];
    }
    return m_sprites[0];
}

const SpriteFrame& AssetBank::GetAtlasFrame(AtlasHandle h, u16 frameOffset) const noexcept {
    if (h.index < m_highWaterAtlas && m_atlases[h.index].generation == h.generation) {
        const AtlasRecord& rec = m_atlases[h.index];
        if (rec.spriteCount > 0) {
            const u16 idx = static_cast<u16>(rec.firstSprite + (frameOffset % rec.spriteCount));
            if (idx < m_highWaterSprite) return m_sprites[idx];
        }
    }
    return m_sprites[0];
}

SpriteHandle AssetBank::GetSpriteHandle(AtlasHandle h, u16 frameOffset) const noexcept {
    if (h.index < m_highWaterAtlas && m_atlases[h.index].generation == h.generation) {
        const AtlasRecord& rec = m_atlases[h.index];
        if (rec.spriteCount > 0) {
            const u16 idx = static_cast<u16>(rec.firstSprite + (frameOffset % rec.spriteCount));
            if (idx < m_highWaterSprite) return SpriteHandle{idx, m_spriteGens[idx]};
        }
    }
    return SpriteHandle{0, 1};
}

const TextureRecord& AssetBank::GetTexture(TextureHandle h) const noexcept {
    if (h.index < m_highWaterTex && m_textures[h.index].generation == h.generation) {
        return m_textures[h.index];
    }
    return m_textures[0];
}

TextureRecord* AssetBank::TryGetMutableTexture(TextureHandle h) noexcept {
    if (h.index > 0 && h.index < m_highWaterTex && m_textures[h.index].generation == h.generation) {
        return &m_textures[h.index];
    }
    return nullptr;
}

void AssetBank::BindGpuTexture(u16 texSlot, GpuTextureHandle gpu) noexcept {
    if (texSlot == 0 || texSlot >= m_highWaterTex || !gpu.IsValid()) return;
    m_textures[texSlot].gpu = gpu;
    for (u16 a = 1; a < m_highWaterAtlas; ++a) {
        const AtlasRecord& ar = m_atlases[a];
        if (ar.textureSlot == texSlot) {
            for (u16 s = 0; s < ar.spriteCount; ++s) {
                const u16 spIdx = static_cast<u16>(ar.firstSprite + s);
                if (spIdx < m_highWaterSprite) {
                    m_sprites[spIdx].gpuTexture = gpu;
                }
            }
        }
    }
}

const BlobRecord& AssetBank::GetBlob(BlobHandle h) const noexcept {
    if (h.index < m_highWaterBlob && m_blobs[h.index].generation == h.generation) {
        return m_blobs[h.index];
    }
    return m_blobs[0];
}

const AudioRecord& AssetBank::GetAudio(AudioHandle h) const noexcept {
    if (h.index < m_highWaterAudio && m_audio[h.index].generation == h.generation) {
        return m_audio[h.index];
    }
    return m_audio[0];
}

const SlotMeta* AssetBank::FindSlotByAssetId(AssetId id) const noexcept {
    const u16* slotIdx = m_idToSlot.Find(id.value);
    if (!slotIdx || *slotIdx >= m_highWaterSlot) return nullptr;
    return &m_slots[*slotIdx];
}

u16 AssetBank::AcquireSlot(
    AssetId id, AssetKind kind, ScopeId scope, AssetResidency residency, bool& outNewlyCreated) noexcept {
    outNewlyCreated = false;
    if (!id.IsValid() || scope >= kMaxAssetScopes) return 0;
    const u32 scopeBit = (1U << scope);

    if (const u16* existing = m_idToSlot.Find(id.value)) {
        SlotMeta& meta = m_slots[*existing];
        meta.scopeMask |= scopeBit;
        if (meta.state == AssetState::Loading && residency == AssetResidency::Sim) {
            if ((meta.lastUsedFrame & scopeBit) == 0) {
                meta.lastUsedFrame |= scopeBit;
                ++m_scopeSimPending[scope];
            }
        } else if (meta.state == AssetState::Ready) {
            meta.lastUsedFrame = m_currentFrame;
        }
        return *existing;
    }

    if (m_highWaterSlot >= m_slots.size()) return 0;
    const u16 slotIdx = m_highWaterSlot++;
    SlotMeta& meta = m_slots[slotIdx];
    meta.scopeMask = scopeBit;
    meta.generation = 1;
    meta.version = 1;
    meta.kind = kind;
    meta.state = AssetState::Loading;
    meta.lastUsedFrame = (residency == AssetResidency::Sim) ? scopeBit : 0U;
    if (residency == AssetResidency::Sim) {
        ++m_scopeSimPending[scope];
    }
    (void)m_idToSlot.InsertOrAssign(id.value, slotIdx);
    outNewlyCreated = true;
    return slotIdx;
}

void AssetBank::CompleteSlot(SlotMeta& meta, AssetState finalState) noexcept {
    if (meta.state == AssetState::Loading) {
        u32 pendingMask = meta.lastUsedFrame;
        while (pendingMask != 0) {
            const u8 s = static_cast<u8>(std::countr_zero(pendingMask));
            pendingMask &= (pendingMask - 1U);
            if (m_scopeSimPending[s] > 0) --m_scopeSimPending[s];
        }
    }
    meta.state = finalState;
    meta.lastUsedFrame = m_currentFrame;
}

void AssetBank::MarkFailed(u16 masterSlot) noexcept {
    if (masterSlot > 0 && masterSlot < m_highWaterSlot) {
        CompleteSlot(m_slots[masterSlot], AssetState::Failed);
    }
}

TextureHandle AssetBank::CommitTexture(
    u16 masterSlot, u16 width, u16 height, std::span<const u8> rgbaPixels, GpuTextureHandle gpu) noexcept {
    if (masterSlot == 0 || masterSlot >= m_highWaterSlot || m_highWaterTex >= m_textures.size()) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    const u16 texIdx = (meta.payloadSlot != 0) ? meta.payloadSlot : m_highWaterTex++;
    meta.payloadSlot = texIdx;

    std::span<u8> copied{};
    if (!rgbaPixels.empty()) {
        copied = AllocatePayloadSlice(rgbaPixels.size());
        if (!copied.empty()) std::memcpy(copied.data(), rgbaPixels.data(), rgbaPixels.size());
    }

    TextureRecord& rec = m_textures[texIdx];
    rec.pixels = copied;
    rec.gpu = gpu;
    rec.width = width;
    rec.height = height;
    rec.generation = meta.generation;
    rec.state = AssetState::Ready;

    m_currentVramBytes += static_cast<usize>(width) * static_cast<usize>(height) * 4U;
    CompleteSlot(meta, AssetState::Ready);
    return TextureHandle{texIdx, rec.generation};
}

AtlasHandle AssetBank::CommitAtlas(
    u16 masterSlot, TextureHandle texHandle, std::span<const CookedAtlasSpriteEntry> entries) noexcept {
    if (masterSlot == 0 || masterSlot >= m_highWaterSlot ||
        m_highWaterAtlas >= m_atlases.size() ||
        m_highWaterSprite + entries.size() > m_sprites.size()) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    const u16 atlasIdx = (meta.payloadSlot != 0) ? meta.payloadSlot : m_highWaterAtlas++;
    meta.payloadSlot = atlasIdx;

    const TextureRecord& texRec = GetTexture(texHandle);
    const u16 firstSprite = m_highWaterSprite;
    for (const CookedAtlasSpriteEntry& e : entries) {
        const u16 spIdx = m_highWaterSprite++;
        m_sprites[spIdx] = SpriteFrame{
            Rect{e.u0, e.v0, e.u1 - e.u0, e.v1 - e.v0},
            e.width, e.height, texRec.gpu, e.nameHash
        };
        m_spriteGens[spIdx] = meta.generation;
    }

    AtlasRecord& rec = m_atlases[atlasIdx];
    rec.firstSprite = firstSprite;
    rec.spriteCount = static_cast<u16>(entries.size());
    rec.textureSlot = texHandle.index;
    rec.generation = meta.generation;

    CompleteSlot(meta, AssetState::Ready);
    return AtlasHandle{atlasIdx, rec.generation};
}

BlobHandle AssetBank::CommitBlob(u16 masterSlot, std::span<const u8> payload, u32 schemaHash) noexcept {
    if (masterSlot == 0 || masterSlot >= m_highWaterSlot || m_highWaterBlob >= m_blobs.size()) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    const u16 blobIdx = (meta.payloadSlot != 0) ? meta.payloadSlot : m_highWaterBlob++;
    meta.payloadSlot = blobIdx;

    std::span<u8> copied = AllocatePayloadSlice(payload.size());
    if (!payload.empty() && copied.empty()) {
        MarkFailed(masterSlot);
        return {};
    }
    if (!payload.empty()) std::memcpy(copied.data(), payload.data(), payload.size());

    BlobRecord& rec = m_blobs[blobIdx];
    rec.data = copied.data();
    rec.size = static_cast<u32>(copied.size());
    rec.schemaHash = schemaHash;
    rec.generation = meta.generation;
    rec.version = meta.version;
    rec.crc32 = ComputeCrc32(copied);

    CompleteSlot(meta, AssetState::Ready);
    return BlobHandle{blobIdx, rec.generation};
}

bool AssetBank::HotReloadBlob(AssetId id, std::span<const u8> newPayload) noexcept {
    const u16* slotIdx = m_idToSlot.Find(id.value);
    if (!slotIdx || *slotIdx == 0 || *slotIdx >= m_highWaterSlot) return false;
    SlotMeta& meta = m_slots[*slotIdx];
    if (meta.kind != AssetKind::Blob || meta.payloadSlot == 0 || meta.payloadSlot >= m_highWaterBlob) return false;
    std::span<u8> copied = AllocatePayloadSlice(newPayload.size());
    if (!newPayload.empty() && copied.empty()) return false;
    if (!newPayload.empty()) std::memcpy(copied.data(), newPayload.data(), newPayload.size());
    ++meta.version;
    BlobRecord& rec = m_blobs[meta.payloadSlot];
    rec.data = copied.data();
    rec.size = static_cast<u32>(copied.size());
    rec.version = meta.version;
    rec.crc32 = ComputeCrc32(copied);
    return true;
}

AudioHandle AssetBank::CommitAudio(
    u16 masterSlot, std::span<const f32> pcmSamples, u32 sampleRate, u8 channels) noexcept {
    if (masterSlot == 0 || masterSlot >= m_highWaterSlot || m_highWaterAudio >= m_audio.size()) {
        MarkFailed(masterSlot);
        return {};
    }
    SlotMeta& meta = m_slots[masterSlot];
    const u16 audIdx = (meta.payloadSlot != 0) ? meta.payloadSlot : m_highWaterAudio++;
    meta.payloadSlot = audIdx;

    std::span<f32> copied = m_arena.Carve<f32>(pcmSamples.size());
    if (!pcmSamples.empty() && copied.empty()) {
        MarkFailed(masterSlot);
        return {};
    }
    if (!pcmSamples.empty()) std::memcpy(copied.data(), pcmSamples.data(), pcmSamples.size_bytes());

    AudioRecord& rec = m_audio[audIdx];
    rec.pcm = copied.data();
    rec.frameCount = channels > 0 ? static_cast<u32>(pcmSamples.size() / channels) : 0;
    rec.sampleRate = sampleRate;
    rec.generation = meta.generation;
    rec.channels = channels;
    rec.looping = false;
    rec.totalSamples = static_cast<u32>(copied.size());

    CompleteSlot(meta, AssetState::Ready);
    return AudioHandle{audIdx, rec.generation};
}

u32 AssetBank::EvictUnpinned(std::span<GpuTextureHandle> outDestroyedGpuTextures) noexcept {
    u32 evicted = 0;
    for (u16 i = 1; i < m_highWaterSlot; ++i) {
        SlotMeta& meta = m_slots[i];
        if (meta.state != AssetState::Ready || (meta.scopeMask & m_activeScopesMask) != 0) continue;
        if (meta.kind == AssetKind::Texture && meta.payloadSlot < m_highWaterTex) {
            TextureRecord& tex = m_textures[meta.payloadSlot];
            const usize bytes = static_cast<usize>(tex.width) * static_cast<usize>(tex.height) * 4U;
            if (m_currentVramBytes >= bytes) m_currentVramBytes -= bytes;
            if (evicted < outDestroyedGpuTextures.size() && tex.gpu.IsValid() && tex.gpu != kWhiteGpuTexture) {
                outDestroyedGpuTextures[evicted] = tex.gpu;
            }
            tex.generation = static_cast<u16>(tex.generation + 1U);
            tex.gpu = kWhiteGpuTexture;
        }
        meta.state = AssetState::Unloaded;
        ++meta.generation;
        ++evicted;
    }
    return evicted;
}

Result<usize, AssetErrc> AssetBank::DecompressLz4(std::span<const u8> src, std::span<u8> dst) noexcept {
    usize sp = 0;
    usize dp = 0;
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
        std::memcpy(dst.data() + dp, src.data() + sp, litLen);
        sp += litLen;
        dp += litLen;
        if (sp == src.size()) break;

        if (sp + 2 > src.size()) return Err(AssetErrc::DecompressionFailed);
        const u16 offset = static_cast<u16>(src[sp]) | (static_cast<u16>(src[sp + 1]) << 8U);
        sp += 2;
        if (offset == 0 || offset > dp) return Err(AssetErrc::DecompressionFailed);

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
        for (usize i = 0; i < matchLen; ++i) {
            dst[dp + i] = dst[dp - offset + i];
        }
        dp += matchLen;
    }
    return dp;
}

Result<usize, AssetErrc> AssetBank::CompressLz4Literal(std::span<const u8> src, std::span<u8> dst) noexcept {
    const usize n = src.size();
    const usize extra = (n >= 15U) ? ((n - 15U) / 255U + 1U) : 0U;
    if (dst.size() < 1U + extra + n) return Err(AssetErrc::OutOfMemory);
    usize dp = 0;
    if (n < 15U) {
        dst[dp++] = static_cast<u8>(n << 4U);
    } else {
        dst[dp++] = 0xF0U;
        usize rem = n - 15U;
        while (rem >= 255U) {
            dst[dp++] = 255U;
            rem -= 255U;
        }
        dst[dp++] = static_cast<u8>(rem);
    }
    if (n > 0) {
        std::memcpy(dst.data() + dp, src.data(), n);
        dp += n;
    }
    return dp;
}

u64 AssetBank::ComputeAuditHash() const noexcept {
    u64 h = Mix64(static_cast<u64>(m_highWaterSlot) | (static_cast<u64>(m_activeScopesMask) << 16U));
    h = HashCombine64(h, static_cast<u64>(m_highWaterSprite) | (static_cast<u64>(m_highWaterBlob) << 16U));
    return h;
}

} // namespace en
