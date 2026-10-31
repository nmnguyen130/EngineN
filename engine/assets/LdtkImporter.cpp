#include "assets/LdtkImporter.h"

#include "core/Serialization.h"

namespace en {
namespace {

constexpr void SkipWs(std::string_view s, usize& pos) noexcept {
    while (pos < s.size()) {
        const char c = s[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos;
        else break;
    }
}

[[nodiscard]] constexpr bool ParseI32(std::string_view s, usize& pos, i32& out) noexcept {
    SkipWs(s, pos);
    if (pos >= s.size()) return false;
    i32 sign = 1;
    if (s[pos] == '-') {
        sign = -1;
        ++pos;
    }
    if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return false;
    i32 val = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        val = val * 10 + static_cast<i32>(s[pos] - '0');
        ++pos;
    }
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    }
    out = val * sign;
    return true;
}

[[nodiscard]] constexpr bool FindKeyAfter(
    std::string_view s, std::string_view key, usize startPos, usize& valPos) noexcept {
    usize search = startPos;
    while (search < s.size()) {
        const usize found = s.find(key, search);
        if (found == std::string_view::npos) return false;
        usize p = found + key.size();
        SkipWs(s, p);
        if (p < s.size() && s[p] == ':') {
            valPos = p + 1U;
            return true;
        }
        search = found + key.size();
    }
    return false;
}

[[nodiscard]] constexpr bool ParseQuotedString(
    std::string_view s, usize& pos, std::string_view& out) noexcept {
    SkipWs(s, pos);
    if (pos >= s.size() || s[pos] != '"') return false;
    const usize start = ++pos;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1U < s.size()) pos += 2U;
        else ++pos;
    }
    if (pos >= s.size()) return false;
    out = s.substr(start, pos - start);
    ++pos;
    return true;
}

} // namespace

