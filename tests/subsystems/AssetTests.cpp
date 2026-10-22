#include "assets/AssetSystem.h"
#include "platform/Window.h"
#include "renderer/Renderer.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace en::tests {

[[nodiscard]] constexpr u32 HashName32(std::string_view name) noexcept {
    return static_cast<u32>(AssetId::Hash(name).value);
}

[[nodiscard]] constexpr bool VerifyConstexprAssetsLayer1() noexcept {
    // 1. Verify exact byte layouts and zero-padding invariants
    static_assert(sizeof(AssetId) == 8 && alignof(AssetId) == 8);
    static_assert(sizeof(SpriteHandle) == 4 && alignof(SpriteHandle) == 4);
    static_assert(sizeof(TextureAssetHandle) == 4 && alignof(TextureAssetHandle) == 4);
    static_assert(sizeof(AtlasHandle) == 4 && alignof(AtlasHandle) == 4);
    static_assert(sizeof(BlobHandle) == 4 && alignof(BlobHandle) == 4);
    static_assert(sizeof(AudioHandle) == 4 && alignof(AudioHandle) == 4);
    static_assert(sizeof(UnitPivotU16) == 2 && alignof(UnitPivotU16) == 2);
    static_assert(sizeof(SpriteFrame) == 32 && alignof(SpriteFrame) == 32);
    static_assert(sizeof(TextureRecord) == 16 && alignof(TextureRecord) == 16);
    static_assert(sizeof(AtlasRecord) == 8 && alignof(AtlasRecord) == 8);
    static_assert(sizeof(BlobRecord) == 16 && alignof(BlobRecord) == 16);
    static_assert(sizeof(AudioRecord) == 16 && alignof(AudioRecord) == 16);
    static_assert(sizeof(SlotMeta) == 16 && alignof(SlotMeta) == 16);
    static_assert(sizeof(EnPakHeader) == 16 && alignof(EnPakHeader) == 16);
    static_assert(sizeof(EnPakEntry) == 32 && alignof(EnPakEntry) == 8);
    static_assert(sizeof(CookedAtlasSpriteEntry) == 32 && alignof(CookedAtlasSpriteEntry) == 8);
    static_assert(sizeof(AssetBank::FreePayloadBlock) == 16 && alignof(AssetBank::FreePayloadBlock) == 16);
    static_assert(sizeof(AssetBank) == 256 && alignof(AssetBank) == 64);
    static_assert(sizeof(AssetSystem::VfsTocSlot) == 48 && alignof(AssetSystem::VfsTocSlot) == 8);
    static_assert(sizeof(AssetSystem::PendingJob) == 16 && alignof(AssetSystem::PendingJob) == 16);

    // 2. Verify constexpr AssetId hashing, StringId conversion & EnPakEntry BackingTexture packing
    constexpr AssetId kHeroTex = "textures/hero.png"_aid;
    static_assert(kHeroTex.IsValid());
    static_assert(kHeroTex == "textures/hero.png"_aid);
    static_assert(kHeroTex != "textures/boss.png"_aid);
    constexpr StringId kSid = kHeroTex;
    static_assert(AssetId{kSid} == kHeroTex);

    EnPakEntry testEntry{};
    testEntry.SetBackingTexture(kHeroTex);
    if (testEntry.BackingTexture() != kHeroTex) {
        return false;
    }

    constexpr SpriteHandle kSprite{7U, 3U};
    static_assert(kSprite.IsValid());
    static_assert(kSprite.index == 7U);
    static_assert(kSprite.generation == 3U);

    // 3. Verify UnitPivotU16 quantization and SpriteFrame::Pivot()
    constexpr UnitPivotU16 kHalf{0.5f};
    constexpr f32 kDecoded = static_cast<f32>(kHalf);
    if (kDecoded < 0.4999f || kDecoded > 0.5001f) {
        return false;
    }

    constexpr SpriteFrame kFrame{
        .uv         = {0.0f, 0.0f, 0.5f, 0.5f},
        .width      = 32.0f,
        .height     = 64.0f,
        .pivotX     = UnitPivotU16{0.0f},
        .pivotY     = UnitPivotU16{1.0f},
        .gpuTexture = TextureHandle{1U, 1U},
    };
    constexpr Vec2 kPivot = kFrame.Pivot();
    if (kPivot.x != 0.0f || kPivot.y < 0.9999f || kPivot.y > 1.0001f) {
        return false;
    }

    return true;
}

