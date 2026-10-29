#include "assets/ImageLoader.h"

#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace en {

namespace {

[[nodiscard]] constexpr u32 ReadBeU32(const u8* p) noexcept {
    return (static_cast<u32>(p[0]) << 24U) |
           (static_cast<u32>(p[1]) << 16U) |
           (static_cast<u32>(p[2]) << 8U) |
           static_cast<u32>(p[3]);
}

[[nodiscard]] constexpr u16 ReadLeU16(const u8* p) noexcept {
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8U));
}

[[nodiscard]] constexpr u32 ReadLeU32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8U) |
           (static_cast<u32>(p[2]) << 16U) |
           (static_cast<u32>(p[3]) << 24U);
}

[[nodiscard]] constexpr u8 PaethPredictor(u8 a, u8 b, u8 c) noexcept {
    const i32 p = static_cast<i32>(a) + static_cast<i32>(b) - static_cast<i32>(c);
    const i32 pa = (p >= static_cast<i32>(a)) ? (p - static_cast<i32>(a)) : (static_cast<i32>(a) - p);
    const i32 pb = (p >= static_cast<i32>(b)) ? (p - static_cast<i32>(b)) : (static_cast<i32>(b) - p);
    const i32 pc = (p >= static_cast<i32>(c)) ? (p - static_cast<i32>(c)) : (static_cast<i32>(c) - p);
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

// Reconstructs PNG scanline in-place inside row[] using left neighbor and previous row.
[[nodiscard]] bool UnfilterScanlineInPlace(
    u8 filterType,
    u8* row,
    const u8* prevRow,
    usize strideBytes) noexcept {
    constexpr usize kBpp = 4U;
    switch (filterType) {
        case 0U:
            return true;
        case 1U:
            for (usize i = kBpp; i < strideBytes; ++i) {
                row[i] = static_cast<u8>(row[i] + row[i - kBpp]);
            }
            return true;
        case 2U:
            if (prevRow != nullptr) {
                for (usize i = 0U; i < strideBytes; ++i) {
                    row[i] = static_cast<u8>(row[i] + prevRow[i]);
                }
            }
            return true;
        case 3U:
            for (usize i = 0U; i < strideBytes; ++i) {
                const u32 left = (i >= kBpp) ? row[i - kBpp] : 0U;
                const u32 up = (prevRow != nullptr) ? prevRow[i] : 0U;
                row[i] = static_cast<u8>(row[i] + ((left + up) >> 1U));
            }
            return true;
        case 4U:
            for (usize i = 0U; i < strideBytes; ++i) {
                const u8 left = (i >= kBpp) ? row[i - kBpp] : 0U;
                const u8 up = (prevRow != nullptr) ? prevRow[i] : 0U;
                const u8 upLeft = (prevRow != nullptr && i >= kBpp) ? prevRow[i - kBpp] : 0U;
                row[i] = static_cast<u8>(row[i] + PaethPredictor(left, up, upLeft));
            }
            return true;
        default:
            return false;
    }
}

} // namespace

