#include "assets/AssetBank.h"
#include "assets/ImageLoader.h"

#include <cstdio>
#include <vector>

namespace {

using namespace en;
using namespace en::literals;

bool TestAssetScopesAndDirectLookup() noexcept {
    AssetBank bank;
    if (!bank.Init({.arenaBytes = 512U * 1024U, .vramBudgetBytes = 64U})) return false;

    if (bank.GetTexture(TextureAssetHandle{}).width != 1U ||
        bank.GetSprite(SpriteHandle{}).width != 1.0f) {
        return false;
    }

    const auto scopeRes = bank.BeginScope();
    if (!scopeRes) return false;
    const u8 scope = *scopeRes;

    std::array<u8, 64> rgba{};
    rgba.fill(200U);
    const TextureAssetHandle tex = bank.RegisterTexture(
        "sprites/hero.png"_aid, scope, 4U, 4U, rgba, TextureHandle{3U, 1U});
    if (!tex.IsValid() || bank.GetTexture(tex).width != 4U || bank.VramBytes() != 64U) {
        return false;
    }

    const AtlasHandle atlas = bank.RegisterGridAtlas("atlases/hero"_aid, scope, tex, 2U, 2U);
    if (!atlas.IsValid()) return false;
    const SpriteFrame& f0 = bank.GetAtlasFrame(atlas, 0U);
    if (f0.width != 2.0f || f0.height != 2.0f || f0.gpuTexture != TextureHandle{3U, 1U}) {
        return false;
    }

    const u8 blobBytes[]{1, 2, 3, 4, 5};
    const BlobHandle blob = bank.RegisterBlob("data/room1.bin"_aid, scope, blobBytes);
    if (!blob.IsValid() || bank.GetBlob(blob).Bytes().size() != 5U) return false;

    const f32 pcm[]{0.1f, -0.1f, 0.2f, -0.2f};
    const AudioHandle aud = bank.RegisterAudio("sfx/slash.wav"_aid, scope, pcm, 48000U, 2U, false);
    if (!aud.IsValid() || bank.GetAudio(aud).TotalSamples() != 4U) return false;

    // Test LRU eviction when over budget after scope ends.
    const u8 scope2 = *bank.BeginScope();
    bank.AdvanceFrame(10U);
    const TextureAssetHandle tex2 = bank.RegisterTexture(
        "sprites/boss.png"_aid, scope2, 4U, 4U, rgba, TextureHandle{4U, 1U});
    if (!tex2.IsValid() || !bank.OverVram()) return false;

    bank.EndScope(scope);
    std::array<TextureHandle, 4> evictedGpu{};
    if (bank.EvictLru(evictedGpu) != 1U || evictedGpu[0] != TextureHandle{3U, 1U}) return false;
    if (bank.OverVram() || bank.GetTexture(tex).gpu != kWhiteTexture) return false;

    bank.EndScope(scope2);
    if (bank.EvictUnpinned() != 4U || bank.VramBytes() != 0U) return false;

    return true;
}

bool TestDiskImageLoading() noexcept {
    std::vector<u8> pixels(512U * 192U * 4U);
    if (!ImageLoader::LoadRgbaImageFromDisk(
            "data/fonts/pixel_game_font.png",
            "data/fonts/pixel_game_font.bmp",
            512U, 192U, pixels)) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!TestAssetScopesAndDirectLookup()) return 1;
    if (!TestDiskImageLoading()) return 2;
    std::printf("AssetsTests: ALL PASSED\n");
    return 0;
}
