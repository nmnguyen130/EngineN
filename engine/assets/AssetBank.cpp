#include "assets/AssetBank.h"
#include "assets/ImageLoader.h"

#include <algorithm>
#include <cstring>

namespace en {
namespace {

constexpr u8 kWhitePx[4]{255, 255, 255, 255};
constexpr u8 kEmptyByte{0};
constexpr f32 kSilence[2]{0.0f, 0.0f};

[[nodiscard]] constexpr u16 NextGen(u16 g) noexcept {
    const u16 n = static_cast<u16>(g + 1U);
    return n == 0U ? 1U : n;
}

template <typename T, typename M>
[[nodiscard]] bool PoolAlloc(AssetBank::Pool& pool, T* table, M T::*link, u16& out) noexcept {
    if (pool.free != 0U) {
        out = pool.free;
        pool.free = static_cast<u16>(table[out].*link);
        table[out].*link = static_cast<M>(0);
        return true;
    }
    if (pool.high < pool.max) {
        out = pool.high++;
        table[out].*link = static_cast<M>(0);
        return true;
    }
    return false;
}

template <typename T, typename M>
void PoolFree(AssetBank::Pool& pool, T& rec, M T::*link, u16 idx) noexcept {
    rec = T{};
    rec.*link = static_cast<M>(pool.free);
    pool.free = idx;
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

    auto meta = m_arena.Carve<SlotMeta>(cfg.maxSlots);
    auto sprites = m_arena.Carve<SpriteFrame>(cfg.maxSprites);
    auto atlases = m_arena.Carve<AtlasRecord>(cfg.maxAtlases);
    auto textures = m_arena.Carve<TextureRecord>(cfg.maxTextures);
    auto blobs = m_arena.Carve<BlobRecord>(cfg.maxBlobs);
    auto audio = m_arena.Carve<AudioRecord>(cfg.maxAudio);

    if (meta.empty() || sprites.empty() || atlases.empty() || textures.empty() ||
        blobs.empty() || audio.empty() ||
        !m_idMap.Init(m_arena, static_cast<u32>(cfg.maxSlots) * 2U)) {
        Shutdown();
        return Err(AssetErrc::OutOfMemory);
    }

    m_meta = meta.data();
    m_sprites = sprites.data();
    m_atlases = atlases.data();
    m_textures = textures.data();
    m_blobs = blobs.data();
    m_audio = audio.data();

    m_vramBudget = cfg.vramBudgetBytes;
    m_slots.Reset(cfg.maxSlots);
    m_pools[kTex].Reset(cfg.maxTextures);
    m_pools[kAtl].Reset(cfg.maxAtlases);
    m_pools[kBlob].Reset(cfg.maxBlobs);
    m_pools[kAud].Reset(cfg.maxAudio);
    m_maxSprites = cfg.maxSprites;
    m_spriteHigh = 1U;

    m_meta[0] = SlotMeta{.scopeMask = 0xFFFFU, .lastUsedFrame = 1U, .generation = 1U};
    m_sprites[0] = SpriteFrame{.generation = 1U};
    m_textures[0] = TextureRecord{.pixels = kWhitePx, .gpu = kWhiteTexture, .width = 1, .height = 1};
    m_atlases[0] = AtlasRecord{.firstSprite = 0, .spriteCount = 1};
    m_blobs[0] = BlobRecord{.data = &kEmptyByte};
    m_audio[0] = AudioRecord{.pcm = kSilence, .sampleRate = 48000, .channels = 2};

    m_payloadMark = m_arena.SaveMarker();
    return {};
}

void AssetBank::Shutdown() noexcept {
    m_arena.Shutdown();
    m_meta = nullptr;
    m_sprites = nullptr;
    m_atlases = nullptr;
    m_textures = nullptr;
    m_blobs = nullptr;
    m_audio = nullptr;
    m_freeHead = nullptr;
    m_vram = 0;
    m_payloadMark = 0;
    m_evicts = 0;
    m_slots = {};
    m_pools = {};
    m_maxSprites = 0;
    m_spriteHigh = 0;
    m_scopes = 0;
    m_frame = 1;
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
    for (u16 i = 1U; i < m_slots.high; ++i) {
        m_meta[i].scopeMask &= static_cast<u16>(~bit);
    }
}

std::span<u8> AssetBank::AllocBytes(usize n) noexcept {
    if (n == 0U || !m_meta) return {};
    const u32 aligned = static_cast<u32>(AlignUp(n, AlignedArena::kCacheLineBytes));
    if (aligned == 0U) return {};

    FreeBlock* prev = nullptr;
    for (FreeBlock* cur = m_freeHead; cur; prev = cur, cur = cur->next) {
        if (cur->bytes < aligned) continue;
        const u32 rem = cur->bytes - aligned;
        u8* p = nullptr;
        if (rem >= AlignedArena::kCacheLineBytes) {
            cur->bytes = rem;
            p = reinterpret_cast<u8*>(cur) + rem;
        } else {
            if (prev) prev->next = cur->next;
            else m_freeHead = cur->next;
            p = reinterpret_cast<u8*>(cur);
        }
        return {p, n};
    }
    return m_arena.Carve<u8>(n);
}

void AssetBank::FreeBytes(const void* ptr, usize n) noexcept {
    if (!ptr || n == 0U || !m_meta) return;
    const auto* base = reinterpret_cast<const u8*>(m_meta);
    const auto* bp = static_cast<const u8*>(ptr);
    if (bp < base) return;

    const usize off = static_cast<usize>(bp - base);
    const u32 aligned = static_cast<u32>(AlignUp(n, AlignedArena::kCacheLineBytes));
    if (off < m_payloadMark || off + aligned > m_arena.UsedBytes()) return;

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
    if (blkOff + blk->bytes == m_arena.UsedBytes()) {
        if (m_freeHead == blk) m_freeHead = nullptr;
        else {
            FreeBlock* p = m_freeHead;
            while (p && p->next != blk) p = p->next;
            if (p) p->next = nullptr;
        }
        m_arena.RollbackToMarker(static_cast<u32>(blkOff));
    }
}

void AssetBank::PropagateGpu(u16 texMaster, TextureHandle gpu) noexcept {
    for (u16 a = 1U; a < m_pools[kAtl].high; ++a) {
        AtlasRecord& ar = m_atlases[a];
        if (ar.spriteCount == 0U || ar.texMasterSlot != texMaster) continue;
        for (u16 s = 0U; s < ar.spriteCount; ++s) {
            const u16 si = static_cast<u16>(ar.firstSprite + s);
            if (si < m_spriteHigh) m_sprites[si].gpuTexture = gpu;
        }
    }
}

void AssetBank::BindGpu(TextureAssetHandle tex, TextureHandle gpu) noexcept {
    if (!gpu.IsValid()) return;
    const u16 ti = Resolve(tex, AssetKind::Texture);
    if (ti == 0U) return;
    m_textures[ti].gpu = gpu;
    PropagateGpu(tex.index, gpu);
}

void AssetBank::Touch(AssetId id) noexcept {
    if (const u16* p = m_idMap.Find(id.value); p && *p > 0U && *p < m_slots.high) {
        m_meta[*p].lastUsedFrame = m_frame;
    }
}

void AssetBank::TouchAtlas(AtlasHandle h) noexcept {
    const u16 ai = Resolve(h, AssetKind::Atlas);
    if (ai == 0U) return;
    m_meta[h.index].lastUsedFrame = m_frame;
    const u16 tm = m_atlases[ai].texMasterSlot;
    if (tm > 0U && tm < m_slots.high) m_meta[tm].lastUsedFrame = m_frame;
}

bool AssetBank::EnsurePayload(SlotMeta& m) noexcept {
    if (m.Payload() != 0U) return true;
    u16 idx = 0;
    bool ok = false;
    switch (m.Kind()) {
        case AssetKind::Texture:
            ok = PoolAlloc(m_pools[kTex], m_textures, &TextureRecord::width, idx);
            break;
        case AssetKind::Atlas:
            ok = PoolAlloc(m_pools[kAtl], m_atlases, &AtlasRecord::firstSprite, idx);
            break;
        case AssetKind::Blob:
            ok = PoolAlloc(m_pools[kBlob], m_blobs, &BlobRecord::size, idx);
            break;
        case AssetKind::Audio:
            ok = PoolAlloc(m_pools[kAud], m_audio, &AudioRecord::sampleRate, idx);
            break;
        default:
            return false;
    }
    if (!ok) return false;
    m.SetPayload(idx);
    return true;
}

u16 AssetBank::Acquire(AssetId id, AssetKind kind, u8 scope, bool& needsLoad) noexcept {
    needsLoad = false;
    if (!id.IsValid() || scope >= kMaxAssetScopes || !m_meta) return 0U;
    const u16 bit = static_cast<u16>(1U << scope);
    if ((m_scopes & bit) == 0U) return 0U;

    if (const u16* existing = m_idMap.Find(id.value)) {
        SlotMeta& m = m_meta[*existing];
        if (m.Kind() != AssetKind::None && kind != AssetKind::None && m.Kind() != kind) return 0U;
        if (m.Kind() == AssetKind::None && kind != AssetKind::None) {
            m.SetKind(kind);
            if (!EnsurePayload(m)) return 0U;
            needsLoad = true;
        }
        m.scopeMask |= bit;
        m.lastUsedFrame = m_frame;
        return *existing;
    }

    if (m_slots.free == 0U && m_slots.high >= m_slots.max) return 0U;

    u16 slot = m_slots.free;
    u16 oldGen = 0;
    const bool fromFree = (slot != 0U);
    if (fromFree) {
        oldGen = m_meta[slot].generation;
        m_slots.free = m_meta[slot].Payload();
    } else {
        slot = m_slots.high++;
    }

    if (!m_idMap.InsertOrAssign(id.value, slot)) {
        if (fromFree) {
            m_meta[slot].SetPayload(m_slots.free);
            m_slots.free = slot;
        } else {
            --m_slots.high;
        }
        return 0U;
    }

    SlotMeta cand{
        .assetId = id.value,
        .scopeMask = bit,
        .lastUsedFrame = m_frame,
        .generation = NextGen(oldGen),
    };
    cand.SetKind(kind);

    if (kind != AssetKind::None && !EnsurePayload(cand)) {
        (void)m_idMap.Erase(id.value);
        if (fromFree) {
            m_meta[slot].SetPayload(m_slots.free);
            m_slots.free = slot;
        } else {
            --m_slots.high;
        }
        return 0U;
    }

    m_meta[slot] = cand;
    needsLoad = true;
    return slot;
}

void AssetBank::Fail(u16 slot) noexcept {
    if (slot > 0U && slot < m_slots.high && m_meta[slot].assetId != 0ULL) {
        u32 dummy = 0;
        EvictOne(slot, {}, dummy);
        if (m_evicts > 0U) --m_evicts;
    }
}

TextureAssetHandle AssetBank::CommitTex(
    u16 slot, u16 w, u16 h, std::span<const u8> rgba, TextureHandle gpu) noexcept {
    const u64 bytes64 = static_cast<u64>(w) * static_cast<u64>(h) * 4ULL;
    if (slot == 0U || slot >= m_slots.high || m_meta[slot].assetId == 0ULL ||
        w == 0U || h == 0U || w > 16384U || h > 16384U || bytes64 > 0x7FFFFFFFULL ||
        (!rgba.empty() && rgba.size() != static_cast<usize>(bytes64))) {
        Fail(slot);
        return {};
    }
    SlotMeta& m = m_meta[slot];
    if (m.Kind() != AssetKind::None && m.Kind() != AssetKind::Texture) {
        Fail(slot);
        return {};
    }
    m.SetKind(AssetKind::Texture);
    if (!EnsurePayload(m)) {
        Fail(slot);
        return {};
    }

    const u8* stored = nullptr;
    if (!rgba.empty()) {
        auto copy = AllocBytes(rgba.size());
        if (copy.empty()) {
            Fail(slot);
            return {};
        }
        std::memcpy(copy.data(), rgba.data(), rgba.size());
        stored = copy.data();
    }

    const u16 ti = m.Payload();
    TextureRecord& prev = m_textures[ti];
    if (prev.width > 0U && prev.height > 0U) {
        const u64 old64 = static_cast<u64>(prev.width) * static_cast<u64>(prev.height) * 4ULL;
        const u32 old = static_cast<u32>(std::min<u64>(old64, 0xFFFFFFFFULL));
        m_vram = (m_vram >= old) ? (m_vram - old) : 0U;
        if (prev.pixels && prev.pixels != kWhitePx) FreeBytes(prev.pixels, static_cast<usize>(old64));
    }

    const TextureHandle g = gpu.IsValid() ? gpu : kWhiteTexture;
    prev = TextureRecord{.pixels = stored, .gpu = g, .width = w, .height = h};
    if (g != kWhiteTexture) PropagateGpu(slot, g);

    m_vram = static_cast<u32>(
        std::min<u64>(static_cast<u64>(m_vram) + bytes64, 0xFFFFFFFFULL));
    m.lastUsedFrame = m_frame;
    return TextureAssetHandle{slot, m.generation};
}

AtlasHandle AssetBank::CommitGrid(
    u16 slot, TextureAssetHandle tex, u16 cols, u16 rows, Vec2 pivot) noexcept {
    const u32 total = static_cast<u32>(cols) * static_cast<u32>(rows);
    if (slot == 0U || slot >= m_slots.high || m_meta[slot].assetId == 0ULL ||
        cols == 0U || rows == 0U || m_spriteHigh + total > m_maxSprites) {
        Fail(slot);
        return {};
    }
    SlotMeta& m = m_meta[slot];
    if (m.Kind() != AssetKind::None && m.Kind() != AssetKind::Atlas) {
        Fail(slot);
        return {};
    }
    m.SetKind(AssetKind::Atlas);
    if (!EnsurePayload(m)) {
        Fail(slot);
        return {};
    }

    const u16 ti = Resolve(tex, AssetKind::Texture);
    const u16 texMaster = (ti != 0U) ? tex.index : 0U;
    const TextureRecord& tr = m_textures[ti];
    const f32 invC = 1.0f / static_cast<f32>(cols);
    const f32 invR = 1.0f / static_cast<f32>(rows);
    const f32 cellW = static_cast<f32>(tr.width > 0U ? tr.width : cols) * invC;
    const f32 cellH = static_cast<f32>(tr.height > 0U ? tr.height : rows) * invR;

    const u16 first = m_spriteHigh;
    for (u16 r = 0U; r < rows; ++r) {
        for (u16 c = 0U; c < cols; ++c) {
            m_sprites[m_spriteHigh++] = SpriteFrame{
                .uv = Rect{static_cast<f32>(c) * invC, static_cast<f32>(r) * invR, invC, invR},
                .width = cellW,
                .height = cellH,
                .pivot = pivot,
                .gpuTexture = tr.gpu,
                .generation = m.generation,
            };
        }
    }

    m_atlases[m.Payload()] = AtlasRecord{
        .firstSprite = first,
        .spriteCount = static_cast<u16>(total),
        .texMasterSlot = texMaster,
    };
    m.lastUsedFrame = m_frame;
    return AtlasHandle{slot, m.generation};
}

BlobHandle AssetBank::CommitBlob(u16 slot, std::span<const u8> data) noexcept {
    if (slot == 0U || slot >= m_slots.high || m_meta[slot].assetId == 0ULL) {
        Fail(slot);
        return {};
    }
    SlotMeta& m = m_meta[slot];
    if (m.Kind() != AssetKind::None && m.Kind() != AssetKind::Blob) {
        Fail(slot);
        return {};
    }
    m.SetKind(AssetKind::Blob);
    if (!EnsurePayload(m)) {
        Fail(slot);
        return {};
    }

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

    BlobRecord& prev = m_blobs[m.Payload()];
    if (prev.data && prev.data != &kEmptyByte && prev.size > 0U) FreeBytes(prev.data, prev.size);
    prev = BlobRecord{.data = stored, .size = static_cast<u32>(data.size())};
    m.lastUsedFrame = m_frame;
    return BlobHandle{slot, m.generation};
}

AudioHandle AssetBank::CommitAudio(
    u16 slot, std::span<const f32> pcm, u32 rate, u8 ch, bool loop) noexcept {
    if (slot == 0U || slot >= m_slots.high || m_meta[slot].assetId == 0ULL || ch == 0U ||
        (!pcm.empty() && (pcm.size() % ch) != 0U)) {
        Fail(slot);
        return {};
    }
    SlotMeta& m = m_meta[slot];
    if (m.Kind() != AssetKind::None && m.Kind() != AssetKind::Audio) {
        Fail(slot);
        return {};
    }
    m.SetKind(AssetKind::Audio);
    if (!EnsurePayload(m)) {
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

    AudioRecord& prev = m_audio[m.Payload()];
    const usize prevBytes = static_cast<usize>(prev.TotalSamples()) * sizeof(f32);
    if (prev.pcm && prev.pcm != kSilence && prevBytes > 0U) FreeBytes(prev.pcm, prevBytes);

    prev = AudioRecord{
        .pcm = stored,
        .frameCount = static_cast<u32>(pcm.size() / ch),
        .sampleRate = static_cast<u16>(std::clamp(rate, 8000U, 65535U)),
        .channels = ch,
        .looping = loop,
    };
    m.lastUsedFrame = m_frame;
    return AudioHandle{slot, m.generation};
}

TextureAssetHandle AssetBank::LoadTexture(
    AssetId id, u8 scope, std::string_view png, std::string_view bmp,
    u16 w, u16 h, TextureHandle gpu) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Texture, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return TextureAssetHandle{slot, m_meta[slot].generation};

    const usize n = static_cast<usize>(w) * static_cast<usize>(h) * 4U;
    if (w == 0U || h == 0U || n == 0U) {
        Fail(slot);
        return {};
    }
    auto pixels = AllocBytes(n);
    if (pixels.empty() ||
        !ImageLoader::LoadRgbaImageFromDisk(png, bmp, w, h, pixels)) {
        if (!pixels.empty()) FreeBytes(pixels.data(), n);
        Fail(slot);
        return {};
    }

    SlotMeta& m = m_meta[slot];
    const u16 ti = m.Payload();
    const TextureHandle g = gpu.IsValid() ? gpu : kWhiteTexture;
    m_textures[ti] = TextureRecord{.pixels = pixels.data(), .gpu = g, .width = w, .height = h};
    if (g != kWhiteTexture) PropagateGpu(slot, g);
    m_vram = static_cast<u32>(
        std::min<u64>(static_cast<u64>(m_vram) + n, 0xFFFFFFFFULL));
    m.lastUsedFrame = m_frame;
    return TextureAssetHandle{slot, m.generation};
}

AtlasHandle AssetBank::RegisterGridAtlas(
    AssetId id, u8 scope, TextureAssetHandle tex, u16 cols, u16 rows, Vec2 pivot) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Atlas, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return AtlasHandle{slot, m_meta[slot].generation};
    return CommitGrid(slot, tex, cols, rows, pivot);
}

BlobHandle AssetBank::RegisterBlob(AssetId id, u8 scope, std::span<const u8> data) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Blob, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return BlobHandle{slot, m_meta[slot].generation};
    return CommitBlob(slot, data);
}

