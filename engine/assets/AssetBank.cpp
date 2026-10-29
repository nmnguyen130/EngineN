#include "assets/AssetBank.h"
#include "assets/ImageLoader.h"

#include <algorithm>
#include <cstring>

namespace en {
namespace {

constexpr u8  kWhitePx[4]{255, 255, 255, 255};
constexpr u8  kEmptyByte{0};
constexpr f32 kSilence[2]{0.0f, 0.0f};

[[nodiscard]] constexpr u16 NextGen(u16 g) noexcept {
    const u16 n = static_cast<u16>(g + 1U);
    return n == 0U ? 1U : n;
}

} // namespace

Result<void, AssetErrc> AssetBank::Init(const AssetBankConfig& cfg) noexcept {
    Shutdown();
    if (cfg.arenaBytes < 65536U || cfg.maxSlots < 4U || cfg.maxSprites < 4U ||
        cfg.maxAtlases < 2U || cfg.maxTextures < 2U || cfg.maxBlobs < 2U ||
        cfg.maxAudio < 2U) {
        return Err(AssetErrc::InvalidConfig);
    }
    if (!m_arena.Init(cfg.arenaBytes)) return Err(AssetErrc::OutOfMemory);

    auto meta     = m_arena.Alloc<SlotMeta>(cfg.maxSlots);
    auto sprites  = m_arena.Alloc<SpriteFrame>(cfg.maxSprites);
    auto atlases  = m_arena.Alloc<AtlasRecord>(cfg.maxSlots);
    auto textures = m_arena.Alloc<TextureRecord>(cfg.maxSlots);
    auto blobs    = m_arena.Alloc<BlobRecord>(cfg.maxSlots);
    auto audio    = m_arena.Alloc<AudioRecord>(cfg.maxSlots);

    if (meta.empty() || sprites.empty() || atlases.empty() || textures.empty() ||
        blobs.empty() || audio.empty() ||
        !m_idMap.Init(m_arena, static_cast<u32>(cfg.maxSlots) * 2U)) {
        Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }

    m_meta     = meta.data();
    m_sprites  = sprites.data();
    m_atlases  = atlases.data();
    m_textures = textures.data();
    m_blobs    = blobs.data();
    m_audio    = audio.data();

    m_vramBudget = cfg.vramBudgetBytes;
    m_maxSlots   = cfg.maxSlots;
    m_slotHigh   = 1U;
    m_slotFree   = 0U;
    m_maxSprites = cfg.maxSprites;
    m_spriteHigh = 1U;

    m_maxByKind  = {0U, cfg.maxTextures, cfg.maxAtlases, cfg.maxBlobs, cfg.maxAudio};
    m_liveByKind = {0U, 1U, 1U, 1U, 1U};

    m_meta[0]     = SlotMeta{.scopeMask = 0xFFFFU, .lastUsedFrame = 1U, .generation = 1U};
    m_sprites[0]  = SpriteFrame{.generation = 1U};
    m_textures[0] = TextureRecord{.pixels = kWhitePx, .gpu = kWhiteTexture, .width = 1, .height = 1};
    m_atlases[0]  = AtlasRecord{.firstSprite = 0, .spriteCount = 1};
    m_blobs[0]    = BlobRecord{.data = &kEmptyByte};
    m_audio[0]    = AudioRecord{.pcm = kSilence, .sampleRate = 48000, .channels = 2};

    m_payloadMark = m_arena.Mark();
    return {};
}

void AssetBank::Shutdown() noexcept {
    m_arena.Shutdown();
    m_meta        = nullptr;
    m_sprites     = nullptr;
    m_atlases     = nullptr;
    m_textures    = nullptr;
    m_blobs       = nullptr;
    m_audio       = nullptr;
    m_freeHead    = nullptr;
    m_vram        = 0U;
    m_payloadMark = 0U;
    m_evicts      = 0U;
    m_maxSlots    = 0U;
    m_slotHigh    = 0U;
    m_slotFree    = 0U;
    m_maxSprites  = 0U;
    m_spriteHigh  = 0U;
    m_scopes      = 0U;
    m_frame       = 1U;
    m_maxByKind   = {};
    m_liveByKind  = {};
}

Result<u8, AssetErrc> AssetBank::BeginScope() noexcept {
    if (m_scopes == 0xFFFFU) return Err(AssetErrc::ScopeExhausted);
    const u8 s = static_cast<u8>(std::countr_one(m_scopes));
    m_scopes |= static_cast<u16>(1U << s);
    return s;
}

void AssetBank::EndScope(u8 scope) noexcept {
    if (scope >= kMaxAssetScopes) return;
    const u16 bit = static_cast<u16>(1U << scope);
    if ((m_scopes & bit) == 0U) return;
    m_scopes &= static_cast<u16>(~bit);
    for (u16 i = 1U; i < m_slotHigh; ++i) {
        m_meta[i].scopeMask &= static_cast<u16>(~bit);
    }
}

std::span<u8> AssetBank::AllocBytes(usize n) noexcept {
    if (n == 0U || !m_meta) return {};
    const u32 aligned = static_cast<u32>(AlignUp(n, Arena::kSlabAlignment));
    if (aligned == 0U) return {};

    FreeBlock* prev = nullptr;
    for (FreeBlock* cur = m_freeHead; cur; prev = cur, cur = cur->next) {
        if (cur->bytes < aligned) continue;
        const u32 rem = cur->bytes - aligned;
        u8* p = nullptr;
        if (rem >= Arena::kSlabAlignment) {
            cur->bytes = rem;
            p = reinterpret_cast<u8*>(cur) + rem;
        } else {
            if (prev) prev->next = cur->next;
            else m_freeHead = cur->next;
            p = reinterpret_cast<u8*>(cur);
        }
        return {p, n};
    }

    u8* raw = m_arena.AllocBytes(aligned, Arena::kSlabAlignment);
    return raw ? std::span<u8>{raw, n} : std::span<u8>{};
}

void AssetBank::FreeBytes(const void* ptr, usize n) noexcept {
    if (!ptr || n == 0U || !m_arena.Base()) return;
    const u8* base = m_arena.Base();
    const auto* bp = static_cast<const u8*>(ptr);
    if (bp < base) return;

    const usize off = static_cast<usize>(bp - base);
    const u32 aligned = static_cast<u32>(AlignUp(n, Arena::kSlabAlignment));
    if (off < m_payloadMark || off + aligned > m_arena.Used()) return;

    auto* blk = reinterpret_cast<FreeBlock*>(const_cast<u8*>(bp));
    blk->bytes = aligned;

    FreeBlock* prev = nullptr;
    FreeBlock* cur = m_freeHead;
    while (cur && reinterpret_cast<const u8*>(cur) < bp) {
        prev = cur;
        cur = cur->next;
    }
    blk->next = cur;
    if (prev) prev->next = blk;
    else m_freeHead = blk;

    if (cur && (bp + blk->bytes) == reinterpret_cast<u8*>(cur)) {
        blk->bytes += cur->bytes;
        blk->next = cur->next;
    }
    if (prev && (reinterpret_cast<u8*>(prev) + prev->bytes) == bp) {
        prev->bytes += blk->bytes;
        prev->next = blk->next;
        blk = prev;
    }

    const usize blkOff = static_cast<usize>(reinterpret_cast<const u8*>(blk) - base);
    if (blkOff + blk->bytes == m_arena.Used()) {
        if (m_freeHead == blk) {
            m_freeHead = nullptr;
        } else {
            FreeBlock* p = m_freeHead;
            while (p && p->next != blk) p = p->next;
            if (p) p->next = nullptr;
        }
        m_arena.Rollback(static_cast<u32>(blkOff));
    }
}

void AssetBank::PropagateGpu(u16 texSlot, TextureHandle gpu) noexcept {
    for (u16 s = 1U; s < m_slotHigh; ++s) {
        if (m_meta[s].assetId == 0ULL || m_meta[s].Kind() != AssetKind::Atlas) continue;
        AtlasRecord& ar = m_atlases[s];
        if (ar.spriteCount == 0U || ar.texMasterSlot != texSlot) continue;
        for (u16 i = 0U; i < ar.spriteCount; ++i) {
            const u16 si = static_cast<u16>(ar.firstSprite + i);
            if (si < m_spriteHigh) m_sprites[si].gpuTexture = gpu;
        }
    }
}

void AssetBank::BindGpu(TextureAssetHandle tex, TextureHandle gpu) noexcept {
    if (!gpu.IsValid()) return;
    const u16 ti = Resolve(tex, AssetKind::Texture);
    if (ti == 0U) return;
    m_textures[ti].gpu = gpu;
    PropagateGpu(ti, gpu);
}

void AssetBank::Touch(AssetId id) noexcept {
    if (const u16* p = m_idMap.Find(id.value); p && *p > 0U && *p < m_slotHigh) {
        m_meta[*p].lastUsedFrame = m_frame;
    }
}

void AssetBank::TouchAtlas(AtlasHandle h) noexcept {
    const u16 ai = Resolve(h, AssetKind::Atlas);
    if (ai == 0U) return;
    m_meta[ai].lastUsedFrame = m_frame;
    const u16 tm = m_atlases[ai].texMasterSlot;
    if (tm > 0U && tm < m_slotHigh) m_meta[tm].lastUsedFrame = m_frame;
}

u16 AssetBank::Acquire(AssetId id, AssetKind kind, u8 scope, bool& needsLoad) noexcept {
    needsLoad = false;
    const usize kindIdx = static_cast<usize>(kind);
    if (!id.IsValid() || scope >= kMaxAssetScopes || !m_meta || kindIdx == 0U || kindIdx > 4U) {
        return 0U;
    }
    const u16 bit = static_cast<u16>(1U << scope);
    if ((m_scopes & bit) == 0U) return 0U;

    if (const u16* existing = m_idMap.Find(id.value)) {
        SlotMeta& m = m_meta[*existing];
        if (m.Kind() != kind) return 0U;
        m.scopeMask |= bit;
        m.lastUsedFrame = m_frame;
        return *existing;
    }

    if (m_liveByKind[kindIdx] >= m_maxByKind[kindIdx]) return 0U;
    if (m_slotFree == 0U && m_slotHigh >= m_maxSlots) return 0U;

    u16 slot = m_slotFree;
    u16 oldGen = 0U;
    const bool fromFree = (slot != 0U);
    if (fromFree) {
        oldGen = m_meta[slot].generation;
        m_slotFree = m_meta[slot].NextFree();
    } else {
        slot = m_slotHigh++;
    }

    if (!m_idMap.Insert(id.value, slot)) {
        if (fromFree) {
            m_meta[slot].SetNextFree(m_slotFree);
            m_slotFree = slot;
        } else {
            --m_slotHigh;
        }
        return 0U;
    }

    m_meta[slot] = SlotMeta{
        .assetId       = id.value,
        .scopeMask     = bit,
        .lastUsedFrame = m_frame,
        .generation    = NextGen(oldGen),
    };
    m_meta[slot].SetKind(kind);
    ++m_liveByKind[kindIdx];
    needsLoad = true;
    return slot;
}

void AssetBank::Fail(u16 slot) noexcept {
    if (slot > 0U && slot < m_slotHigh && m_meta[slot].assetId != 0ULL) {
        u32 dummy = 0U;
        EvictOne(slot, {}, dummy);
        if (m_evicts > 0U) --m_evicts;
    }
}

TextureAssetHandle AssetBank::RegisterTexture(
    AssetId id, u8 scope, u16 w, u16 h, std::span<const u8> rgba, TextureHandle gpu) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Texture, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return TextureAssetHandle{slot, m_meta[slot].generation};