Result<LdtkLevelView, AssetErrc> LdtkImporter::ParseLevelJson(
    std::string_view json,
    Arena& arena,
    f32 pixelsPerMeter,
    u32 maxEntities) noexcept {
    if (json.empty() || pixelsPerMeter <= 0.0f) return Err(AssetErrc::DecodeFailed);

    i32 cWid     = 0;
    i32 cHei     = 0;
    i32 gridSize = 16;
    i32 worldX   = 0;
    i32 worldY   = 0;

    usize pos = 0U;
    if (FindKeyAfter(json, "\"worldX\"", 0U, pos)) (void)ParseI32(json, pos, worldX);
    if (FindKeyAfter(json, "\"worldY\"", 0U, pos)) (void)ParseI32(json, pos, worldY);
    if (!FindKeyAfter(json, "\"__cWid\"", 0U, pos) || !ParseI32(json, pos, cWid) || cWid <= 0 || cWid > 1024) {
        return Err(AssetErrc::DecodeFailed);
    }
    if (!FindKeyAfter(json, "\"__cHei\"", 0U, pos) || !ParseI32(json, pos, cHei) || cHei <= 0 || cHei > 1024) {
        return Err(AssetErrc::DecodeFailed);
    }
    if (FindKeyAfter(json, "\"__gridSize\"", 0U, pos)) {
        (void)ParseI32(json, pos, gridSize);
        if (gridSize <= 0) gridSize = 16;
    }

    const u32 mark      = arena.Mark();
    const usize tileCnt = static_cast<usize>(cWid) * static_cast<usize>(cHei);
    auto tiles          = arena.Alloc<TileFlag>(tileCnt);
    auto entities       = arena.Alloc<LdtkEntityDesc>(maxEntities);
    if (tiles.empty() || (maxEntities > 0U && entities.empty())) {
        arena.Rollback(mark);
        return Err(AssetErrc::OutOfMemory);
    }
    std::fill(tiles.begin(), tiles.end(), TileFlag::Empty);

    // Parse "intGridCsv": [ ... ] and flip Y so row 0 in LDtk (top) becomes ty = cHei - 1 (top in Y-up world).
    if (FindKeyAfter(json, "\"intGridCsv\"", 0U, pos)) {
        SkipWs(json, pos);
        if (pos < json.size() && json[pos] == '[') {
            ++pos;
            for (i32 row = 0; row < cHei; ++row) {
                const i32 ty = (cHei - 1) - row;
                for (i32 col = 0; col < cWid; ++col) {
                    SkipWs(json, pos);
                    if (pos < json.size() && json[pos] == ',') ++pos;
                    SkipWs(json, pos);
                    if (pos >= json.size() || json[pos] == ']') break;
                    i32 cellVal = 0;
                    if (!ParseI32(json, pos, cellVal)) break;
                    tiles[static_cast<usize>(ty) * static_cast<usize>(cWid) + static_cast<usize>(col)] =
                        MapIntGridValue(cellVal);
                }
            }
        }
    }

    const f32 invPpm       = 1.0f / pixelsPerMeter;
    const f32 tileMeters   = static_cast<f32>(gridSize) * invPpm;
    const f32 levelHeightM = static_cast<f32>(cHei * gridSize) * invPpm;
    const f32 originX      = static_cast<f32>(worldX) * invPpm;
    const f32 originY      = static_cast<f32>(worldY) * invPpm;

    u32 entCount = 0U;
    usize entSearch = 0U;
    if (FindKeyAfter(json, "\"entityInstances\"", 0U, entSearch)) {
        usize cur = entSearch;
        while (entCount < maxEntities) {
            usize idPos = 0U;
            if (!FindKeyAfter(json, "\"__identifier\"", cur, idPos)) break;
            std::string_view identStr{};
            if (!ParseQuotedString(json, idPos, identStr)) break;

            const usize nextEnt = json.find("\"__identifier\"", idPos);
            const std::string_view slice = (nextEnt != std::string_view::npos)
                ? json.substr(idPos, nextEnt - idPos)
                : json.substr(idPos);

            i32 pxX = 0, pxY = 0;
            i32 wid = gridSize, hei = gridSize;
            i32 customVal = 0;
            std::string_view iidStr{};

            usize subPos = 0U;
            if (FindKeyAfter(slice, "\"iid\"", 0U, subPos)) {
                (void)ParseQuotedString(slice, subPos, iidStr);
            }
            if (FindKeyAfter(slice, "\"width\"", 0U, subPos)) (void)ParseI32(slice, subPos, wid);
            if (FindKeyAfter(slice, "\"height\"", 0U, subPos)) (void)ParseI32(slice, subPos, hei);
            if (FindKeyAfter(slice, "\"__value\"", 0U, subPos)) (void)ParseI32(slice, subPos, customVal);

            if (FindKeyAfter(slice, "\"px\"", 0U, subPos)) {
                SkipWs(slice, subPos);
                if (subPos < slice.size() && slice[subPos] == '[') {
                    ++subPos;
                    (void)ParseI32(slice, subPos, pxX);
                    SkipWs(slice, subPos);
                    if (subPos < slice.size() && slice[subPos] == ',') ++subPos;
                    (void)ParseI32(slice, subPos, pxY);
                }
            }

            entities[entCount++] = LdtkEntityDesc{
                .identifier = StringId::Hash(identStr),
                .worldPos   = {originX + static_cast<f32>(pxX) * invPpm,
                               originY + (levelHeightM - static_cast<f32>(pxY) * invPpm)},
                .size       = {static_cast<f32>(wid) * invPpm, static_cast<f32>(hei) * invPpm},
                .iidHash    = static_cast<u32>(StringId::Hash(iidStr).value & 0xFFFFFFFFULL),
                .customInt  = customVal,
            };

            if (nextEnt == std::string_view::npos) break;
            cur = nextEnt;
        }
    }

    return LdtkLevelView{
        .tiles          = tiles,
        .entities       = {entities.data(), entCount},
        .worldBounds    = {originX, originY, static_cast<f32>(cWid) * tileMeters, levelHeightM},
        .widthTiles     = static_cast<u16>(cWid),
        .heightTiles    = static_cast<u16>(cHei),
        .tileSizeMeters = tileMeters,
    };
}

Result<usize, AssetErrc> LdtkImporter::CookTileMapBlob(
    const LdtkLevelView& level,
    std::span<u8> outBlob) noexcept {
    const usize count  = static_cast<usize>(level.widthTiles) * level.heightTiles;
    const usize needed = sizeof(TileMapHeader) + count * sizeof(TileFlag);
    if (count == 0U || level.tiles.size() < count) return Err(AssetErrc::DecodeFailed);
    if (outBlob.size() < needed) return Err(AssetErrc::OutOfMemory);

    BinaryWriter writer(outBlob);
    const TileMapHeader hdr{
        .magic    = kTileMapMagic,
        .version  = 1U,
        .width    = level.widthTiles,
        .height   = level.heightTiles,
        .tileSize = level.tileSizeMeters,
        .origin   = {level.worldBounds.x, level.worldBounds.y},
    };
    if (!writer.Write(hdr)) return Err(AssetErrc::OutOfMemory);
    if (!writer.WriteBytes({reinterpret_cast<const u8*>(level.tiles.data()), count})) {
        return Err(AssetErrc::OutOfMemory);
    }
    return writer.Offset();
}

} // namespace en
