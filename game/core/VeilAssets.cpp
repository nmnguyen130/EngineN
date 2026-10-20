#include "game/core/VeilAssets.h"
#include "core/Serialization.h"
#include <cmath>
#include <vector>

namespace en::veil {

namespace {
void AppendEntry(
    std::vector<EnPakEntry>& entries,
    std::vector<u8>& payloadBlob,
    AssetId id,
    AssetKind kind,
    std::span<const u8> rawBytes,
    u16 wOrRate = 0,
    u16 hOrCh = 0) {
    EnPakEntry e{};
    e.assetId = id.value;
    e.offset = static_cast<u32>(payloadBlob.size());
    e.compressedSize = static_cast<u32>(rawBytes.size());
    e.uncompressedSize = static_cast<u32>(rawBytes.size());
    e.widthOrRate = wOrRate;
    e.heightOrChannels = hOrCh;
    e.kind = kind;
    e.compressed = 0;
    entries.push_back(e);
    payloadBlob.insert(payloadBlob.end(), rawBytes.begin(), rawBytes.end());
    while ((payloadBlob.size() & 15U) != 0U) payloadBlob.push_back(0);
}

std::vector<u8> BuildCollisionMap(u16 w, u16 h, bool bossArena) {
    std::vector<u8> buf(sizeof(CookedCollisionHeader) + static_cast<usize>(w) * h, 0);
    BinaryWriter bw(buf);
    CookedCollisionHeader hdr{};
    hdr.magic = kCookedCollisionMagic;
    hdr.width = w;
    hdr.height = h;
    hdr.tileSize = 1.0f;
    hdr.originX = 0.0f;
    hdr.originY = 0.0f;
    (void)bw.Write(hdr);

    for (u16 y = 0; y < h; ++y) {
        for (u16 x = 0; x < w; ++x) {
            TileType t = TileType::Empty;
            if (y == 0 || y == 1 || x == 0 || x == w - 1 || y == h - 1) {
                t = TileType::Solid;
            } else if (!bossArena) {
                if (y == 5 && x >= 10 && x <= 16) t = TileType::OneWay;
                if (y == 2 && x == 18) t = TileType::SlopeUpRight;
                if (y == 2 && x >= 22 && x <= 25) t = TileType::ConveyorRight;
                if (y == 2 && x >= 30 && x <= 32) t = TileType::SpikeHazard;
                if (y == 2 && x >= 36 && x <= 40) t = TileType::WaterVolume;
            }
            const u8 raw = static_cast<u8>(t);
            (void)bw.Write(raw);
        }
    }
    return buf;
}

std::vector<u8> BuildCombatRig(bool isPlayer) {
    std::vector<u8> buf(256, 0);
    BinaryWriter bw(buf);
    CookedRigHeader hdr{kCookedRigMagic, 1, isPlayer ? u16(6) : u16(2), 0};
    (void)bw.Write(hdr);

    if (isPlayer) {
        const CookedRigFrameDirective dirs[6] = {
            {CompactAabb2D::FromMeters(0.85f, 0.0f, 0.75f, 0.45f), 40, 0, static_cast<u8>(PlayerState::Slash1), 2, 1, 18},
            {CompactAabb2D{}, 0, 0, static_cast<u8>(PlayerState::Slash1), 8, 0, 0},
            {CompactAabb2D::FromMeters(0.95f, 0.05f, 0.85f, 0.5f), 56, 0, static_cast<u8>(PlayerState::Slash2), 2, 1, 24},
            {CompactAabb2D{}, 0, 0, static_cast<u8>(PlayerState::Slash2), 8, 0, 0},
            {CompactAabb2D::FromMeters(1.1f, 0.1f, 1.0f, 0.65f), 80, 16, static_cast<u8>(PlayerState::Slash3), 3, 1, 35},
            {CompactAabb2D{}, 0, 0, static_cast<u8>(PlayerState::Slash3), 10, 0, 0},
        };
        for (const auto& d : dirs) (void)bw.Write(d);
    } else {
        const CookedRigFrameDirective dirs[2] = {
            {CompactAabb2D::FromMeters(0.8f, 0.0f, 0.75f, 0.5f), 32, 0, static_cast<u8>(EnemyState::Attack), 2, 1, 15},
            {CompactAabb2D{}, 0, 0, static_cast<u8>(EnemyState::Attack), 9, 0, 0},
        };
        for (const auto& d : dirs) (void)bw.Write(d);
    }
    buf.resize(bw.Offset());
    return buf;
}

std::vector<u8> BuildSynthWave(f32 freqHz, u32 frames) {
    std::vector<f32> pcm(frames * 2U, 0.0f);
    for (u32 i = 0; i < frames; ++i) {
        const f32 env = 1.0f - static_cast<f32>(i) / static_cast<f32>(frames);
        const f32 s = std::sin(kTwoPi * freqHz * (static_cast<f32>(i) / 48000.0f)) * env * 0.45f;
        pcm[i * 2U + 0] = s;
        pcm[i * 2U + 1] = s;
    }
    std::vector<u8> bytes(pcm.size() * sizeof(f32));
    std::memcpy(bytes.data(), pcm.data(), bytes.size());
    return bytes;
}

std::vector<u8> BuildSceneManifest(AssetId colId) {
    std::vector<u8> buf(128, 0);
    BinaryWriter bw(buf);
    CookedSceneManifestHeader hdr{};
    hdr.magic = kCookedSceneManifestMagic;
    hdr.version = 1;
    hdr.assetCount = 2;
    hdr.collisionMapId = colId.value;
    hdr.cueBankId = kCueBankMainId.value;
    (void)bw.Write(hdr);

    CookedSceneAssetEntry e0{kRigPlayerId.value, AssetKind::Blob, AssetResidency::Sim, 1, 0};
    CookedSceneAssetEntry e1{kRigEnemyId.value, AssetKind::Blob, AssetResidency::Sim, 1, 0};
    (void)bw.Write(e0);
    (void)bw.Write(e1);
    buf.resize(bw.Offset());
    return buf;
}
} // namespace

usize VeilAssets::BuildBootPackage(std::span<u8> outBuffer) noexcept {
    std::vector<EnPakEntry> entries{};
    std::vector<u8> payloads{};

    // Generate a 128x16 RGBA32 procedural pixel-art sprite sheet (8 sprites of 16x16 pixels each)
    constexpr u16 kSheetW = 128;
    constexpr u16 kSheetH = 16;
    std::vector<u8> texPixels(static_cast<usize>(kSheetW) * kSheetH * 4U, 0);
    auto setPx = [&](u32 frameIdx, i32 x, i32 y, u8 r, u8 g, u8 b, u8 a = 255) {
        if (x < 0 || x >= 16 || y < 0 || y >= 16 || frameIdx >= 8U) return;
        const usize px = static_cast<usize>(frameIdx * 16U + static_cast<u32>(x));
        const usize off = (static_cast<usize>(y) * kSheetW + px) * 4U;
        texPixels[off + 0] = r;
        texPixels[off + 1] = g;
        texPixels[off + 2] = b;
        texPixels[off + 3] = a;
    };

    // Handcrafted 16x16 pixel-art sprite matrices (0..7) with rich multi-tone palette
    auto drawPaletteSprite = [&](u32 frameIdx, const char* const rows[16]) {
        for (i32 y = 0; y < 16; ++y) {
            const char* row = rows[y];
            for (i32 x = 0; x < 16 && row[x] != '\0'; ++x) {
                const char c = row[x];
                if (c == '.') continue;
                switch (c) {
                    case 'D': setPx(frameIdx, x, y, 22, 28, 48); break;     // Dark indigo outline/shadow
                    case 'B': setPx(frameIdx, x, y, 42, 62, 108); break;    // Shinobi tunic blue
                    case 'L': setPx(frameIdx, x, y, 72, 108, 168); break;   // Tunic highlight
                    case 'C': setPx(frameIdx, x, y, 70, 250, 255); break;   // Glowing cyan visor/energy
                    case 'W': setPx(frameIdx, x, y, 235, 250, 255); break;  // Blade white/silver edge
                    case 'S': setPx(frameIdx, x, y, 165, 190, 215); break;  // Steel gray
                    case 'R': setPx(frameIdx, x, y, 230, 45, 75); break;    // Crimson scarf / Ronin armor
                    case 'M': setPx(frameIdx, x, y, 145, 24, 42); break;    // Deep maroon armor shadow
                    case 'G': setPx(frameIdx, x, y, 250, 205, 70); break;   // Gold trim / horns / hilt
                    case 'O': setPx(frameIdx, x, y, 255, 120, 45); break;   // Fiery orange eye/glow
                    case 'E': setPx(frameIdx, x, y, 48, 145, 115); break;   // Emerald archer cloak
                    case 'T': setPx(frameIdx, x, y, 28, 88, 72); break;     // Dark forest cloak shadow
                    case 'K': setPx(frameIdx, x, y, 195, 155, 90); break;   // Straw kasa hat / wood
                    case 'J': setPx(frameIdx, x, y, 80, 255, 210); break;   // Jade shrine spirit flame
                    case 'P': setPx(frameIdx, x, y, 115, 130, 152); break;  // Stone shrine light
                    case 'Q': setPx(frameIdx, x, y, 68, 80, 98); break;     // Stone shrine dark
                    default: break;
                }
            }
        }
    };

    // Frame 0: Shinobi Hero Idle (hooded silhouette, cyan visor, flowing red scarf, katana & separate legs)
    static constexpr const char* kHeroIdle[16] = {
        ".....DDDDDD.....",
        "....DBBBBBBD....",
        "....DBBCCWBD....",
        "....DBBBBBBD..W.",
        "..RRDDDDDDDD.WS.",
        ".R..DBLLLLBD.S..",
        "R...DBBBBBBDWS..",
        "....DBBGGGBDGD..",
        "....DBBBBBBD....",
        "....DBB..BBD....",
        "....DBB..BBD....",
        "....DDD..DDD....",
        "....DRD..DRD....",
        "....DDD..DDD....",
        "...DDDD..DDDD...",
        "................"
    };
    drawPaletteSprite(0, kHeroIdle);

    // Frame 1: Shinobi Hero Slash (forward lunge, extended glowing katana + crescent arc)
    static constexpr const char* kHeroSlash[16] = {
        "......DDDDDD..CW",
        ".....DBBBBBBD.CW",
        ".....DBBCCWBD..C",
        ".RRR.DBBBBBBD..W",
        "R....DDDDDDDDWWW",
        ".....DBLLLLBGGWW",
        ".....DBBBBBBD..C",
        ".....DBGGGGBD..C",
        "....DDBBBBBBD...",
        "...DDBB...BBD...",
        "...DDB....BBD...",
        "...DDD....DDD...",
        "..DDRD....DRDD..",
        "..DDDD....DDDD..",
        "................",
        "................"
    };
    drawPaletteSprite(1, kHeroSlash);

    // Frame 2: Corrupted Ronin Scout (golden crescent horns, kabuto helmet, shoulder armor, nodachi sword)
    static constexpr const char* kRonin[16] = {
        "..G..........G..",
        "...G.DDDDDD.G...",
        "..DDDDMMMMDDDD..",
        "...DDMOCCMDD..W.",
        "....DMMMMMMD.WS.",
        "..GGRRRRRRRRGWS.",
        "..DDRRMMMMRRDWS.",
        "..DDRRGGGGMRDGD.",
        "...DRRMMMMRRD...",
        "...DMMMMMMMMD...",
        "....DMM..MMD....",
        "....DMM..MMD....",
        "....DDD..DDD....",
        "....DMD..DMD....",
        "...DDDD..DDDD...",
        "................"
    };
    drawPaletteSprite(2, kRonin);

    // Frame 3: Ash Archer (wide conical kasa straw hat, emerald cloak, drawn bow & spirit arrow)
    static constexpr const char* kArcher[16] = {
        "......KKKK......",
        "....KKKKKKKK....",
        "..KKKKKKKKKKKK..",
        ".DDDDDDDDDDDDDD.",
        "....DTTECCED..G.",
        "....DEEEEEED.G.G",
        "...DDEEEEEEDCCCC",
        "...DTEEEEEED.G.G",
        "...DTEEGGEED..G.",
        "...DTEETTTED....",
        "....DEE..EED....",
        "....DTT..TTD....",
        "....DDD..DDD....",
        "....DKD..DKD....",
        "...DDDD..DDDD...",
        "................"
    };
    drawPaletteSprite(3, kArcher);

    // Frame 4: The Silent Warden Boss (towering golden horns, 3-eyed oni mask, gilded pauldrons & halberd)
    static constexpr const char* kWardenBoss[16] = {
        ".GG..........GG.",
        ".GG..DDDDDD..GG.",
        "..G.DMMMMMMD.G.W",
        "..GDDMOGGOMDDGCW",
        ".GGGDMOOOOOMDGGW",
        "GGRRRMMMMMMRRRGS",
        "DDRRGGGGGGGGMRDS",
        "DDRRMGMMMMGMRRDS",
        ".DRRGGGGGGGGMRD.",
        ".DRRMMMMMMMMRRD.",
        "..DMMMMMMMMMMD..",
        "..DMMM....MMMD..",
        "..DMMM....MMMD..",
        "..DGGD....DGGD..",
        ".DDDDD....DDDDD.",
        "................"
    };
    drawPaletteSprite(4, kWardenBoss);

    // Frame 5: Sanctuary Shrine Lantern (curved pagoda roof, glowing jade spirit flame, stone pedestal)
    static constexpr const char* kShrine[16] = {
        "......DDDD......",
        "....DDPPPPDD....",
        ".DDDPPPPPPPPDDD.",
        "DDDDQQQQQQQQDDDD",
        "...DQPJJJJPQD...",
        "...DQPJCCJPQD...",
        "...DQPJWWJPQD...",
        "...DQPJCCJPQD...",
        "...DQPJJJJPQD...",
        "...DQQPPPPQQD...",
        "....DQPPPPQD....",
        "....DQQPPQQD....",
        "...DDQQQQQQDD...",
        "..DDPPPPPPPPDD..",
        ".DDDDDDDDDDDDDD.",
        "................"
    };
    drawPaletteSprite(5, kShrine);

    // Frame 6: Spirit Shuriken (4-pointed ninja star with cyan core)
    static constexpr const char* kShuriken[16] = {
        ".......WW.......",
        "......WSSW......",
        "......WSSW......",
        ".....WSSSSW.....",
        "....WSSCCSSW....",
        "...WSSCCCCSSW...",
        "WWSSSSCC..CCSSWW",
        "WSSSSSC....CSSSW",
        "WSSSSSC....CSSSW",
        "WWSSSSCC..CCSSWW",
        "...WSSCCCCSSW...",
        "....WSSCCSSW....",
        ".....WSSSSW.....",
        "......WSSW......",
        "......WSSW......",
        ".......WW......."
    };
    drawPaletteSprite(6, kShuriken);

    // Frame 7: Ancient Temple Stone Brick Tile (ashlar masonry with highlights and bevels)
    for (i32 y = 0; y < 16; ++y) {
        for (i32 x = 0; x < 16; ++x) {
            const bool mortar = (y == 0 || y == 8 || (y < 8 && x == 0) || (y >= 8 && x == 8));
            const bool topEdge = (y == 1 || y == 9);
            const bool botEdge = (y == 7 || y == 15);
            if (mortar) setPx(7, x, y, 20, 26, 40);
            else if (topEdge) setPx(7, x, y, 82, 108, 142);
            else if (botEdge) setPx(7, x, y, 36, 48, 68);
            else setPx(7, x, y, 52, 68, 96);
        }
    }

    AppendEntry(entries, payloads, kTexMainAtlasId, AssetKind::Texture, texPixels, kSheetW, kSheetH);

    std::vector<CookedAtlasSpriteEntry> atlasEntries(8);
    for (u32 i = 0; i < 8U; ++i) {
        const f32 u0 = (static_cast<f32>(i) * 16.0f + 0.35f) / 128.0f;
        const f32 u1 = (static_cast<f32>(i + 1U) * 16.0f - 0.35f) / 128.0f;
        atlasEntries[i] = CookedAtlasSpriteEntry{u0, 0.0f, u1, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f, i + 1U, 0};
    }
    AppendEntry(
        entries, payloads, kAtlasMainId, AssetKind::Atlas,
        std::span<const u8>(reinterpret_cast<const u8*>(atlasEntries.data()), atlasEntries.size() * sizeof(CookedAtlasSpriteEntry)));

    const auto colCourtyard = BuildCollisionMap(64, 24, false);
    const auto colSanctum = BuildCollisionMap(48, 24, true);
    AppendEntry(entries, payloads, kColCourtyardId, AssetKind::Blob, colCourtyard);
    AppendEntry(entries, payloads, kColSanctumId, AssetKind::Blob, colSanctum);

    const auto rigPlayer = BuildCombatRig(true);
    const auto rigEnemy = BuildCombatRig(false);
    AppendEntry(entries, payloads, kRigPlayerId, AssetKind::Blob, rigPlayer);
    AppendEntry(entries, payloads, kRigEnemyId, AssetKind::Blob, rigEnemy);

    const auto pcmSlash = BuildSynthWave(520.0f, 1024);
    const auto pcmParry = BuildSynthWave(880.0f, 1024);
    const auto pcmHit = BuildSynthWave(240.0f, 1024);
    AppendEntry(entries, payloads, kClipSlashId, AssetKind::Audio, pcmSlash, 48000, 2);
    AppendEntry(entries, payloads, kClipParryId, AssetKind::Audio, pcmParry, 48000, 2);
    AppendEntry(entries, payloads, kClipHitId, AssetKind::Audio, pcmHit, 48000, 2);

    std::vector<u8> cueBankBuf(256, 0);
    BinaryWriter cbw(cueBankBuf);
    CookedAudioBankHeader chdr{kCookedAudioBankMagic, 1, 3, 0, 0};
    (void)cbw.Write(chdr);
    SoundCueDef c0{};
    c0.cueId = kClipSlashId;
    c0.clipHandle = AudioHandle{1, 1};
    SoundCueDef c1 = c0;
    c1.cueId = kClipParryId;
    c1.clipHandle = AudioHandle{2, 1};
    SoundCueDef c2 = c0;
    c2.cueId = kClipHitId;
    c2.clipHandle = AudioHandle{3, 1};
    (void)cbw.Write(c0);
    (void)cbw.Write(c1);
    (void)cbw.Write(c2);
    cueBankBuf.resize(cbw.Offset());
    AppendEntry(entries, payloads, kCueBankMainId, AssetKind::Blob, cueBankBuf);

    const auto scnCourtyard = BuildSceneManifest(kColCourtyardId);
    const auto scnSanctum = BuildSceneManifest(kColSanctumId);
    AppendEntry(entries, payloads, kSceneCourtyardId, AssetKind::Blob, scnCourtyard);
    AppendEntry(entries, payloads, kSceneBossSanctumId, AssetKind::Blob, scnSanctum);

    const usize headerAndToc = sizeof(EnPakHeader) + entries.size() * sizeof(EnPakEntry);
    for (EnPakEntry& e : entries) {
        e.offset += static_cast<u32>(headerAndToc);
    }

    BinaryWriter finalWriter(outBuffer);
    EnPakHeader pakHdr{kEnPakMagic, 1, static_cast<u32>(entries.size()), 0};
    (void)finalWriter.Write(pakHdr);
    (void)finalWriter.WriteBytes(
        std::span<const u8>(reinterpret_cast<const u8*>(entries.data()), entries.size() * sizeof(EnPakEntry)));
    (void)finalWriter.WriteBytes(payloads);
    return finalWriter.Offset();
}

} // namespace en::veil