    const usize bytes = static_cast<usize>(w) * static_cast<usize>(h) * 4U;
    if (w == 0U || h == 0U || w > 16384U || h > 16384U ||
        (!rgba.empty() && rgba.size() != bytes)) {
        Fail(slot);
        return {};
    }

    const u8* stored = kWhitePx;
    if (!rgba.empty()) {
        auto copy = AllocBytes(bytes);
        if (copy.empty()) {
            Fail(slot);
            return {};
        }
        std::memcpy(copy.data(), rgba.data(), bytes);
        stored = copy.data();
    }

    const TextureHandle g = gpu.IsValid() ? gpu : kWhiteTexture;
    m_textures[slot] = TextureRecord{.pixels = stored, .gpu = g, .width = w, .height = h};
    if (g != kWhiteTexture) PropagateGpu(slot, g);
    m_vram = static_cast<u32>(std::min<u64>(static_cast<u64>(m_vram) + bytes, 0xFFFFFFFFULL));
    return TextureAssetHandle{slot, m_meta[slot].generation};
}

TextureAssetHandle AssetBank::LoadTexture(
    AssetId id, u8 scope, std::string_view png, std::string_view bmp,
    u16 w, u16 h, TextureHandle gpu) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Texture, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return TextureAssetHandle{slot, m_meta[slot].generation};

    const usize bytes = static_cast<usize>(w) * static_cast<usize>(h) * 4U;
    if (w == 0U || h == 0U || bytes == 0U) {
        Fail(slot);
        return {};
    }
    auto pixels = AllocBytes(bytes);
    if (pixels.empty() || !ImageLoader::LoadRgbaImageFromDisk(png, bmp, w, h, pixels)) {
        if (!pixels.empty()) FreeBytes(pixels.data(), bytes);
        Fail(slot);
        return {};
    }

    const TextureHandle g = gpu.IsValid() ? gpu : kWhiteTexture;
    m_textures[slot] = TextureRecord{.pixels = pixels.data(), .gpu = g, .width = w, .height = h};
    if (g != kWhiteTexture) PropagateGpu(slot, g);
    m_vram = static_cast<u32>(std::min<u64>(static_cast<u64>(m_vram) + bytes, 0xFFFFFFFFULL));
    return TextureAssetHandle{slot, m_meta[slot].generation};
}