static_assert(VerifyConstexprAssetsLayer1(), "Compile-time verification of Phase 6 Assets Layer 1 failed.");

struct TestSuite {
    u32 passed{0};
    u32 failed{0};

    void Check(bool condition, const char* expr, const char* file, int line) noexcept {
        if (condition) {
            ++passed;
        } else {
            ++failed;
            std::printf("[FAIL] %s:%d: %s\n", file, line, expr);
        }
    }
};

#define EXPECT_TRUE(suite, cond) (suite).Check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define EXPECT_EQ(suite, a, b)   (suite).Check((a) == (b), #a " == " #b, __FILE__, __LINE__)

void TestLz4CompressionAndSafety(TestSuite& suite) noexcept {
    // 1. Repetitive data compression ratio < 1.0 + exact round-trip
    u8 src[1024]{};
    for (usize i = 0; i < sizeof(src); ++i) {
        src[i] = static_cast<u8>((i % 16U) + 0x20U);
    }

    u8 compressed[1200]{};
    const auto compRes = AssetBank::CompressLz4(
        std::span<const u8>(src, sizeof(src)),
        std::span<u8>(compressed, sizeof(compressed)));
    EXPECT_TRUE(suite, compRes.has_value());
    const usize compLen = compRes.value_or(0U);
    EXPECT_TRUE(suite, compLen > 0U);
    EXPECT_TRUE(suite, compLen < sizeof(src) / 4U);

    u8 decompressed[1024]{};
    const auto decompRes = AssetBank::DecompressLz4(
        std::span<const u8>(compressed, compLen),
        std::span<u8>(decompressed, sizeof(decompressed)));
    EXPECT_TRUE(suite, decompRes.has_value());
    EXPECT_EQ(suite, decompRes.value_or(0U), sizeof(src));
    EXPECT_EQ(suite, std::memcmp(src, decompressed, sizeof(src)), 0);

    // 2. Overlapping RLE match (offset == 1)
    u8 rleSrc[256]{};
    std::memset(rleSrc, 0xAB, sizeof(rleSrc));
    const auto rleCompRes = AssetBank::CompressLz4(
        std::span<const u8>(rleSrc, sizeof(rleSrc)),
        std::span<u8>(compressed, sizeof(compressed)));
    EXPECT_TRUE(suite, rleCompRes.has_value());
    const usize rleCompLen = rleCompRes.value_or(0U);
    EXPECT_TRUE(suite, rleCompLen > 0U && rleCompLen < 32U);

    std::memset(decompressed, 0, sizeof(decompressed));
    const auto rleDecompRes = AssetBank::DecompressLz4(
        std::span<const u8>(compressed, rleCompLen),
        std::span<u8>(decompressed, sizeof(rleSrc)));
    EXPECT_TRUE(suite, rleDecompRes.has_value());
    EXPECT_EQ(suite, rleDecompRes.value_or(0U), sizeof(rleSrc));
    EXPECT_EQ(suite, std::memcmp(rleSrc, decompressed, sizeof(rleSrc)), 0);

    // 3. Corrupted stream (zero offset / out-of-bounds offset) must fail safely
    const u8 corruptedZeroOffset[] = {0x00, 0x00, 0x00};
    EXPECT_TRUE(suite, !AssetBank::DecompressLz4(
        std::span<const u8>(corruptedZeroOffset, sizeof(corruptedZeroOffset)),
        std::span<u8>(decompressed, sizeof(decompressed))).has_value());

    const u8 corruptedBigOffset[] = {0x10, 0x41, 0x05, 0x00};
    EXPECT_TRUE(suite, !AssetBank::DecompressLz4(
        std::span<const u8>(corruptedBigOffset, sizeof(corruptedBigOffset)),
        std::span<u8>(decompressed, sizeof(decompressed))).has_value());
}

