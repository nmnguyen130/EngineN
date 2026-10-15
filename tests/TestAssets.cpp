#include "assets/AssetBank.h"
#include "assets/AssetSystem.h"
#include "assets/AssetTypes.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <source_location>
#include <string_view>
#include <vector>

using namespace en;
using namespace en::assets;

namespace {

static u32 g_failedAssertions = 0;
static u32 g_passedAssertions = 0;

void AssertTrue(bool condition, std::string_view expression, std::source_location loc = std::source_location::current()) {
    if (!condition) {
        std::cerr << "[FAIL] " << loc.file_name() << ":" << loc.line()
                  << " Assertion failed: " << expression << "\n";
        ++g_failedAssertions;
    } else {
        ++g_passedAssertions;
    }
}

#define TEST_ASSERT(cond) AssertTrue(static_cast<bool>(cond), #cond)

void TestCompileTimeConstantsAndLayout() {
    static_assert(sizeof(SpriteFrame) == 32);
    static_assert(alignof(SpriteFrame) == 32);
    static_assert(sizeof(SlotMeta) == 16);
    static_assert(alignof(SlotMeta) == 16);
    static_assert(sizeof(BlobRecord) == 16);
    static_assert(sizeof(AudioRecord) == 16);
    static_assert(sizeof(PakHeader) == 32);
    static_assert(sizeof(PakEntry) == 32);
    static_assert(sizeof(CookedAtlasHeader) == 16);
    static_assert(sizeof(CookedFrameName) == 8);

    TEST_ASSERT(sizeof(SpriteFrame) == 32);
    TEST_ASSERT(sizeof(SlotMeta) == 16);
    TEST_ASSERT(sizeof(PakHeader) == 32);
    TEST_ASSERT(sizeof(PakEntry) == 32);
}

void TestAssetIdAndLiteral() {
    constexpr AssetId id1 = AssetId::Hash("textures/hero.png");
    constexpr AssetId id2 = AssetId::Hash("TEXTURES\\HERO.PNG");
    constexpr AssetId id3 = "textures/hero.png"_aid;

    TEST_ASSERT(id1.IsValid());
    TEST_ASSERT(id1 == id2);
    TEST_ASSERT(id1 == id3);

    constexpr AssetId emptyId = AssetId::Hash("");
    TEST_ASSERT(!emptyId.IsValid());
    TEST_ASSERT(emptyId.value == 0);
}

void TestHandlesAndSlot0Fallback() {
    AssetBank bank;
    const auto initRes = bank.Init();
    TEST_ASSERT(initRes.has_value());

    // Slot 0 of Sprite is permanently valid (unit UV, white texture)
    const SpriteFrame& defSprite = bank.GetSprite(SpriteHandle{0, 0});
    TEST_ASSERT(defSprite.gpuTexture == renderer::kWhiteTexture);
    TEST_ASSERT(defSprite.uv.w == 1.0f && defSprite.uv.h == 1.0f);

    // Any invalid or out-of-range handle branchlessly returns slot 0 fallback
    const SpriteFrame& invalidSprite = bank.GetSprite(SpriteHandle{9999, 5});
    TEST_ASSERT(invalidSprite.gpuTexture == renderer::kWhiteTexture);

    const TextureRecord& defTex = bank.GetTexture(TextureAssetHandle{9999, 1});
    TEST_ASSERT(defTex.gpu == renderer::kWhiteTexture);

    const BlobRecord& defBlob = bank.GetBlob(BlobHandle{9999, 1});
    TEST_ASSERT(defBlob.data != nullptr);
    TEST_ASSERT(defBlob.size > 0);

    const AudioRecord& defAudio = bank.GetAudio(AudioHandle{9999, 1});
    TEST_ASSERT(defAudio.pcm != nullptr);
    TEST_ASSERT(defAudio.frameCount > 0);

    bank.Shutdown();
}

void TestScopePinningAndLifetime() {
    AssetBank bank;
    const auto initRes = bank.Init();
    TEST_ASSERT(initRes.has_value());

    // Allocate scopes
    const auto scopeA = bank.BeginScope();
    TEST_ASSERT(scopeA.has_value());
    const auto scopeB = bank.BeginScope();
    TEST_ASSERT(scopeB.has_value());
    TEST_ASSERT(*scopeA != *scopeB);

    // Acquire slot for a texture
    bool needsLoad = false;
    const auto slotRes = bank.AcquireSlot("tex/player.png"_aid, AssetKind::Texture, *scopeA, Residency::Presentation, needsLoad);
    TEST_ASSERT(slotRes.has_value());
    TEST_ASSERT(needsLoad);

    const u16 slot = *slotRes;
    const auto hTex = bank.ToTextureHandle(slot);
    TEST_ASSERT(hTex.IsValid());

    // Commit dummy GPU handle
    const auto commitRes = bank.CommitTexture(slot, renderer::TextureHandle{10, 1}, 64, 64, 16384);
    TEST_ASSERT(commitRes.has_value());

    const auto& texRec = bank.GetTexture(hTex);
    TEST_ASSERT(texRec.gpu.index == 10);
    TEST_ASSERT(texRec.width == 64 && texRec.height == 64);

    // End scope A -> unpins slot
    bank.EndScope(*scopeA);

    // Eviction test: under budget, nothing evicted
    std::array<renderer::TextureHandle, 16> evicted{};
    u32 evictCount = bank.EvictUnpinned(evicted);
    TEST_ASSERT(evictCount == 0);

    bank.EndScope(*scopeB);
    bank.Shutdown();
}

void TestRobinHoodHashIndex() {
    AssetBank bank;
    const auto initRes = bank.Init();
    TEST_ASSERT(initRes.has_value());

    const auto scope = bank.BeginScope();
    TEST_ASSERT(scope.has_value());

    // Insert 100 unique assets
    for (u32 i = 1; i <= 100; ++i) {
        const u64 fakeIdVal = 1000000ULL + i;
        const AssetId aid{fakeIdVal};
        bool needsLoad = false;
        const auto slotRes = bank.AcquireSlot(aid, AssetKind::Blob, *scope, Residency::Sim, needsLoad);
        TEST_ASSERT(slotRes.has_value());
        TEST_ASSERT(needsLoad);

        // Find immediately
        const u16 foundSlot = bank.FindSlot(aid);
        TEST_ASSERT(foundSlot == *slotRes);
    }

    // Duplicate acquire returns existing slot with needsLoad == false
    bool needsLoadDup = true;
    const auto dupRes = bank.AcquireSlot(AssetId{1000001ULL}, AssetKind::Blob, *scope, Residency::Sim, needsLoadDup);
    TEST_ASSERT(dupRes.has_value());
    TEST_ASSERT(!needsLoadDup);

    bank.EndScope(*scope);
    bank.Shutdown();
}

void TestLz4Decompressor() {
    // Synthetic LZ4 block: literal "EngineN_LZ4_Test_Data" (21 bytes)
    // LZ4 format for pure literals:
    // Token: (21 - 15 = 6 in high nibble) -> token = 0xF0, followed by length byte 6 (15 + 6 = 21)
    // Followed by 21 bytes literal data
    const std::string_view rawLiteral = "EngineN_LZ4_Test_Data";
    std::vector<u8> compressed;
    compressed.push_back(0xF0); // 15 + next byte
    compressed.push_back(6);    // 15 + 6 = 21 literals
    for (char c : rawLiteral) {
        compressed.push_back(static_cast<u8>(c));
    }

    std::array<u8, 64> decompressed{};
    const auto decRes = AssetBank::DecompressLz4(compressed, decompressed);
    TEST_ASSERT(decRes.has_value());
    TEST_ASSERT(*decRes == rawLiteral.size());
    TEST_ASSERT(std::string_view(reinterpret_cast<const char*>(decompressed.data()), *decRes) == rawLiteral);

    // Corrupted payload returns error
    const std::array<u8, 2> corrupted{0xF0, 255};
    const auto errRes = AssetBank::DecompressLz4(corrupted, decompressed);
    TEST_ASSERT(!errRes.has_value());
}

void TestCookedPakMemoryMountAndStreaming() {
    AssetSystem sys;
    const auto initRes = sys.Init();
    TEST_ASSERT(initRes.has_value());

    // Construct a synthetic valid in-memory .enpak archive
    const std::string_view blobData = "HeroStats_Cooked_Binary_Blob";
    const AssetId blobAid = "prefabs/hero.dat"_aid;

    std::vector<u8> pakBuffer;

    // Header placeholder
    PakHeader hdr{};
    hdr.magic = kPakMagic;
    hdr.version = kPakVersion;
    hdr.entryCount = 1;
    hdr.flags = 0;
    hdr.contentHash = 0x12345678ULL;

    pakBuffer.resize(sizeof(PakHeader));

    // Payload offset
    const u64 payloadOffset = pakBuffer.size();
    for (char c : blobData) {
        pakBuffer.push_back(static_cast<u8>(c));
    }

    // TOC offset
    hdr.tocOffset = pakBuffer.size();

    PakEntry entry{};
    entry.assetId = blobAid.value;
    entry.offset = payloadOffset;
    entry.storedSize = static_cast<u32>(blobData.size());
    entry.rawSize = static_cast<u32>(blobData.size());
    entry.metaA = 0xCAFEBABE; // schemaHash
    entry.metaB = 0;
    entry.kind = AssetKind::Blob;
    entry.codec = AssetCodec::Raw;

    const u8* entryPtr = reinterpret_cast<const u8*>(&entry);
    pakBuffer.insert(pakBuffer.end(), entryPtr, entryPtr + sizeof(PakEntry));

    // Overwrite header with computed offsets
    std::memcpy(pakBuffer.data(), &hdr, sizeof(PakHeader));

    // Mount pack from memory
    const auto mountRes = sys.MountPackMemory(pakBuffer, 0);
    TEST_ASSERT(mountRes.has_value());

    const auto scope = sys.BeginScope();
    TEST_ASSERT(scope.has_value());

    // Pin blob
    const auto blobHandleRes = sys.PinBlob(*scope, blobAid, Residency::Sim);
    TEST_ASSERT(blobHandleRes.has_value());

    // Headless pump processes completed queue
    sys.PumpHeadless();

    // Verify blob payload resolves zero-copy directly to memory view
    const auto& blobRecord = sys.Bank().GetBlob(*blobHandleRes);
    TEST_ASSERT(blobRecord.data != nullptr);
    TEST_ASSERT(blobRecord.size == blobData.size());
    TEST_ASSERT(std::string_view(reinterpret_cast<const char*>(blobRecord.data), blobRecord.size) == blobData);
    TEST_ASSERT(blobRecord.schemaHash == 0xCAFEBABE);

    // WaitSimReady succeeds
    const auto simReady = sys.WaitSimReady(*scope);
    TEST_ASSERT(simReady.has_value());

    sys.EndScope(*scope);
    sys.Shutdown();
}

void TestAuditHashDeterminism() {
    AssetBank a;
    (void)a.Init();
    const auto sA = a.BeginScope();
    bool nA = false;
    (void)a.AcquireSlot("tex/a.png"_aid, AssetKind::Texture, *sA, Residency::Presentation, nA);

    AssetBank b;
    (void)b.Init();
    const auto sB = b.BeginScope();
    bool nB = false;
    (void)b.AcquireSlot("tex/a.png"_aid, AssetKind::Texture, *sB, Residency::Presentation, nB);

    TEST_ASSERT(a.ComputeAuditHash() != 0);
    TEST_ASSERT(a.ComputeAuditHash() == b.ComputeAuditHash());

    a.Shutdown();
    b.Shutdown();
}

} // namespace

int main() {
    std::cout << "Running EngineN Asset Subsystem Tests...\n";

    TestCompileTimeConstantsAndLayout();
    TestAssetIdAndLiteral();
    TestHandlesAndSlot0Fallback();
    TestScopePinningAndLifetime();
    TestRobinHoodHashIndex();
    TestLz4Decompressor();
    TestCookedPakMemoryMountAndStreaming();
    TestAuditHashDeterminism();

    std::cout << "Test Summary: " << g_passedAssertions << " passed, "
              << g_failedAssertions << " failed.\n";

    return (g_failedAssertions == 0) ? 0 : 1;
}
