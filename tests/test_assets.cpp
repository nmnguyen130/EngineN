#include "core/Log.h"
#include "asset/AssetTypes.h"
#include "asset/TextureAsset.h"
#include "asset/AssetTable.h"
#include "asset/AssetReader.h"
#include "asset/NullTextureDevice.h"
#include "asset/AssetManager.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>

using namespace EngineN;

// -----------------------------------------------------------------------------
// Global Allocation Counter for 0-Alloc Hot-Path Contract Verification
// -----------------------------------------------------------------------------
static bool g_trackAllocations = false;
static usize g_allocationCount = 0;

void* operator new(std::size_t size) {
    if (g_trackAllocations) {
        g_allocationCount++;
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

// -----------------------------------------------------------------------------
// Unit Test Suites
// -----------------------------------------------------------------------------

void TestHandleLayoutAndIdentities() {
    std::cout << "  [1/7] Testing Canonical Struct Sizes, Alignments & Bit-Packing..." << std::endl;

    static_assert(sizeof(AssetHandle<Texture>) == 4);
    static_assert(sizeof(AssetId) == 8);
    static_assert(sizeof(Texture::Hot) == 16);

    // Null handle checks
    auto nullHandle = AssetHandle<Texture>::Null();
    assert(nullHandle.IsNull());
    assert(!nullHandle.IsValid());
    assert(nullHandle.Index() == 0);
    assert(nullHandle.Generation() == 0);
    assert(nullHandle.raw == 0);

    // FromParts packing / unpacking
    auto h1 = AssetHandle<Texture>::FromParts(12345, 42);
    assert(h1.IsValid());
    assert(!h1.IsNull());
    assert(h1.Index() == 12345);
    assert(h1.Generation() == 42);

    // Maximum index (20-bit = 1,048,575) and max generation (12-bit = 4,095)
    auto hMax = AssetHandle<Texture>::FromParts(0x000FFFFF, 0x0FFF);
    assert(hMax.Index() == 0x000FFFFF);
    assert(hMax.Generation() == 0x0FFF);

    // Constexpr AssetId literals and normalization
    constexpr AssetId id1 = "textures/player.png"_aid;
    AssetId id2 = MakeAssetId("textures/player.png");
    AssetId idNormalized = MakeAssetId("textures\\player.png");
    assert(id1 == id2);
    assert(id1 == idNormalized);
    assert(id1.Valid());

    std::cout << "        -> PASS" << std::endl;
}

void TestFallbackAndBranchlessResolve() {
    std::cout << "  [2/7] Testing Fallback Slot 0 & Branchless Resolution..." << std::endl;

    AssetTable<Texture> table;
    table.Init(16);

    // Configure fallback slot 0 with distinct identifiable markers
    Texture::Hot fallbackHot{};
    fallbackHot.gpuHandle = TextureHandle{99, 1};
    fallbackHot.width = 8;
    fallbackHot.height = 8;
    Texture::Cold fallbackCold{};
    fallbackCold.id = MakeAssetId("__fallback__");
    table.SetFallback(fallbackHot, fallbackCold);

    AssetReader reader(table);

    // 1. Null handle must resolve to slot 0 (fallback)
    auto nullHandle = AssetHandle<Texture>::Null();
    const auto& resNull = reader.Resolve(nullHandle);
    assert(resNull.gpuHandle.slot == 99);
    assert(resNull.width == 8);
    assert(reader.GpuTexture(nullHandle).slot == 99);

    // 2. Allocate a valid slot
    AssetId testId = "textures/test.png"_aid;
    auto validHandle = table.Allocate(testId);
    assert(validHandle.IsValid());
    assert(validHandle.Index() == 1);
    assert(validHandle.Generation() == 1);

    // While loading, hot data reflects fallback
    const auto& resLoading = reader.Resolve(validHandle);
    assert(resLoading.gpuHandle.slot == 99);

    // Set ready
    Texture::Hot validHot{};
    validHot.gpuHandle = TextureHandle{10, 1};
    validHot.width = 128;
    validHot.height = 256;
    Texture::Cold validCold{};
    validCold.id = testId;
    table.SetReady(validHandle.Index(), validHot, validCold);

    // Now valid handle resolves to actual hot data
    const auto& resReady = reader.Resolve(validHandle);
    assert(resReady.gpuHandle.slot == 10);
    assert(resReady.width == 128);
    assert(resReady.height == 256);

    // 3. Stale handle (correct slot, wrong generation) must resolve to fallback
    auto staleHandle = AssetHandle<Texture>::FromParts(validHandle.Index(), 999);
    const auto& resStale = reader.Resolve(staleHandle);
    assert(resStale.gpuHandle.slot == 99);
    assert(resStale.width == 8);

    // 4. Out-of-bounds handle (index >= capacity) must resolve to fallback
    auto oobHandle = AssetHandle<Texture>::FromParts(9999, 1);
    const auto& resOob = reader.Resolve(oobHandle);
    assert(resOob.gpuHandle.slot == 99);
    assert(resOob.width == 8);

    std::cout << "        -> PASS" << std::endl;
}

void TestDeduplication() {
    std::cout << "  [3/7] Testing Asset Request Deduplication..." << std::endl;

    AssetTable<Texture> table;
    table.Init(32);

    AssetId id = "textures/dungeon/tileset.png"_aid;
    auto firstHandle = table.Allocate(id);
    assert(firstHandle.IsValid());

    // 1000 duplicate allocation requests must return identical handle without new slots
    for (int i = 0; i < 1000; ++i) {
        auto dupHandle = table.Allocate(id);
        assert(dupHandle == firstHandle);
    }

    assert(table.ActiveCount() == 1);
    assert(table.Find(id) == firstHandle);

    std::cout << "        -> PASS" << std::endl;
}

void TestCapacityOverflow() {
    std::cout << "  [4/7] Testing Capacity Overflow & Drop Handling..." << std::endl;

    constexpr u32 kCap = 8;
    AssetTable<Texture> table;
    table.Init(kCap); // Free slots: 1 to 7 (7 total available)

    for (u32 i = 1; i < kCap; ++i) {
        AssetId id{1000ULL + i};
        auto h = table.Allocate(id);
        assert(h.IsValid());
        assert(table.Find(id) == h);
    }

    assert(table.FreeCount() == 0);
    assert(table.ActiveCount() == kCap - 1);

    // Attempting allocation beyond capacity must return Null handle and increment droppedAllocations
    AssetId overflowId{99999ULL};
    auto overflowHandle = table.Allocate(overflowId);
    assert(overflowHandle.IsNull());
    assert(table.DroppedAllocations() == 1);

    // Table state remains completely uncorrupted
    assert(table.Find(overflowId).IsNull());

    std::cout << "        -> PASS" << std::endl;
}

void TestLifecycleAndGenerationRetirement() {
    std::cout << "  [5/7] Testing Slot Lifecycle, Generation Bump & Retirement..." << std::endl;

    AssetTable<Texture> table;
    table.Init(4);

    AssetId id = "textures/particle.png"_aid;
    auto h1 = table.Allocate(id);
    assert(h1.IsValid());
    const u32 slot = h1.Index();
    assert(h1.Generation() == 1);

    // Free the asset
    table.Free(h1);
    assert(table.Find(id).IsNull());

    // Old handle h1 is now stale
    AssetReader reader(table);
    assert(reader.Resolve(h1).gpuHandle == table.GetHot(0).gpuHandle);

    // Re-allocating pops the recycled slot with bumped generation
    auto h2 = table.Allocate(id);
    assert(h2.Index() == slot);
    assert(h2.Generation() == 2);

    // Test generation boundary retirement
    // Force generation near ceiling
    constexpr u16 kMaxGen = AssetHandle<Texture>::kGenMask;
    for (u16 g = 2; g < kMaxGen; ++g) {
        table.Free(h2);
        h2 = table.Allocate(id);
        assert(h2.Generation() == g + 1);
    }

    // Now h2 has generation == kMaxGen. Freeing it must retire the slot instead of re-adding to free stack.
    assert(h2.Generation() == kMaxGen);
    const u32 freeCountBefore = table.FreeCount();
    table.Free(h2);
    // Free count must NOT increase because the slot was retired!
    assert(table.FreeCount() == freeCountBefore);

    std::cout << "        -> PASS" << std::endl;
}

void TestNullTextureDevice() {
    std::cout << "  [6/7] Testing NullTextureDevice Seam & Stub..." << std::endl;

    NullTextureDevice dev;
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;

    auto res = dev.Create(desc);
    assert(res.HasValue());
    auto handle = res.Value();
    assert(handle.IsValid());
    assert(dev.CreateCount() == 1);

    u8 dummyPixels[64 * 64 * 4] = {};
    auto uploadRes = dev.Upload(handle, URect{0, 0, 64, 64}, dummyPixels, 64 * 4);
    assert(uploadRes.HasValue());
    assert(dev.UploadCount() == 1);

    dev.Destroy(handle);
    assert(dev.DestroyCount() == 1);

    // Simulated failure
    dev.SetSimulateCreateFailure(true);
    auto failRes = dev.Create(desc);
    assert(!failRes.HasValue());

    std::cout << "        -> PASS" << std::endl;
}

void TestZeroAllocHotPath() {
    std::cout << "  [7/7] Testing Strict 0-Allocation Hot-Path Contract..." << std::endl;

    AssetTable<Texture> table;
    table.Init(64);

    Texture::Hot fallbackHot{};
    fallbackHot.gpuHandle = TextureHandle{1, 1};
    table.SetFallback(fallbackHot, {});

    AssetId id = "textures/hotpath.png"_aid;
    auto handle = table.Allocate(id);
    Texture::Hot hot{};
    hot.gpuHandle = TextureHandle{2, 1};
    hot.width = 256;
    hot.height = 256;
    table.SetReady(handle.Index(), hot, {});

    AssetReader reader(table);

    // Warm up and verify results
    assert(reader.GpuTexture(handle).slot == 2);
    assert(reader.GpuTexture(AssetHandle<Texture>::Null()).slot == 1);

    // Begin allocation tracking
    g_allocationCount = 0;
    g_trackAllocations = true;

    constexpr usize kIterations = 1'000'000;
    volatile u16 accumulator = 0;

    for (usize i = 0; i < kIterations; ++i) {
        // Resolve valid handle
        const auto& item = reader.Resolve(handle);
        accumulator = static_cast<u16>(accumulator + item.width);

        // Resolve null handle (branchless fallback)
        const auto& fb = reader.Resolve(AssetHandle<Texture>::Null());
        accumulator = static_cast<u16>(accumulator + fb.gpuHandle.slot);

        // GpuTexture shorthand
        auto tex = reader.GpuTexture(handle);
        accumulator = static_cast<u16>(accumulator + tex.slot);
    }

    g_trackAllocations = false;

    std::cout << "        Total heap allocations during " << kIterations << " Resolve calls: "
              << g_allocationCount << std::endl;
    assert(g_allocationCount == 0);

    std::cout << "        -> PASS" << std::endl;
}

void TestAssetManagerLifecycle() {
    std::cout << "  [8/8] Testing AssetManager Control Plane & Texture Pipeline..." << std::endl;

    NullTextureDevice dev;
    AssetManagerConfig cfg{.textureCapacity = 16};
    auto mgrRes = AssetManager::Create(cfg, dev);
    assert(mgrRes.HasValue());
    auto mgr = std::move(mgrRes.Value());

    // 1. Verify fallback texture creation at boot
    assert(dev.CreateCount() == 1);
    assert(dev.UploadCount() == 1);

    AssetReader reader = mgr.Reader();
    auto nullTex = reader.GpuTexture(AssetHandle<Texture>::Null());
    assert(nullTex.IsValid());
    assert(nullTex.slot == 1);

    // 2. Create temporary 16x16 TGA image on disk
    const char* testPath = "test_asset_tmp.tga";
    std::FILE* fp = std::fopen(testPath, "wb");
    assert(fp != nullptr);
    const u8 tgaHeader[18] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        16, 0, 16, 0, 32, 8
    };
    std::fwrite(tgaHeader, 1, sizeof(tgaHeader), fp);
    u8 pixelData[16 * 16 * 4];
    for (usize i = 0; i < sizeof(pixelData); ++i) {
        pixelData[i] = static_cast<u8>(i & 0xFF);
    }
    std::fwrite(pixelData, 1, sizeof(pixelData), fp);
    std::fclose(fp);

    // 3. Load valid texture
    AssetId testId = "textures/test_sample"_aid;
    auto handle = mgr.LoadTexture(testId, testPath);
    assert(handle.IsValid());
    assert(dev.CreateCount() == 2);
    assert(dev.UploadCount() == 2);

    const auto& hot = reader.Resolve(handle);
    assert(hot.width == 16);
    assert(hot.height == 16);
    assert(hot.gpuHandle.slot == 2);

    // Deduplication test: re-requesting returns identical handle
    auto dupHandle = mgr.LoadTexture(testId, testPath);
    assert(dupHandle == handle);
    assert(dev.CreateCount() == 2);

    // 4. Missing file fails gracefully and resolves to fallback
    auto missingHandle = mgr.LoadTexture("textures/missing"_aid, "non_existent_file.png");
    assert(missingHandle.IsValid());
    const auto& missingHot = reader.Resolve(missingHandle);
    assert(missingHot.gpuHandle == nullTex);
    assert(missingHot.width == 8);

    // 5. Unload
    mgr.Unload(handle);
    assert(dev.DestroyCount() == 1);
    const auto& staleHot = reader.Resolve(handle);
    assert(staleHot.gpuHandle == nullTex);

    std::remove(testPath);
    std::cout << "        -> PASS" << std::endl;
}

void TestAtlasAndSpriteResolution() {
    std::cout << "  [9/9] Testing Atlas Asset Creation & Sprite Resolution..." << std::endl;

    static_assert(sizeof(FrameHot) == 16);
    static_assert(sizeof(Atlas::Hot) == 16);

    NullTextureDevice dev;
    AssetManagerConfig cfg{
        .textureCapacity = 16,
        .atlasCapacity = 16,
        .maxFrames = 128
    };
    auto mgrRes = AssetManager::Create(cfg, dev);
    assert(mgrRes.HasValue());
    auto mgr = std::move(mgrRes.Value());
    AssetReader reader = mgr.Reader();

    // 1. Fallback sprite resolution for null and stale refs
    SpriteRef nullRef{};
    SpriteView nullView = reader.Sprite(nullRef);
    assert(nullView.texture.slot == 1);
    assert(nullView.uvRect[0] == 0 && nullView.uvRect[2] == 65535);

    // 2. Create a test texture (64x64)
    const char* texPath = "test_atlas_tex.tga";
    std::FILE* fp = std::fopen(texPath, "wb");
    assert(fp != nullptr);
    const u8 tgaHeader[18] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        64, 0, 64, 0, 32, 8
    };
    std::fwrite(tgaHeader, 1, sizeof(tgaHeader), fp);
    u8 pixelData[64 * 64 * 4] = {};
    std::fwrite(pixelData, 1, sizeof(pixelData), fp);
    std::fclose(fp);

    auto texHandle = mgr.LoadTexture("textures/atlas_src"_aid, texPath);
    assert(texHandle.IsValid());

    // 3. Grid atlas slicing: 2x2 grid of 32x32 frames on 64x64 texture
    AtlasGridDesc gridDesc{
        .texture = texHandle,
        .frameWidth = 32,
        .frameHeight = 32,
        .frameCount = 4,
        .pivotX = 16,
        .pivotY = 16
    };
    auto atlasHandle = mgr.CreateAtlasFromGrid("atlases/hero_run"_aid, gridDesc);
    assert(atlasHandle.IsValid());

    // Verify SpriteView unorm16 UVs for all 4 frames
    // Frame 0: (0, 0, 32767, 32767)
    SpriteView v0 = reader.Sprite(SpriteRef{atlasHandle, 0});
    assert(v0.texture.slot == 2);
    assert(v0.size[0] == 32 && v0.size[1] == 32);
    assert(v0.pivot[0] == 16 && v0.pivot[1] == 16);
    assert(v0.uvRect[0] == 0 && v0.uvRect[1] == 0);
    assert(v0.uvRect[2] == 32767 && v0.uvRect[3] == 32767);

    // Frame 1: (32767, 0, 65535, 32767)
    SpriteView v1 = reader.Sprite(SpriteRef{atlasHandle, 1});
    assert(v1.uvRect[0] == 32767 && v1.uvRect[1] == 0);
    assert(v1.uvRect[2] == 65535 && v1.uvRect[3] == 32767);

    // Frame 2: (0, 32767, 32767, 65535)
    SpriteView v2 = reader.Sprite(SpriteRef{atlasHandle, 2});
    assert(v2.uvRect[0] == 0 && v2.uvRect[1] == 32767);
    assert(v2.uvRect[2] == 32767 && v2.uvRect[3] == 65535);

    // Frame 3: (32767, 32767, 65535, 65535)
    SpriteView v3 = reader.Sprite(SpriteRef{atlasHandle, 3});
    assert(v3.uvRect[0] == 32767 && v3.uvRect[1] == 32767);
    assert(v3.uvRect[2] == 65535 && v3.uvRect[3] == 65535);

    // 4. Frame clamping: index 99 clamps to frame 3
    SpriteView vClamped = reader.Sprite(SpriteRef{atlasHandle, 99});
    assert(vClamped.uvRect[0] == v3.uvRect[0] && vClamped.uvRect[2] == v3.uvRect[2]);

    // 5. Unload atlas: handle becomes stale and resolves to fallback
    mgr.UnloadAtlas(atlasHandle);
    SpriteView vStale = reader.Sprite(SpriteRef{atlasHandle, 0});
    assert(vStale.texture.slot == 1);

    // 6. Strict zero-alloc contract on Sprite() hot path
    auto atlasActive = mgr.CreateAtlasFromGrid("atlases/hero_active"_aid, gridDesc);
    SpriteRef activeRef{atlasActive, 1};

    g_allocationCount = 0;
    g_trackAllocations = true;
    constexpr usize kIterations = 1'000'000;
    volatile u16 uvAccum = 0;
    for (usize i = 0; i < kIterations; ++i) {
        SpriteView sv = reader.Sprite(activeRef);
        uvAccum = static_cast<u16>(uvAccum + sv.uvRect[0]);
    }
    g_trackAllocations = false;

    std::cout << "        Total heap allocations during " << kIterations << " Sprite calls: "
              << g_allocationCount << std::endl;
    assert(g_allocationCount == 0);

    std::remove(texPath);
    std::cout << "        -> PASS" << std::endl;
}

int main() {
    std::cout << "\n========================================================" << std::endl;
    std::cout << " Running EngineN Asset Subsystem Verification (Phase 1a, 1b & 1c)" << std::endl;
    std::cout << "========================================================" << std::endl;

    TestHandleLayoutAndIdentities();
    TestFallbackAndBranchlessResolve();
    TestDeduplication();
    TestCapacityOverflow();
    TestLifecycleAndGenerationRetirement();
    TestNullTextureDevice();
    TestZeroAllocHotPath();
    TestAssetManagerLifecycle();
    TestAtlasAndSpriteResolution();

    std::cout << "\n========================================================" << std::endl;
    std::cout << " ALL ASSET SUBSYSTEM TESTS PASSED (0-ALLOC VERIFIED)" << std::endl;
    std::cout << "========================================================\n" << std::endl;

    return 0;
}