bool ImageLoader::LoadPngRgba(
    const char* filePath,
    u16 expectedWidth,
    u16 expectedHeight,
    std::span<u8> outPixels) noexcept {
    const usize expectedByteSize = static_cast<usize>(expectedWidth) * expectedHeight * 4U;
    if (filePath == nullptr || expectedWidth == 0U || expectedHeight == 0U ||
        outPixels.size() < expectedByteSize) {
        return false;
    }

    std::FILE* fp = std::fopen(filePath, "rb");
    if (fp == nullptr) return false;

    static constexpr std::array<u8, 8> kPngSignature{{0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU}};
    std::array<u8, 8> sig{};
    if (std::fread(sig.data(), 1U, sig.size(), fp) != sig.size() || sig != kPngSignature) {
        std::fclose(fp);
        return false;
    }

    bool ihdrValid = false;
    bool zlibHeaderParsed = false;
    bool finalBlockSeen = false;
    u16 blockRemaining = 0U;

    const usize strideBytes = static_cast<usize>(expectedWidth) * 4U;
    usize rowColPos = 0U;
    u16 currentRow = 0U;
    u8 curFilter = 0U;

    u8 zlibHdrBytes[2]{};
    u8 zlibHdrRead = 0U;
    u8 blockHdrBytes[5]{};
    u8 blockHdrRead = 0U;

    const auto feedIdatByte = [&](u8 byteVal) noexcept -> bool {
        if (!zlibHeaderParsed) {
            zlibHdrBytes[zlibHdrRead++] = byteVal;
            if (zlibHdrRead == 2U) {
                const u16 cmfFlg = static_cast<u16>((static_cast<u16>(zlibHdrBytes[0]) << 8U) | zlibHdrBytes[1]);
                if ((zlibHdrBytes[0] & 0x0FU) != 8U || (cmfFlg % 31U) != 0U || (zlibHdrBytes[1] & 0x20U) != 0U) {
                    return false;
                }
                zlibHeaderParsed = true;
            }
            return true;
        }

        if (currentRow >= expectedHeight) return true;

        if (blockRemaining == 0U) {
            if (finalBlockSeen) return true;
            blockHdrBytes[blockHdrRead++] = byteVal;
            if (blockHdrRead == 5U) {
                const u8 bheader = blockHdrBytes[0];
                if (((bheader >> 1U) & 0x03U) != 0U) return false;
                finalBlockSeen = ((bheader & 0x01U) != 0U);
                const u16 len = ReadLeU16(&blockHdrBytes[1]);
                const u16 nlen = ReadLeU16(&blockHdrBytes[3]);
                if (static_cast<u16>(len ^ 0xFFFFU) != nlen) return false;
                blockRemaining = len;
                blockHdrRead = 0U;
            }
            return true;
        }

        --blockRemaining;
        if (rowColPos == 0U) {
            curFilter = byteVal;
            rowColPos = 1U;
            return true;
        }

        u8* dstRow = outPixels.data() + static_cast<usize>(currentRow) * strideBytes;
        dstRow[rowColPos - 1U] = byteVal;
        if (rowColPos == strideBytes) {
            const u8* prevRow = (currentRow > 0U) ? (dstRow - strideBytes) : nullptr;
            if (!UnfilterScanlineInPlace(curFilter, dstRow, prevRow, strideBytes)) {
                return false;
            }
            rowColPos = 0U;
            ++currentRow;
        } else {
            ++rowColPos;
        }
        return true;
    };

    std::array<u8, 4096> chunkBuf{};
    while (true) {
        std::array<u8, 8> chunkHdr{};
        if (std::fread(chunkHdr.data(), 1U, chunkHdr.size(), fp) != chunkHdr.size()) break;
        const u32 chunkLen = ReadBeU32(chunkHdr.data());
        const u32 chunkType = ReadBeU32(chunkHdr.data() + 4U);
        if (chunkLen > 0x3FFFFFFFU) {
            std::fclose(fp);
            return false;
        }

        if (chunkType == 0x49484452U) {
            std::array<u8, 13> ihdr{};
            if (chunkLen != 13U || std::fread(ihdr.data(), 1U, ihdr.size(), fp) != ihdr.size() ||
                std::fseek(fp, 4L, SEEK_CUR) != 0) {
                std::fclose(fp);
                return false;
            }
            const u32 w = ReadBeU32(&ihdr[0]);
            const u32 h = ReadBeU32(&ihdr[4]);
            if (w != expectedWidth || h != expectedHeight ||
                ihdr[8] != 8U || ihdr[9] != 6U || ihdr[12] != 0U) {
                std::fclose(fp);
                return false;
            }
            ihdrValid = true;
        } else if (chunkType == 0x49444154U) {
            if (!ihdrValid) {
                std::fclose(fp);
                return false;
            }
            u32 remaining = chunkLen;
            while (remaining > 0U) {
                const u32 toRead = std::min(remaining, static_cast<u32>(chunkBuf.size()));
                if (std::fread(chunkBuf.data(), 1U, toRead, fp) != toRead) {
                    std::fclose(fp);
                    return false;
                }
                for (u32 i = 0U; i < toRead; ++i) {
                    if (!feedIdatByte(chunkBuf[i])) {
                        std::fclose(fp);
                        return false;
                    }
                }
                remaining -= toRead;
            }
            if (std::fseek(fp, 4L, SEEK_CUR) != 0) {
                std::fclose(fp);
                return false;
            }
        } else if (chunkType == 0x49454E44U) {
            break;
        } else {
            if (std::fseek(fp, static_cast<long>(chunkLen) + 4L, SEEK_CUR) != 0) {
                std::fclose(fp);
                return false;
            }
        }
    }

    std::fclose(fp);
    return ihdrValid && (currentRow == expectedHeight);
}