// Helper to build a cooked .enpak archive in memory
usize BuildTestEnPak(
    std::span<u8> outBuf,
    std::span<const EnPakEntry> entries,
    std::span<const std::span<const u8>> payloads) noexcept {
    const u32 count = static_cast<u32>(entries.size());
    const usize tocOffset = sizeof(EnPakHeader);
    const usize tocBytes  = static_cast<usize>(count) * sizeof(EnPakEntry);
    usize cursor          = (tocOffset + tocBytes + 15U) & ~usize{15U};

    EnPakEntry localEntries[16]{};
    for (u32 i = 0; i < count; ++i) {
        localEntries[i]                = entries[i];
        localEntries[i].offset         = static_cast<u32>(cursor);
        localEntries[i].compressedSize = static_cast<u32>(payloads[i].size());
        std::memcpy(outBuf.data() + cursor, payloads[i].data(), payloads[i].size());
        cursor = (cursor + payloads[i].size() + 15U) & ~usize{15U};
    }

    std::memcpy(outBuf.data() + tocOffset, localEntries, tocBytes);

    EnPakHeader hdr{};
    hdr.magic       = kEnPakMagic;
    hdr.version     = 1U;
    hdr.entryCount  = count;
    hdr.headerCrc32 = AssetSystem::ComputeTocCrc32(std::span<const EnPakEntry>(localEntries, count));
    std::memcpy(outBuf.data(), &hdr, sizeof(EnPakHeader));
    return cursor;
}

