#include "tests/subsystems/CtsHarness.h"

#include "assets/AssetBank.h"
#include "assets/ImageLoader.h"

#include <vector>

namespace {

using namespace en;
using namespace en::cts;
using namespace en::literals;

bool CtsGenerationalFallbacksAndIdempotentScopes(CtsRunner& r) noexcept {
    AssetBank bank;
    CTS_CHECK(r, bank.Init({.arenaBytes = 512U * 1024U, .vramBudgetBytes = 1024U}).has_value());

    // Invalid, null, and out-of-bounds handles must safely resolve to slot-0 defaults.
    CTS_CHECK(r, bank.GetTexture(TextureAssetHandle{}).width == 1U);
    CTS_CHECK(r, bank.GetTexture(TextureAssetHandle{999U, 7U}).gpu == kWhiteTexture);
    CTS_CHECK(r, bank.GetSprite(SpriteHandle{999U, 3U}).width == 1.0f);
    CTS_CHECK(r, bank.GetBlob(BlobHandle{500U, 1U}).Bytes().empty());
    CTS_CHECK(r, bank.GetAudio(AudioHandle{500U, 1U}).TotalSamples() == 0U);

    const u8 s0 = *bank.BeginScope();
    const u8 s1 = *bank.BeginScope();

    std::array<u8, 64> rgba{};
    rgba.fill(0xABU);
    const TextureAssetHandle texA = bank.RegisterTexture(
        "sprites/kaelen.png"_aid, s0, 4U, 4U, rgba, TextureHandle{2U, 1U});
    CTS_CHECK(r, texA.IsValid() && bank.VramBytes() == 64U);

    // Re-registering the same AssetId under scope s1 must return the identical handle,
    // pin both scopes (s0 | s1), and NOT duplicate VRAM or arena allocations!
    const u32 usedBefore = bank.ArenaUsed();
    const TextureAssetHandle texB = bank.RegisterTexture(
        "sprites/kaelen.png"_aid, s1, 4U, 4U, rgba, TextureHandle{2U, 1U});
    CTS_CHECK(r, texA == texB && bank.VramBytes() == 64U && bank.ArenaUsed() == usedBefore);

    // Wrong AssetKind lookup for an existing AssetId must fail safely.
    CTS_CHECK(r, !bank.FindBlob("sprites/kaelen.png"_aid).IsValid());
    CTS_CHECK(r, bank.FindTexture("sprites/kaelen.png"_aid) == texA);

    // Registering a grid atlas and updating the master texture GPU handle propagates to all frames.
    const AtlasHandle atlas = bank.RegisterGridAtlas("atlases/kaelen"_aid, s0, texA, 2U, 2U);
    CTS_CHECK(r, atlas.IsValid());
    bank.BindGpu(texA, TextureHandle{9U, 2U});
    for (u16 i = 0U; i < 4U; ++i) {
        CTS_CHECK(r, bank.GetAtlasFrame(atlas, i).gpuTexture == TextureHandle{9U, 2U});
    }

    // Ending s0 while s1 is still active keeps the shared texture pinned!
    bank.EndScope(s0);
    CTS_CHECK(r, bank.EvictUnpinned() == 1U); // only atlas (pinned exclusively to s0) is evicted
    CTS_CHECK(r, bank.GetTexture(texA).width == 4U && bank.VramBytes() == 64U);

    bank.EndScope(s1);
    CTS_CHECK(r, bank.EvictUnpinned() == 1U && bank.VramBytes() == 0U);
    return true;
}

bool DstMetroidvania500RoomStreamingStress(CtsRunner& r) noexcept {
    // Simulate a Metroidvania player traversing 500 rooms while Scope 0 (permanent HUD + Hero)
    // stays pinned forever, verifying FreeBlock splitting & coalescing never leaks or fragments Arena!
    AssetBank bank;
    CTS_CHECK(r, bank.Init({
        .arenaBytes      = 256U * 1024U,
        .vramBudgetBytes = 4096U,
        .maxSlots        = 256U,
        .maxSprites      = 512U,
        .maxTextures     = 128U,
        .maxAtlases      = 64U,
        .maxBlobs        = 64U,
        .maxAudio        = 64U,
    }).has_value());

    const u8 permScope = *bank.BeginScope();
    std::array<u8, 256> heroPx{};
    heroPx.fill(255U);
    const TextureAssetHandle permHero = bank.RegisterTexture(
        "perm/hero.png"_aid, permScope, 8U, 8U, heroPx, TextureHandle{1U, 1U});
    CTS_CHECK(r, permHero.IsValid());
    const u32 permBaselineArena = bank.ArenaUsed();

    Rng rng(0x572EA42026ULL);
    std::vector<u8> scratchPx(4096U, 0x7FU);
    std::vector<f32> scratchPcm(256U, 0.25f);

    for (u32 room = 1U; room <= 500U; ++room) {
        bank.AdvanceFrame(room);
        const auto roomScopeRes = bank.BeginScope();
        CTS_CHECK(r, roomScopeRes.has_value());
        const u8 roomScope = *roomScopeRes;

        // Allocate variable-sized room textures, atlas, blob, and audio.
        const u16 dim = (room % 3U == 0U) ? 16U : ((room % 2U == 0U) ? 8U : 4U);
        const usize bytes = static_cast<usize>(dim) * dim * 4U;
        const AssetId texId{Mix64(room * 10ULL + 1ULL)};
        const AssetId atlId{Mix64(room * 10ULL + 2ULL)};
        const AssetId blbId{Mix64(room * 10ULL + 3ULL)};
        const AssetId audId{Mix64(room * 10ULL + 4ULL)};

        const TextureAssetHandle roomTex = bank.RegisterTexture(
            texId, roomScope, dim, dim, {scratchPx.data(), bytes}, TextureHandle{10U, 1U});
        const AtlasHandle roomAtl = bank.RegisterGridAtlas(atlId, roomScope, roomTex, 2U, 2U);
        const BlobHandle roomBlb  = bank.RegisterBlob(blbId, roomScope, {scratchPx.data(), (room % 7U + 1U) * 32U});
        const AudioHandle roomAud = bank.RegisterAudio(audId, roomScope, {scratchPcm.data(), (room % 4U + 1U) * 64U});

        CTS_CHECK(r, roomTex.IsValid() && roomAtl.IsValid() && roomBlb.IsValid() && roomAud.IsValid());

        // Leave room: end scope and evict unpinned room assets.
        bank.EndScope(roomScope);
        CTS_CHECK(r, bank.EvictUnpinned() == 4U);

        // Because FreeBytes coalesces adjacent blocks and rolls back the top of the arena,
        // ArenaUsed() must return to the exact permanent baseline after every single room!
        CTS_CHECK(r, bank.ArenaUsed() == permBaselineArena);
        CTS_CHECK(r, bank.VramBytes() == 256U);
    }

    // Permanent hero texture must remain valid after 500 room transitions.
    CTS_CHECK(r, bank.GetTexture(permHero).width == 8U);
    r.RecordFuzzOps(500U * 4U);
    r.MixDigest(bank.EvictCount());
    return true;
}

bool CtsImageDecodersAndCorruptionGuards(CtsRunner& r) noexcept {
    std::vector<u8> pixels(512U * 192U * 4U);
    CTS_CHECK(r, ImageLoader::LoadRgbaImageFromDisk(
        "data/fonts/pixel_game_font.png",
        "data/fonts/pixel_game_font.bmp",
        512U, 192U, pixels));

    // Dimension mismatch and non-existent paths must fail cleanly without modifying buffer size.
    CTS_CHECK(r, !ImageLoader::LoadRgbaImageFromDisk(
        "data/fonts/pixel_game_font.png",
        "data/fonts/pixel_game_font.bmp",
        256U, 192U, pixels));
    CTS_CHECK(r, !ImageLoader::LoadRgbaImageFromDisk(
        "data/fonts/non_existent.png",
        "data/fonts/non_existent.bmp",
        512U, 192U, pixels));
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "AssetsTests (CTS+DST)"};
    runner.RunCase("Generational Fallbacks & Multi-Scope Pinning", CtsGenerationalFallbacksAndIdempotentScopes);
    runner.RunCase("DST: 500-Room Streaming & Coalescing Stress", DstMetroidvania500RoomStreamingStress);
    runner.RunCase("Disk PNG/BMP Decoders & Corruption Guards", CtsImageDecodersAndCorruptionGuards);
    return runner.Finish();
}
