#include "assets/AssetBank.h"
#include "assets/AssetSystem.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace en::assets::tests {

namespace {

static_assert(alignof(AssetBank) == 64);
static_assert(alignof(AssetSystem) == 64);
static_assert(sizeof(AssetId) == 8);
static_assert(sizeof(SpriteFrame) == 32 && alignof(SpriteFrame) == 32);
static_assert(sizeof(TextureRecord) == 8 && alignof(TextureRecord) == 8);
static_assert(sizeof(AtlasRecord) == 8 && alignof(AtlasRecord) == 8);
static_assert(sizeof(BlobRecord) == 16 && alignof(BlobRecord) == 16);
static_assert(sizeof(AudioRecord) == 16 && alignof(AudioRecord) == 16);
static_assert(sizeof(SlotMeta) == 16 && alignof(SlotMeta) == 16);
static_assert(sizeof(PakHeader) == 32 && alignof(PakHeader) == 32);
static_assert(sizeof(PakEntry) == 32 && alignof(PakEntry) == 32);
static_assert(sizeof(CookedAtlasHeader) == 16);
static_assert(sizeof(CookedFrameName) == 8);

void TestAssetIdNormalization() {
    constexpr AssetId idA = "Textures\\Hero\\Idle_01.png"_aid;
    constexpr AssetId idB = "textures/hero/idle_01.png"_aid;
    static_assert(idA.IsValid());
    static_assert(idA == idB);
    static_assert(!AssetId::Hash("").IsValid());
}

void TestBankFallbacksAndLifecycle() {
    AssetBank bank{};
    auto initRes = bank.Init({
        .maxSlots = 64,
        .maxTextures = 16,
        .maxAtlases = 8,
        .maxBlobs = 16,
        .maxAudio = 16,
        .maxSprites = 128,
        .maxNamedFrames = 64,
        .blobArenaBytes = 4096,
        .vramBudgetBytes = 1024,
        .evictGraceFrames = 10
    });
    assert(initRes.has_value());

    // Invalid and unready handles must branchlessly resolve to Slot 0 fallbacks.
    const auto& fbTex = bank.GetTexture(TextureAssetHandle{});
    assert(fbTex.gpu == renderer::kWhiteTexture && fbTex.width == 1 && fbTex.height == 1);

    const auto& fbSprite = bank.GetSprite(SpriteHandle{});
    assert(fbSprite.gpuTexture == renderer::kWhiteTexture && fbSprite.width == 1.0f);

    const auto& fbBlob = bank.GetBlob(BlobHandle{});
    assert(fbBlob.data != nullptr && fbBlob.size == 64);

    const auto& fbAudio = bank.GetAudio(AudioHandle{});
    assert(fbAudio.pcm != nullptr && fbAudio.frameCount == 32 && fbAudio.sampleRate == 48000);

    auto scopeRes = bank.BeginScope();
    assert(scopeRes.has_value() && *scopeRes == 0);
    assert(bank.IsScopeSimReady(*scopeRes));

    bool needsLoad = false;
    auto slotRes = bank.AcquireSlot("data/config.bin"_aid, AssetKind::Blob, *scopeRes, Residency::Sim, needsLoad);
    assert(slotRes.has_value() && needsLoad);
    assert(!bank.IsScopeSimReady(*scopeRes));

    const u8 payload[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto commitRes = bank.CommitBlob(*slotRes, payload, 0xCAFEBABEu, false);
    assert(commitRes.has_value());
    assert(bank.IsScopeSimReady(*scopeRes));

    const BlobHandle bh = bank.ToBlobHandle(*slotRes);
    assert(bh.IsValid());
    const auto& bRec = bank.GetBlob(bh);
    assert(bRec.size == 16 && bRec.schemaHash == 0xCAFEBABEu);
    assert(std::memcmp(bRec.data, payload, 16) == 0);

    bank.EndScope(*scopeRes);
    bank.Shutdown();
}

void TestAtlasPropagationAndVramEviction() {
    AssetBank bank{};
    (void)bank.Init({
        .maxSlots = 64,
        .maxTextures = 16,
        .maxAtlases = 8,
        .maxBlobs = 16,
        .maxAudio = 16,
        .maxSprites = 64,
        .maxNamedFrames = 32,
        .blobArenaBytes = 4096,
        .vramBudgetBytes = 512,
        .evictGraceFrames = 5
    });

    const ScopeId scope = *bank.BeginScope();
    bool needsLoadTex = false;
    const u16 texSlot = *bank.AcquireSlot("tex/sheet.png"_aid, AssetKind::Texture, scope, Residency::Presentation, needsLoadTex);
    assert(needsLoadTex);

    bool needsLoadAtlas = false;
    const u16 atlasSlot = *bank.AcquireSlot("atlas/hero.eatlas"_aid, AssetKind::Atlas, scope, Residency::Presentation, needsLoadAtlas);
    assert(needsLoadAtlas);

    std::vector<u8> atlasBytes(sizeof(CookedAtlasHeader) + 2 * sizeof(SpriteFrame) + sizeof(CookedFrameName), 0);
    CookedAtlasHeader hdr{
        .backingTextureId = ("tex/sheet.png"_aid).value,
        .spriteCount = 2,
        .namedFrameCount = 1,
        .reserved = 0
    };
    std::memcpy(atlasBytes.data(), &hdr, sizeof(hdr));

    SpriteFrame frames[2]{};
    frames[0].width = 32.0f;
    frames[0].height = 48.0f;
    frames[1].width = 64.0f;
    frames[1].height = 64.0f;
    std::memcpy(atlasBytes.data() + sizeof(hdr), frames, sizeof(frames));

    CookedFrameName named{.nameHash = 0x12345678u, .frameOffset = 1, .reserved = 0};
    std::memcpy(atlasBytes.data() + sizeof(hdr) + sizeof(frames), &named, sizeof(named));

    assert(bank.CommitAtlas(atlasSlot, texSlot, atlasBytes).has_value());

    const AtlasHandle ah = bank.ToAtlasHandle(atlasSlot);
    const SpriteHandle sh1 = bank.GetSpriteHandle(ah, 1);
    assert(bank.FindNamedFrame(ah, 0x12345678u) == 1);
    assert(bank.GetSprite(sh1).gpuTexture == renderer::kWhiteTexture);

    const renderer::TextureHandle resolvedGpu{7, 3};
    assert(bank.CommitTexture(texSlot, resolvedGpu, 256, 256, 1024).has_value());
    assert(bank.GetSprite(sh1).gpuTexture == resolvedGpu);
    assert(bank.GetAtlasFrame(ah, 0).gpuTexture == resolvedGpu);

    bank.AdvanceFrame(10);
    bank.EndScope(scope);
    bank.AdvanceFrame(20);

    renderer::TextureHandle destroyed[4]{};
    const u32 evicted = bank.EvictUnpinned(destroyed);
    assert(evicted == 1 && destroyed[0] == resolvedGpu);
    assert(bank.CurrentVramBytes() == 0);
    assert(bank.GetAtlasFrame(ah, 1).gpuTexture == renderer::kWhiteTexture);

    // Re-acquiring and committing the atlas must reuse the sprite slice without growing sprite count.
    const ScopeId scope2 = *bank.BeginScope();
    const u16 texSlot2 = *bank.AcquireSlot("tex/sheet.png"_aid, AssetKind::Texture, scope2, Residency::Presentation, needsLoadTex);
    const u16 atlasSlot2 = *bank.AcquireSlot("atlas/hero.eatlas"_aid, AssetKind::Atlas, scope2, Residency::Presentation, needsLoadAtlas);
    assert(needsLoadTex && needsLoadAtlas);
    assert(bank.CommitTexture(texSlot2, renderer::TextureHandle{8, 1}, 256, 256, 512).has_value());
    assert(bank.CommitAtlas(atlasSlot2, texSlot2, atlasBytes).has_value());

    const AtlasHandle ah2 = bank.ToAtlasHandle(atlasSlot2);
    assert(ah2.generation == 2);
    assert(bank.GetSpriteHandle(ah2, 0).spriteIndex == 1);
    bank.Shutdown();
}

void TestLz4Decompressor() {
    const u8 compressed[] = {0x44, 'A', 'B', 'C', 'D', 0x04, 0x00};
    u8 decompressed[12]{};
    auto res = AssetBank::DecompressLz4(compressed, decompressed);
    assert(res.has_value() && *res == 12);
    assert(std::memcmp(decompressed, "ABCDABCDABCD", 12) == 0);

    // Corrupted offset=0 must fail cleanly without out-of-bounds read.
    const u8 badStream[] = {0x44, 'A', 'B', 'C', 'D', 0x00, 0x00};
    assert(!AssetBank::DecompressLz4(badStream, decompressed).has_value());
}

void TestAssetSystemMemoryMountsBudgetAndMove() {
    auto buildPak = [](AssetId id, AssetKind kind, AssetCodec codec, std::span<const u8> payload, u32 rawSize, u32 metaA) {
        std::vector<u8> pak(sizeof(PakHeader) + 64 + sizeof(PakEntry), 0);
        PakHeader hdr{
            .magic = kPakMagic,
            .version = kPakVersion,
            .entryCount = 1,
            .flags = 0,
            .tocOffset = sizeof(PakHeader) + 64,
            .contentHash = 0
        };
        PakEntry entry{
            .assetId = id.value,
            .offset = sizeof(PakHeader),
            .storedSize = static_cast<u32>(payload.size()),
            .rawSize = rawSize,
            .metaA = metaA,
            .metaB = 0,
            .kind = kind,
            .codec = codec
        };
        std::memcpy(pak.data(), &hdr, sizeof(hdr));
        std::memcpy(pak.data() + sizeof(PakHeader), payload.data(), payload.size());
        std::memcpy(pak.data() + hdr.tocOffset, &entry, sizeof(entry));
        return pak;
    };

    const u8 baseData[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    const u8 patchData[8] = {99, 98, 97, 96, 95, 94, 93, 92};
    const auto basePak = buildPak("cfg/balance.bin"_aid, AssetKind::Blob, AssetCodec::Raw, baseData, 8, 111u);
    const auto patchPak = buildPak("cfg/balance.bin"_aid, AssetKind::Blob, AssetCodec::Raw, patchData, 8, 222u);

    const u8 lz4Stream[] = {0x44, 'A', 'B', 'C', 'D', 0x04, 0x00};
    const auto lz4Pak = buildPak("cfg/compressed.bin"_aid, AssetKind::Blob, AssetCodec::Lz4, lz4Stream, 12, 333u);

    const u8 texLz4[] = {0x48, 0xFF, 0x00, 0xFF, 0xFF, 0x04, 0x00}; // 4 literals + 12 match = 16 bytes (2x2 RGBA8)
    const auto texPak1 = buildPak("tex/t1.png"_aid, AssetKind::Texture, AssetCodec::Lz4, texLz4, 16, (2u << 16) | 2u);
    const auto texPak2 = buildPak("tex/t2.png"_aid, AssetKind::Texture, AssetCodec::Lz4, texLz4, 16, (2u << 16) | 2u);

    AssetSystem sysInit{};
    assert(sysInit.Init({.stagingRingBytes = 64}).has_value());
    assert(sysInit.MountPackMemory(basePak, 0).has_value());
    assert(sysInit.MountPackMemory(patchPak, 10).has_value());
    assert(sysInit.MountPackMemory(lz4Pak, 5).has_value());
    assert(sysInit.MountPackMemory(texPak1, 1).has_value());
    assert(sysInit.MountPackMemory(texPak2, 1).has_value());

    // Move construction must preserve all mounted packs, arenas, and state.
    AssetSystem sys = std::move(sysInit);
    const ScopeId scope = *sys.BeginScope();

    const auto bh = sys.PinBlob(scope, "cfg/balance.bin"_aid, Residency::Sim);
    assert(bh.has_value());
    const auto& bRec = sys.Bank().GetBlob(*bh);
    assert(bRec.schemaHash == 222u && bRec.data[0] == 99);

    // Pin two LZ4 textures and one LZ4 blob; throttle texture upload budget to 1 per Pump().
    const auto th1 = sys.PinTexture(scope, "tex/t1.png"_aid);
    const auto th2 = sys.PinTexture(scope, "tex/t2.png"_aid);
    const auto lz4Slot = sys.Pin(scope, "cfg/compressed.bin"_aid, AssetKind::Blob, Residency::Sim);
    assert(th1.has_value() && th2.has_value() && lz4Slot.has_value());

    // First pump with maxGpuUploads = 1 must upload th1 and still finalize the younger LZ4 blob without starving it!
    sys.PumpHeadless(PumpBudget{.maxGpuUploads = 1, .maxUploadBytes = 1024});
    assert(sys.Bank().GetTexture(*th1).width == 2);
    assert(sys.Bank().GetTexture(*th2).width == 1); // Still fallback
    assert(sys.Bank().IsScopeSimReady(scope));

    // Second pump finishes th2.
    sys.PumpHeadless(PumpBudget{.maxGpuUploads = 1, .maxUploadBytes = 1024});
    assert(sys.Bank().GetTexture(*th2).width == 2);

    const u64 auditA = sys.Bank().ComputeAuditHash();
    assert(auditA != 0);
    sys.Shutdown();
}

} // namespace

} // namespace en::assets::tests

int main() {
    en::assets::tests::TestAssetIdNormalization();
    en::assets::tests::TestBankFallbacksAndLifecycle();
    en::assets::tests::TestAtlasPropagationAndVramEviction();
    en::assets::tests::TestLz4Decompressor();
    en::assets::tests::TestAssetSystemMemoryMountsBudgetAndMove();
    std::puts("All EngineN Phase 5 (en::assets) tests passed.");
    return 0;
}