AtlasHandle AssetBank::RegisterGridAtlas(
    AssetId id, u8 scope, TextureAssetHandle tex, u16 cols, u16 rows, Vec2 pivot) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Atlas, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return AtlasHandle{slot, m_meta[slot].generation};

    const u32 total = static_cast<u32>(cols) * static_cast<u32>(rows);
    if (cols == 0U || rows == 0U || m_spriteHigh + total > m_maxSprites) {
        Fail(slot);
        return {};
    }

    const u16 ti = Resolve(tex, AssetKind::Texture);
    const TextureRecord& tr = m_textures[ti];
    const f32 invC = 1.0f / static_cast<f32>(cols);
    const f32 invR = 1.0f / static_cast<f32>(rows);
    const f32 cellW = static_cast<f32>(tr.width > 0U ? tr.width : cols) * invC;
    const f32 cellH = static_cast<f32>(tr.height > 0U ? tr.height : rows) * invR;

    const u16 first = m_spriteHigh;
    const u16 gen = m_meta[slot].generation;
    for (u16 r = 0U; r < rows; ++r) {
        for (u16 c = 0U; c < cols; ++c) {
            m_sprites[m_spriteHigh++] = SpriteFrame{
                .uv         = Rect{static_cast<f32>(c) * invC, static_cast<f32>(r) * invR, invC, invR},
                .width      = cellW,
                .height     = cellH,
                .pivot      = pivot,
                .gpuTexture = tr.gpu,
                .generation = gen,
            };
        }
    }

    m_atlases[slot] = AtlasRecord{
        .firstSprite   = first,
        .spriteCount   = static_cast<u16>(total),
        .texMasterSlot = ti,
    };
    return AtlasHandle{slot, gen};
}

