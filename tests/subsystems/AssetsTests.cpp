#include "tests/subsystems/CtsHarness.h"

#include "assets/AssetBank.h"
#include "assets/ImageLoader.h"
#include "assets/LdtkImporter.h"

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
        "sprites/actor.png"_aid, s0, 4U, 4U, rgba, TextureHandle{2U, 1U});
    CTS_CHECK(r, texA.IsValid() && bank.VramBytes() == 64U);

    // Re-registering the same AssetId under scope s1 must return the identical handle,
    // pin both scopes (s0 | s1), and NOT duplicate VRAM or arena allocations!
    const u32 usedBefore = bank.ArenaUsed();
    const TextureAssetHandle texB = bank.RegisterTexture(
        "sprites/actor.png"_aid, s1, 4U, 4U, rgba, TextureHandle{2U, 1U});
    CTS_CHECK(r, texA == texB && bank.VramBytes() == 64U && bank.ArenaUsed() == usedBefore);

    // Wrong AssetKind lookup for an existing AssetId must fail safely.
    CTS_CHECK(r, !bank.FindBlob("sprites/actor.png"_aid).IsValid());
    CTS_CHECK(r, bank.FindTexture("sprites/actor.png"_aid) == texA);

    // Registering a grid atlas and updating the master texture GPU handle propagates to all frames.
    const AtlasHandle atlas = bank.RegisterGridAtlas("atlases/actor"_aid, s0, texA, 2U, 2U);
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

        bank.EndScope(roomScope);
        CTS_CHECK(r, bank.EvictUnpinned() == 4U);
        CTS_CHECK(r, bank.ArenaUsed() == permBaselineArena);
        CTS_CHECK(r, bank.VramBytes() == 256U);
    }

    CTS_CHECK(r, bank.GetTexture(permHero).width == 8U);
    r.RecordFuzzOps(500U * 4U);
    r.MixDigest(bank.EvictCount());
    return true;
}

bool CtsImageDecodersAndLdtkLevelImporter(CtsRunner& r) noexcept {
    std::vector<u8> pixels(512U * 192U * 4U);
    CTS_CHECK(r, ImageLoader::LoadRgbaImageFromDisk(
        "data/fonts/pixel_game_font.png",
        "data/fonts/pixel_game_font.bmp",
        512U, 192U, pixels));

    CTS_CHECK(r, !ImageLoader::LoadRgbaImageFromDisk(
        "data/fonts/pixel_game_font.png",
        "data/fonts/pixel_game_font.bmp",
        256U, 192U, pixels));
    CTS_CHECK(r, !ImageLoader::LoadRgbaImageFromDisk(
        "data/fonts/non_existent.png",
        "data/fonts/non_existent.bmp",
        512U, 192U, pixels));

    // Verify zero-allocation LdtkImporter JSON parsing (including slopes & Y-up row flip) and binary blob cooking.
    Arena scratch;
    CTS_CHECK(r, scratch.Init(64U * 1024U));

    constexpr std::string_view kLdtkJson = R"json({
        "worldX": 0,
        "worldY": 0,
        "layerInstances": [
            {
                "__identifier": "Collisions",
                "__cWid": 4,
                "__cHei": 3,
                "__gridSize": 16,
                "intGridCsv": [
                    0, 0, 2, 0,
                    0, 5, 1, 6,
                    1, 1, 1, 1
                ]
            },
            {
                "__identifier": "Entities",
                "entityInstances": [
                    {
                        "__identifier": "PlayerSpawn",
                        "iid": "ent-hero-01",
                        "width": 16,
                        "height": 32,
                        "px": [32, 32],
                        "fieldInstances": [{ "__value": 7 }]
                    },
                    {
                        "__identifier": "SaveBench",
                        "iid": "ent-bench-01",
                        "width": 32,
                        "height": 16,
                        "px": [48, 32],
                        "fieldInstances": [{ "__value": 42 }]
                    }
                ]
            }
        ]
    })json";

    const auto parsed = LdtkImporter::ParseLevelJson(kLdtkJson, scratch, 16.0f);
    CTS_CHECK(r, parsed.has_value());
    CTS_CHECK(r, parsed->widthTiles == 4U && parsed->heightTiles == 3U);
    CTS_CHECK(r, Near(parsed->tileSizeMeters, 1.0f));
    // Bottom row in LDtk (row 2) -> ty = 0 in Y-up engine: all Solid!
    CTS_CHECK(r, parsed->tiles[0 * 4 + 0] == TileFlag::Solid);
    // Middle row in LDtk (row 1) -> ty = 1 in Y-up engine: col 1 = Slope45Up (5), col 2 = Solid (1), col 3 = Slope45Down (6)!
    CTS_CHECK(r, parsed->tiles[1 * 4 + 1] == TileFlag::Slope45Up);
    CTS_CHECK(r, parsed->tiles[1 * 4 + 2] == TileFlag::Solid);
    CTS_CHECK(r, parsed->tiles[1 * 4 + 3] == TileFlag::Slope45Down);
    // Top row in LDtk (row 0) -> ty = 2 in Y-up engine: col 2 = OneWay (2)!
    CTS_CHECK(r, parsed->tiles[2 * 4 + 2] == TileFlag::OneWay);

    CTS_CHECK(r, parsed->entities.size() == 2U);
    CTS_CHECK(r, parsed->entities[0].identifier == "PlayerSpawn"_sid);
    CTS_CHECK(r, Near(parsed->entities[0].worldPos.x, 2.0f) && Near(parsed->entities[0].worldPos.y, 1.0f));
    CTS_CHECK(r, parsed->entities[0].customInt == 7);
    CTS_CHECK(r, parsed->entities[1].identifier == "SaveBench"_sid && parsed->entities[1].customInt == 42);

    std::array<u8, 256> cookedBlob{};
    const auto cookedBytes = LdtkImporter::CookTileMapBlob(*parsed, cookedBlob);
    CTS_CHECK(r, cookedBytes.has_value() && *cookedBytes == sizeof(TileMapHeader) + 12U);
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "AssetsTests (CTS+DST)"};
    runner.RunCase("Generational Fallbacks & Multi-Scope Pinning", CtsGenerationalFallbacksAndIdempotentScopes);
    runner.RunCase("DST: 500-Room Streaming & Coalescing Stress", DstMetroidvania500RoomStreamingStress);
    runner.RunCase("Disk PNG/BMP Decoders & LDtk Level Importer", CtsImageDecodersAndLdtkLevelImporter);
    return runner.Finish();
}