bool ImageLoader::LoadBmpRgba(
    const char* filePath,
    u16 expectedWidth,
    u16 expectedHeight,
    std::span<u8> outPixels) noexcept {
    const usize expectedByteSize = static_cast<usize>(expectedWidth) * expectedHeight * 4U;
    if (filePath == nullptr || expectedWidth == 0U || expectedHeight == 0U ||
        outPixels.size() < expectedByteSize) {
        return false;
    }

    std::FILE* fp = std::fopen(filePath, "rb");
    if (fp == nullptr) return false;

    std::array<u8, 54> hdr{};
    if (std::fread(hdr.data(), 1U, hdr.size(), fp) != hdr.size() ||
        hdr[0] != 'B' || hdr[1] != 'M') {
        std::fclose(fp);
        return false;
    }

    const u32 pixelOffset = ReadLeU32(&hdr[10]);
    const i32 width = static_cast<i32>(ReadLeU32(&hdr[18]));
    const i32 rawHeight = static_cast<i32>(ReadLeU32(&hdr[22]));
    const bool topDown = (rawHeight < 0);
    const u32 absHeight = topDown ? (0U - static_cast<u32>(rawHeight)) : static_cast<u32>(rawHeight);
    const u16 bpp = ReadLeU16(&hdr[28]);
    const u32 compression = ReadLeU32(&hdr[30]);

    if (width != static_cast<i32>(expectedWidth) ||
        absHeight != static_cast<u32>(expectedHeight) ||
        bpp != 32U ||
        (compression != 0U && compression != 3U) ||
        std::fseek(fp, static_cast<long>(pixelOffset), SEEK_SET) != 0) {
        std::fclose(fp);
        return false;
    }

    const usize stride = static_cast<usize>(expectedWidth) * 4U;
    for (u16 r = 0U; r < expectedHeight; ++r) {
        const u16 dstY = topDown ? r : static_cast<u16>(expectedHeight - 1U - r);
        u8* rowPtr = outPixels.data() + static_cast<usize>(dstY) * stride;
        if (std::fread(rowPtr, 1U, stride, fp) != stride) {
            std::fclose(fp);
            return false;
        }
        for (u16 x = 0U; x < expectedWidth; ++x) {
            std::swap(rowPtr[x * 4U + 0U], rowPtr[x * 4U + 2U]);
        }
    }

    std::fclose(fp);
    return true;
}

bool ImageLoader::LoadRgbaImageFromDisk(
    std::string_view primaryPngPath,
    std::string_view fallbackBmpPath,
    u16 expectedWidth,
    u16 expectedHeight,
    std::span<u8> outPixels) noexcept {
    static constexpr std::array<const char*, 3> kPrefixes{{"", "../", "../../"}};
    char pathBuf[512]{};

    const auto tryPrefix = [&](const char* base, const char* rel) noexcept -> bool {
        if (!primaryPngPath.empty()) {
            std::snprintf(
                pathBuf, sizeof(pathBuf), "%s%s%.*s",
                base, rel, static_cast<int>(primaryPngPath.size()), primaryPngPath.data());
            if (LoadPngRgba(pathBuf, expectedWidth, expectedHeight, outPixels)) return true;
        }
        if (!fallbackBmpPath.empty()) {
            std::snprintf(
                pathBuf, sizeof(pathBuf), "%s%s%.*s",
                base, rel, static_cast<int>(fallbackBmpPath.size()), fallbackBmpPath.data());
            if (LoadBmpRgba(pathBuf, expectedWidth, expectedHeight, outPixels)) return true;
        }
        return false;
    };

    for (const char* prefix : kPrefixes) {
        if (tryPrefix("", prefix)) return true;
    }

    if (const char* sdlBase = SDL_GetBasePath()) {
        for (const char* prefix : kPrefixes) {
            if (tryPrefix(sdlBase, prefix)) return true;
        }
    }
    return false;
}

} // namespace en