void TestAssetBankDirectLifecycleCoalescingAndGridAtlas(TestSuite& suite) noexcept {
    AssetBank bank;
    AssetBankConfig cfg{};
    cfg.arenaBytes      = 512U * 1024U;
    cfg.maxSlots        = 32;
    cfg.maxTextures     = 16;
    cfg.maxAtlases      = 8;
    cfg.maxSprites      = 64;
    cfg.maxBlobs        = 16;
    cfg.maxAudio        = 8;
    cfg.vramBudgetBytes = 64; // Tiny 64B VRAM budget to trigger LRU eviction
    EXPECT_TRUE(suite, bank.Init(cfg).has_value());

    const u32 baseArenaBytes = bank.ArenaUsedBytes();
    const auto scopeRes = bank.BeginScope();
    EXPECT_TRUE(suite, scopeRes.has_value());
    const u8 scope = scopeRes.value_or(kInvalidScope);

    // Allocate textures (each 4x4 RGBA8 = 64 bytes) at different frames
    const u32 pixelsA[16] = {0xFF0000FFU, 0xFF00FF00U};
    const u32 pixelsB[16] = {0xFFFF0000U, 0xFFFFFFFFU};
    const u32 pixelsC[16] = {0xFF112233U, 0xFF445566U};

    bank.AdvanceFrame(10);
    bool needsLoadA = false;
    const u16 slotA = bank.AcquireSlot("tex/a"_aid, AssetKind::Texture, scope, AssetResidency::Sim, needsLoadA);
    EXPECT_TRUE(suite, needsLoadA);
    const TextureAssetHandle hTexA = bank.CommitTexture(
        slotA, 4, 4, std::span<const u8>(reinterpret_cast<const u8*>(pixelsA), sizeof(pixelsA)));
    EXPECT_TRUE(suite, hTexA.IsValid());
    const u32 arenaAfterA = bank.ArenaUsedBytes();
    EXPECT_EQ(suite, bank.CurrentVramBytes(), 64U);
    EXPECT_TRUE(suite, !bank.IsOverVramBudget());

    // Test procedural CommitGridAtlas (2x2 grid over 4x4 texture -> 4 frames of 2x2 pixels each)
    bool needsLoadGrid = false;
    const u16 slotGrid = bank.AcquireSlot("atlas/grid"_aid, AssetKind::Atlas, scope, AssetResidency::Sim, needsLoadGrid);
    EXPECT_TRUE(suite, needsLoadGrid);
    const AtlasHandle hGrid = bank.CommitGridAtlas(slotGrid, hTexA, 2U, 2U, {0.5f, 0.5f});
    EXPECT_TRUE(suite, hGrid.IsValid());
    const SpriteFrame& gridFrame3 = bank.GetAtlasFrame(hGrid, 3U);
    EXPECT_EQ(suite, gridFrame3.width, 2.0f);
    EXPECT_EQ(suite, gridFrame3.height, 2.0f);
    EXPECT_EQ(suite, gridFrame3.uv.x, 0.5f);
    EXPECT_EQ(suite, gridFrame3.uv.y, 0.5f);

    bank.AdvanceFrame(20);
    bool needsLoadB = false;
    const u16 slotB = bank.AcquireSlot("tex/b"_aid, AssetKind::Texture, scope, AssetResidency::Sim, needsLoadB);
    EXPECT_TRUE(suite, needsLoadB);
    const TextureAssetHandle hTexB = bank.CommitTexture(
        slotB, 4, 4, std::span<const u8>(reinterpret_cast<const u8*>(pixelsB), sizeof(pixelsB)));
    EXPECT_TRUE(suite, hTexB.IsValid());
    const u32 arenaAfterB = bank.ArenaUsedBytes();
    EXPECT_EQ(suite, bank.CurrentVramBytes(), 128U);
    EXPECT_TRUE(suite, bank.IsOverVramBudget());

    // Touch Texture A on frame 30 so Texture B becomes the oldest LRU texture (frame 20)
    bank.AdvanceFrame(30);
    bank.TouchAsset("tex/a"_aid);

    // End scope so A, B, and Grid become unpinned
    bank.EndScope(scope);

    // Evict down to 64B VRAM budget: Texture B (lastUsedFrame = 20) must be evicted first, leaving Texture A (lastUsedFrame = 30)
    // Because Texture B was at the top of m_arena, FreePayloadSlice rewinds m_arena back to arenaAfterA!
    TextureHandle destroyedGpu[4]{};
    const u32 lruEvicted = bank.EvictLruToVramBudget(std::span<TextureHandle>(destroyedGpu, 4), 64U);
    EXPECT_EQ(suite, lruEvicted, 1U);
    EXPECT_EQ(suite, bank.ArenaUsedBytes(), arenaAfterA);
    EXPECT_EQ(suite, bank.CurrentVramBytes(), 64U);
    EXPECT_TRUE(suite, !bank.IsOverVramBudget());
    EXPECT_EQ(suite, bank.GetTexture(hTexA).width, 4U);
    EXPECT_EQ(suite, bank.GetTexture(hTexB).width, 1U); // Fallback slot 0 (1x1 white texture)

    // Re-allocate a new texture C in scope2: intrusive freelist must reuse Texture B's texture slot with incremented ABA generation
    const u8 scope2 = bank.BeginScope().value_or(kInvalidScope);
    bool needsLoadC = false;
    const u16 slotC = bank.AcquireSlot("tex/c"_aid, AssetKind::Texture, scope2, AssetResidency::Sim, needsLoadC);
    EXPECT_TRUE(suite, needsLoadC);
    const TextureAssetHandle hTexC = bank.CommitTexture(
        slotC, 4, 4, std::span<const u8>(reinterpret_cast<const u8*>(pixelsC), sizeof(pixelsC)));
    EXPECT_TRUE(suite, hTexC.IsValid());
    EXPECT_EQ(suite, bank.ArenaUsedBytes(), arenaAfterB);
    EXPECT_EQ(suite, hTexC.index, hTexB.index);
    EXPECT_EQ(suite, hTexC.generation, static_cast<u16>(hTexB.generation + 1U));
    EXPECT_EQ(suite, bank.GetTexture(hTexB).width, 1U); // Stale handle falls back to slot 0
    EXPECT_EQ(suite, bank.GetTexture(hTexC).width, 4U);

    // Evict unpinned Texture A (which sits in the middle of m_arena below Texture C)
    // and verify a subsequent 64B Blob allocation reuses Texture A's mid-arena FreePayloadBlock without growing m_arena!
    EXPECT_EQ(suite, bank.EvictUnpinned(), 2U); // Evicts unpinned Texture A + Grid Atlas
    EXPECT_EQ(suite, bank.ArenaUsedBytes(), arenaAfterB);
    bool needsLoadBlob = false;
    const u16 slotMidBlob = bank.AcquireSlot("blob/mid"_aid, AssetKind::Blob, scope2, AssetResidency::Sim, needsLoadBlob);
    const BlobHandle hMidBlob = bank.CommitBlob(
        slotMidBlob, std::span<const u8>(reinterpret_cast<const u8*>(pixelsA), sizeof(pixelsA)));
    EXPECT_TRUE(suite, hMidBlob.IsValid());
    EXPECT_EQ(suite, bank.ArenaUsedBytes(), arenaAfterB);

    // Evict all remaining assets and verify m_arena rewinds all the way back to baseArenaBytes!
    bank.EndScope(scope2);
    const u32 restEvicted = bank.EvictUnpinned();
    EXPECT_EQ(suite, restEvicted, 2U);
    EXPECT_EQ(suite, bank.ArenaUsedBytes(), baseArenaBytes);

    bank.Shutdown();
}