BlobHandle AssetBank::RegisterBlob(AssetId id, u8 scope, std::span<const u8> data) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Blob, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return BlobHandle{slot, m_meta[slot].generation};

    const u8* stored = &kEmptyByte;
    if (!data.empty()) {
        auto copy = AllocBytes(data.size());
        if (copy.empty()) {
            Fail(slot);
            return {};
        }
        std::memcpy(copy.data(), data.data(), data.size());
        stored = copy.data();
    }

    m_blobs[slot] = BlobRecord{.data = stored, .size = static_cast<u32>(data.size())};
    return BlobHandle{slot, m_meta[slot].generation};
}

AudioHandle AssetBank::RegisterAudio(
    AssetId id, u8 scope, std::span<const f32> pcm, u32 rate, u8 ch, bool loop) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Audio, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return AudioHandle{slot, m_meta[slot].generation};

    if (ch == 0U || (!pcm.empty() && (pcm.size() % ch) != 0U)) {
        Fail(slot);
        return {};
    }

    const f32* stored = kSilence;
    if (!pcm.empty()) {
        auto raw = AllocBytes(pcm.size_bytes());
        if (raw.empty()) {
            Fail(slot);
            return {};
        }
        std::memcpy(raw.data(), pcm.data(), pcm.size_bytes());
        stored = reinterpret_cast<const f32*>(raw.data());
    }

    m_audio[slot] = AudioRecord{
        .pcm        = stored,
        .frameCount = static_cast<u32>(pcm.size() / ch),
        .sampleRate = static_cast<u16>(std::clamp(rate, 8000U, 65535U)),
        .channels   = ch,
        .looping    = loop,
    };
    return AudioHandle{slot, m_meta[slot].generation};
}