AudioHandle AssetBank::RegisterAudio(
    AssetId id, u8 scope, std::span<const f32> pcm, u32 rate, u8 ch, bool loop) noexcept {
    bool needs = false;
    const u16 slot = Acquire(id, AssetKind::Audio, scope, needs);
    if (slot == 0U) return {};
    if (!needs) return AudioHandle{slot, m_meta[slot].generation};
    return CommitAudio(slot, pcm, rate, ch, loop);
}

void AssetBank::EvictOne(u16 slot, std::span<TextureHandle> outGpu, u32& nGpu) noexcept {
    SlotMeta& m = m_meta[slot];
    const u16 pi = m.Payload();

    switch (m.Kind()) {
        case AssetKind::Texture:
            if (pi > 0U && pi < m_pools[kTex].high) {
                TextureRecord& t = m_textures[pi];
                if (t.width > 0U && t.height > 0U) {
                    const u64 b64 = static_cast<u64>(t.width) * static_cast<u64>(t.height) * 4ULL;
                    const u32 b = static_cast<u32>(std::min<u64>(b64, 0xFFFFFFFFULL));
                    m_vram = (m_vram >= b) ? (m_vram - b) : 0U;
                    if (t.pixels && t.pixels != kWhitePx) FreeBytes(t.pixels, static_cast<usize>(b64));
                }
                for (u16 a = 1U; a < m_pools[kAtl].high; ++a) {
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
                PoolFree(m_pools[kTex], t, &TextureRecord::width, pi);
            }
            break;
        case AssetKind::Atlas:
            if (pi > 0U && pi < m_pools[kAtl].high) {
                AtlasRecord& ar = m_atlases[pi];
                for (u16 s = 0U; s < ar.spriteCount; ++s) {
                    const u16 si = static_cast<u16>(ar.firstSprite + s);
                    if (si < m_spriteHigh) m_sprites[si].generation = 0U;
                }
                while (m_spriteHigh > 1U && m_sprites[m_spriteHigh - 1U].generation == 0U) {
                    --m_spriteHigh;
                }
                PoolFree(m_pools[kAtl], ar, &AtlasRecord::firstSprite, pi);
            }
            break;
        case AssetKind::Blob:
            if (pi > 0U && pi < m_pools[kBlob].high) {
                BlobRecord& b = m_blobs[pi];
                if (b.data && b.data != &kEmptyByte && b.size > 0U) FreeBytes(b.data, b.size);
                PoolFree(m_pools[kBlob], b, &BlobRecord::size, pi);
            }
            break;
        case AssetKind::Audio:
            if (pi > 0U && pi < m_pools[kAud].high) {
                AudioRecord& a = m_audio[pi];
                const usize bytes = static_cast<usize>(a.TotalSamples()) * sizeof(f32);
                if (a.pcm && a.pcm != kSilence && bytes > 0U) FreeBytes(a.pcm, bytes);
                PoolFree(m_pools[kAud], a, &AudioRecord::sampleRate, pi);
            }
            break;
        default:
            break;
    }

    if (m.assetId != 0ULL) {
        (void)m_idMap.Erase(m.assetId);
        m.assetId = 0ULL;
    }
    m.scopeMask = 0;
    m.lastUsedFrame = 0;
    m.payloadAndKind = m_slots.free;
    m_slots.free = slot;
    ++m_evicts;
}

u32 AssetBank::EvictUnpinned(std::span<TextureHandle> outGpu) noexcept {
    u32 n = 0;
    u32 nGpu = 0;
    for (u16 i = 1U; i < m_slots.high; ++i) {
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
    u32 nGpu = 0;
    u32 n = 0;
    while (m_vram > goal) {
        u16 oldest = 0;
        u16 maxAge = 0;
        for (u16 i = 1U; i < m_slots.high; ++i) {
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