void TestAssetSystemVfsMountStreamingAndGpuUpload(TestSuite& suite) noexcept {
    // 1. Build base pack (priority 0) with:
    //    - compressed texture ("tex/hero")
    //    - cooked sprite atlas ("atlas/hero") with 2 sprites ("idle", "run") and automatic SetBackingTexture("tex/hero"_aid)
    //    - encrypted + LZ4-compressed blob ("blob/config")
    //    - audio clip ("sfx/jump")
    u32 rawPixels[16]{};
    for (u32 i = 0; i < 16; ++i) {
        rawPixels[i] = 0xFF00FF00U + i;
    }
    u8 compTexBuf[128]{};
    const usize compTexSize = AssetBank::CompressLz4(
        std::span<const u8>(reinterpret_cast<const u8*>(rawPixels), sizeof(rawPixels)),
        std::span<u8>(compTexBuf, sizeof(compTexBuf))).value_or(0U);
    EXPECT_TRUE(suite, compTexSize > 0U);

    CookedAtlasSpriteEntry atlasSprites[2]{};
    atlasSprites[0].u0       = 0.0f;
    atlasSprites[0].v0       = 0.0f;
    atlasSprites[0].u1       = 0.5f;
    atlasSprites[0].v1       = 1.0f;
    atlasSprites[0].width    = 16.0f;
    atlasSprites[0].height   = 32.0f;
    atlasSprites[0].pivotX   = UnitPivotU16{0.5f};
    atlasSprites[0].pivotY   = UnitPivotU16{1.0f};
    atlasSprites[0].nameHash = HashName32("idle");

    atlasSprites[1].u0       = 0.5f;
    atlasSprites[1].v0       = 0.0f;
    atlasSprites[1].u1       = 1.0f;
    atlasSprites[1].v1       = 1.0f;
    atlasSprites[1].width    = 20.0f;
    atlasSprites[1].height   = 32.0f;
    atlasSprites[1].pivotX   = UnitPivotU16{0.5f};
    atlasSprites[1].pivotY   = UnitPivotU16{1.0f};
    atlasSprites[1].nameHash = HashName32("run");

    const u32 configSecret[8] = {100U, 200U, 300U, 400U, 100U, 200U, 300U, 400U};
    u8 encryptedBlob[128]{};
    const usize compBlobSize = AssetBank::CompressLz4(
        std::span<const u8>(reinterpret_cast<const u8*>(configSecret), sizeof(configSecret)),
        std::span<u8>(encryptedBlob, sizeof(encryptedBlob))).value_or(0U);
    EXPECT_TRUE(suite, compBlobSize > 0U);
    constexpr AssetId kBlobId = "blob/config"_aid;
    AssetSystem::XorCipherPayload(std::span<u8>(encryptedBlob, compBlobSize), kBlobId.value);

    const f32 pcmSamples[8] = {0.25f, -0.25f, 0.5f, -0.5f, 0.75f, -0.75f, 1.0f, -1.0f};

    EnPakEntry baseEntries[4]{};
    baseEntries[0].assetId          = ("tex/hero"_aid).value;
    baseEntries[0].uncompressedSize = sizeof(rawPixels);
    baseEntries[0].widthOrRate      = 4U;
    baseEntries[0].heightOrChannels = 4U;
    baseEntries[0].kind             = AssetKind::Texture;
    baseEntries[0].compressed       = 1U; // LZ4

    baseEntries[1].assetId          = ("atlas/hero"_aid).value;
    baseEntries[1].uncompressedSize = sizeof(atlasSprites);
    baseEntries[1].kind             = AssetKind::Atlas;
    baseEntries[1].compressed       = 0U;
    baseEntries[1].SetBackingTexture("tex/hero"_aid);

    baseEntries[2].assetId          = kBlobId.value;
    baseEntries[2].uncompressedSize = sizeof(configSecret);
    baseEntries[2].kind             = AssetKind::Blob;
    baseEntries[2].compressed       = 2U; // Encrypted + LZ4

    baseEntries[3].assetId          = ("sfx/jump"_aid).value;
    baseEntries[3].uncompressedSize = sizeof(pcmSamples);
    baseEntries[3].schemaOrMeta     = 1U;     // looping = true
    baseEntries[3].widthOrRate      = 48000U; // sampleRate
    baseEntries[3].heightOrChannels = 2U;     // channels (stereo)
    baseEntries[3].kind             = AssetKind::Audio;
    baseEntries[3].compressed       = 0U;

    const std::span<const u8> basePayloads[4] = {
        std::span<const u8>(compTexBuf, compTexSize),
        std::span<const u8>(reinterpret_cast<const u8*>(atlasSprites), sizeof(atlasSprites)),
        std::span<const u8>(encryptedBlob, compBlobSize),
        std::span<const u8>(reinterpret_cast<const u8*>(pcmSamples), sizeof(pcmSamples)),
    };

    alignas(16) u8 basePak[2048]{};
    const usize basePakBytes = BuildTestEnPak(
        std::span<u8>(basePak, sizeof(basePak)),
        std::span<const EnPakEntry>(baseEntries, 4),
        std::span<const std::span<const u8>>(basePayloads, 4));

    // 2. Build patch pack (priority 10) overriding "blob/config"
    const u32 patchedSecret[8] = {999U, 888U, 777U, 666U, 555U, 444U, 333U, 222U};
    EnPakEntry patchEntries[1]{};
    patchEntries[0].assetId          = kBlobId.value;
    patchEntries[0].uncompressedSize = sizeof(patchedSecret);
    patchEntries[0].kind             = AssetKind::Blob;
    patchEntries[0].compressed       = 0U;

    const std::span<const u8> patchPayloads[1] = {
        std::span<const u8>(reinterpret_cast<const u8*>(patchedSecret), sizeof(patchedSecret)),
    };
    alignas(16) u8 patchPak[512]{};
    const usize patchPakBytes = BuildTestEnPak(
        std::span<u8>(patchPak, sizeof(patchPak)),
        std::span<const EnPakEntry>(patchEntries, 1),
        std::span<const std::span<const u8>>(patchPayloads, 1));

    // Write basePak to disk to test on-demand MountPackFile via SDL_IOStream
    const char* kTempPakPath = "/tmp/enginen_test_base.enpak";
    if (FILE* f = std::fopen(kTempPakPath, "wb")) {
        std::fwrite(basePak, 1, basePakBytes, f);
        std::fclose(f);
    }

    // 3. Initialize AssetSystem & Renderer
    AssetSystem sys;
    AssetSystemConfig sysCfg{};
    sysCfg.bank.arenaBytes      = 512U * 1024U;
    sysCfg.bank.maxSlots        = 64;
    sysCfg.bank.maxTextures     = 32;
    sysCfg.bank.maxAtlases      = 16;
    sysCfg.bank.maxSprites      = 64;
    sysCfg.bank.maxBlobs        = 32;
    sysCfg.bank.maxAudio        = 16;
    sysCfg.bank.vramBudgetBytes = 64U * 1024U;
    sysCfg.vfsArenaBytes        = 64U * 1024U; // Tiny 64KB VFS arena proves MountPackFile uses 0 persistent archive bytes!
    sysCfg.maxVfsEntries        = 64;
    EXPECT_TRUE(suite, sys.Init(sysCfg).has_value());

    EXPECT_TRUE(suite, sys.MountPackFile(kTempPakPath, 0).has_value());
    EXPECT_TRUE(suite, sys.MountPackMemory(std::span<const u8>(patchPak, patchPakBytes), 10, false).has_value());
    EXPECT_EQ(suite, sys.MountCount(), 2U);

    Window win{};
    (void)win.Create(WindowConfig{}, true);
    auto rnd = std::make_unique<Renderer>();
    RendererConfig rndCfg{};
    rndCfg.headless = false;
    EXPECT_TRUE(suite, rnd->Init(win, rndCfg).has_value());

    const u8 gameplayScope = sys.BeginScope().value_or(kInvalidScope);
    EXPECT_TRUE(suite, gameplayScope != kInvalidScope);

    // 4. Stream Atlas WITHOUT manually pinning Texture first: PinAtlas({}, ...) auto-pins "tex/hero"_aid!
    const AtlasHandle hHeroAtlas = sys.PinAtlas(
        gameplayScope, "atlas/hero"_aid, {}, AssetResidency::Stream);
    const TextureAssetHandle hHeroTex = sys.Bank().FindTextureHandle("tex/hero"_aid);
    const BlobHandle hConfigBlob = sys.PinBlob(
        gameplayScope, kBlobId, AssetResidency::Stream);
    const AudioHandle hJumpSfx = sys.PinAudio(
        gameplayScope, "sfx/jump"_aid, AssetResidency::Stream);

    EXPECT_TRUE(suite, hHeroTex.IsValid());
    EXPECT_TRUE(suite, hHeroAtlas.IsValid());
    EXPECT_TRUE(suite, hConfigBlob.IsValid());
    EXPECT_TRUE(suite, hJumpSfx.IsValid());

    // Before Pump(), Gameplay simulation scope is not yet ready and queries return Slot 0 fallbacks
    EXPECT_TRUE(suite, !sys.Bank().IsScopeSimReady(gameplayScope));
    EXPECT_EQ(suite, sys.PendingJobCount(), 4U);
    EXPECT_EQ(suite, sys.Bank().GetTexture(hHeroTex).width, 1U);
    EXPECT_EQ(suite, sys.Bank().GetAtlasFrame(hHeroAtlas, 0U).width, 1.0f);

    // Pump 2 jobs first, then WaitSimReady for remaining jobs + GPU upload
    sys.Pump(rnd.get(), 2U);
    EXPECT_EQ(suite, sys.PendingJobCount(), 2U);
    EXPECT_TRUE(suite, sys.WaitSimReady(gameplayScope, rnd.get()));
    EXPECT_TRUE(suite, sys.Bank().IsScopeSimReady(gameplayScope));
    EXPECT_EQ(suite, sys.PendingJobCount(), 0U);
    EXPECT_EQ(suite, sys.Bank().PendingGpuTextureCount(), 0U);

    // 5. Verify Texture & automatic GPU upload propagation to Atlas sprites using the SAME streaming handles!
    const TextureRecord& texRec = sys.Bank().GetTexture(hHeroTex);
    EXPECT_TRUE(suite, texRec.gpu.IsValid());
    EXPECT_TRUE(suite, texRec.gpu != kWhiteTexture);
    EXPECT_EQ(suite, texRec.width, 4U);
    EXPECT_EQ(suite, texRec.height, 4U);

    const SpriteHandle hIdle = sys.Bank().FindNamedFrame(hHeroAtlas, "idle");
    const SpriteHandle hRun  = sys.Bank().FindNamedFrame(hHeroAtlas, "run");
    EXPECT_TRUE(suite, hIdle.IsValid());
    EXPECT_TRUE(suite, hRun.IsValid());

    const SpriteFrame& idleFrame = sys.Bank().GetSprite(hIdle);
    const SpriteFrame& runFrame  = sys.Bank().GetSprite(hRun);
    EXPECT_EQ(suite, idleFrame.gpuTexture, texRec.gpu);
    EXPECT_EQ(suite, idleFrame.width, 16.0f);
    EXPECT_EQ(suite, idleFrame.height, 32.0f);
    EXPECT_EQ(suite, runFrame.width, 20.0f);

    // 6. Verify priority 10 patch pack overrode encrypted blob from priority 0 base pack
    const BlobRecord& cfgBlobRec = sys.Bank().GetBlob(hConfigBlob);
    EXPECT_TRUE(suite, cfgBlobRec.IsValid());
    EXPECT_EQ(suite, cfgBlobRec.size, sizeof(patchedSecret));
    EXPECT_EQ(suite, cfgBlobRec.version, 1U);
    if (cfgBlobRec.IsValid()) {
        const auto* words = reinterpret_cast<const u32*>(cfgBlobRec.data);
        EXPECT_EQ(suite, words[0], 999U);
        EXPECT_EQ(suite, words[7], 222U);
    }

    // 7. Verify AudioClip record streamed on-demand from file pack
    const AudioRecord& audioRec = sys.Bank().GetAudio(hJumpSfx);
    EXPECT_TRUE(suite, audioRec.IsValid());
    EXPECT_EQ(suite, audioRec.sampleRate, 48000U);
    EXPECT_EQ(suite, audioRec.channels, 2U);
    EXPECT_EQ(suite, audioRec.frameCount, 4U);
    EXPECT_TRUE(suite, audioRec.looping);
    EXPECT_EQ(suite, audioRec.Samples().size(), 8U);

    // 8. Verify live HotReloadBlob increments version and updates data in-place
    const u32 hotData[4] = {42U, 43U, 44U, 45U};
    EXPECT_TRUE(suite, sys.Bank().HotReloadBlob(
        kBlobId,
        std::span<const u8>(reinterpret_cast<const u8*>(hotData), sizeof(hotData))));
    EXPECT_EQ(suite, sys.Bank().HotReloadCount(), 1U);
    EXPECT_EQ(suite, sys.Bank().GetAssetVersion(kBlobId), 2U);
    const BlobRecord& hotBlobRec = sys.Bank().GetBlob(hConfigBlob);
    EXPECT_EQ(suite, hotBlobRec.size, sizeof(hotData));
    EXPECT_EQ(suite, hotBlobRec.version, 2U);
    if (hotBlobRec.data != nullptr) {
        const auto* hotWords = reinterpret_cast<const u32*>(hotBlobRec.data);
        EXPECT_EQ(suite, hotWords[0], 42U);
        EXPECT_EQ(suite, hotWords[3], 45U);
    }

    // 9. Also verify encrypted + LZ4-compressed pack entry when mounted at priority 20
    constexpr AssetId kEncBlobId = "blob/encrypted"_aid;
    u8 encPayload[128]{};
    const usize encCompLen = AssetBank::CompressLz4(
        std::span<const u8>(reinterpret_cast<const u8*>(configSecret), sizeof(configSecret)),
        std::span<u8>(encPayload, sizeof(encPayload))).value_or(0U);
    AssetSystem::XorCipherPayload(std::span<u8>(encPayload, encCompLen), kEncBlobId.value);

    EnPakEntry encEntry{};
    encEntry.assetId          = kEncBlobId.value;
    encEntry.uncompressedSize = sizeof(configSecret);
    encEntry.kind             = AssetKind::Blob;
    encEntry.compressed       = 2U;
    const std::span<const u8> encPayloads[1] = {std::span<const u8>(encPayload, encCompLen)};
    alignas(16) u8 encPak[512]{};
    const usize encPakBytes = BuildTestEnPak(
        std::span<u8>(encPak, sizeof(encPak)),
        std::span<const EnPakEntry>(&encEntry, 1),
        std::span<const std::span<const u8>>(encPayloads, 1));
    EXPECT_TRUE(suite, sys.MountPackMemory(std::span<const u8>(encPak, encPakBytes), 20, false).has_value());

    const BlobHandle hEncBlob = sys.PinBlob(gameplayScope, kEncBlobId, AssetResidency::Sim);
    const BlobRecord& encBlobRec = sys.Bank().GetBlob(hEncBlob);
    EXPECT_TRUE(suite, encBlobRec.IsValid());
    if (encBlobRec.IsValid()) {
        const auto* encWords = reinterpret_cast<const u32*>(encBlobRec.data);
        EXPECT_EQ(suite, encWords[0], 100U);
        EXPECT_EQ(suite, encWords[3], 400U);
    }

    // 10. Verify deterministic audit hash and clean EndScope + EvictUnpinned
    const u64 hash1 = sys.Bank().ComputeAuditHash();
    const u64 hash2 = sys.Bank().ComputeAuditHash();
    EXPECT_EQ(suite, hash1, hash2);

    sys.EndScope(gameplayScope);
    TextureHandle destroyedGpu[8]{};
    const u32 evicted = sys.Bank().EvictUnpinned(std::span<TextureHandle>(destroyedGpu, 8));
    EXPECT_EQ(suite, evicted, 5U);
    EXPECT_EQ(suite, sys.Bank().GetTexture(hHeroTex).width, 1U); // Fallback slot 0
    EXPECT_TRUE(suite, !sys.Bank().GetAudio(hJumpSfx).IsValid());

    sys.Shutdown();
    rnd->Shutdown();
    win.Destroy();
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestSuite suite{};

    TestLz4CompressionAndSafety(suite);
    TestAssetBankDirectLifecycleCoalescingAndGridAtlas(suite);
    TestAssetSystemVfsMountStreamingAndGpuUpload(suite);

    std::printf("Phase 6 Assets v2 Verification: %u passed, %u failed\n", suite.passed, suite.failed);
    return suite.failed == 0U ? 0 : 1;
}