void AssetBank::EvictOne(u16 slot, std::span<TextureHandle> outGpu, u32& nGpu) noexcept {
    SlotMeta& m = m_meta[slot];
    const usize kindIdx = static_cast<usize>(m.Kind());

    switch (m.Kind()) {
        case AssetKind::Texture: {
            TextureRecord& t = m_textures[slot];
            if (t.width > 0U && t.height > 0U) {
                const u64 b64 = static_cast<u64>(t.width) * static_cast<u64>(t.height) * 4ULL;
                const u32 b = static_cast<u32>(std::min<u64>(b64, 0xFFFFFFFFULL));
                m_vram = (m_vram >= b) ? (m_vram - b) : 0U;
                if (t.pixels && t.pixels != kWhitePx) FreeBytes(t.pixels, static_cast<usize>(b64));
            }
            for (u16 a = 1U; a < m_slotHigh; ++a) {
                if (m_meta[a].assetId == 0ULL || m_meta[a].Kind() != AssetKind::Atlas) continue;
                AtlasRecord& ar = m_atlases[a];
                if (ar.spriteCount > 0U && ar.texMasterSlot == slot) {
                    for (u16 s = 0U; s < ar.spriteCount; ++s) {
                        const u16 si = static_cast<u16>(ar.firstSprite + s);
                        if (si < m_spriteHigh) m_sprites[si].gpuTexture = kWhiteTexture;
                    }
                    ar.texMasterSlot = 0U;
                }
            }
            if (t.gpu.IsValid() && t.gpu != kWhiteTexture) {
                if (m_gpuDestroy) m_gpuDestroy(t.gpu, m_gpuCtx);
                else if (nGpu < outGpu.size()) outGpu[nGpu++] = t.gpu;
            }
            t = {};
            break;
        }
        case AssetKind::Atlas: {
            AtlasRecord& ar = m_atlases[slot];
            for (u16 s = 0U; s < ar.spriteCount; ++s) {
                const u16 si = static_cast<u16>(ar.firstSprite + s);
                if (si < m_spriteHigh) m_sprites[si].generation = 0U;
            }
            while (m_spriteHigh > 1U && m_sprites[m_spriteHigh - 1U].generation == 0U) {
                --m_spriteHigh;
            }
            ar = {};
            break;
        }
        case AssetKind::Blob: {
            BlobRecord& b = m_blobs[slot];
            if (b.data && b.data != &kEmptyByte && b.size > 0U) FreeBytes(b.data, b.size);
            b = {};
            break;
        }
        case AssetKind::Audio: {
            AudioRecord& a = m_audio[slot];
            const usize bytes = static_cast<usize>(a.TotalSamples()) * sizeof(f32);
            if (a.pcm && a.pcm != kSilence && bytes > 0U) FreeBytes(a.pcm, bytes);
            a = {};
            break;
        }
        default:
            break;
    }

    if (kindIdx > 0U && kindIdx <= 4U && m_liveByKind[kindIdx] > 0U) {
        --m_liveByKind[kindIdx];
    }
    if (m.assetId != 0ULL) {
        (void)m_idMap.Erase(m.assetId);
        m.assetId = 0ULL;
    }
    m.scopeMask = 0U;
    m.lastUsedFrame = 0U;
    m.nextAndKind = 0U;
    m.SetNextFree(m_slotFree);
    m_slotFree = slot;
    ++m_evicts;
}

u32 AssetBank::EvictUnpinned(std::span<TextureHandle> outGpu) noexcept {
    u32 n = 0U;
    u32 nGpu = 0U;
    for (u16 i = 1U; i < m_slotHigh; ++i) {
        const SlotMeta& m = m_meta[i];
        if (m.assetId != 0ULL && (m.scopeMask & m_scopes) == 0U) {
            EvictOne(i, outGpu, nGpu);
            ++n;
        }
    }
    return n;
}

u32 AssetBank::EvictLru(std::span<TextureHandle> outGpu, u32 budget) noexcept {
    const u32 goal = (budget == 0xFFFFFFFFU) ? m_vramBudget : budget;
    u32 nGpu = 0U;
    u32 n = 0U;
    while (m_vram > goal) {
        u16 oldest = 0U;
        u16 maxAge = 0U;
        for (u16 i = 1U; i < m_slotHigh; ++i) {
            const SlotMeta& m = m_meta[i];
            if (m.assetId == 0ULL || m.Kind() != AssetKind::Texture ||
                (m.scopeMask & m_scopes) != 0U) {
                continue;
            }
            const u16 age = (m_frame >= m.lastUsedFrame)
                                ? static_cast<u16>(m_frame - m.lastUsedFrame)
                                : static_cast<u16>(0xFFFFU - m.lastUsedFrame + m_frame + 1U);
            if (oldest == 0U || age >= maxAge) {
                maxAge = age;
                oldest = i;
            }
        }
        if (oldest == 0U) break;
        EvictOne(oldest, outGpu, nGpu);
        ++n;
    }
    return n;
}

} // namespace en
