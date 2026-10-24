#include "game/assets/ImageLoader.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vots {

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

[[nodiscard]] constexpr i32 ReadLeI32(const u8* p) noexcept {
    return static_cast<i32>(ReadLeU32(p));
}

[[nodiscard]] constexpr u8 PaethPredictor(u8 a, u8 b, u8 c) noexcept {
    const i32 p = static_cast<i32>(a) + static_cast<i32>(b) - static_cast<i32>(c);
    const i32 pa = (p >= static_cast<i32>(a)) ? (p - static_cast<i32>(a)) : (static_cast<i32>(a) - p);
    const i32 pb = (p >= static_cast<i32>(b)) ? (p - static_cast<i32>(b)) : (static_cast<i32>(b) - p);
    const i32 pc = (p >= static_cast<i32>(c)) ? (p - static_cast<i32>(c)) : (static_cast<i32>(c) - p);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

[[nodiscard]] bool UnfilterScanline(
    u8 filterType,
    const u8* rawLine,
    const u8* prevReconLine,
    u8* outReconLine,
    usize strideBytes) noexcept {
    constexpr usize kBpp = 4U;
    switch (filterType) {
        case 0U: // None
            std::memcpy(outReconLine, rawLine, strideBytes);
            return true;
        case 1U: // Sub
            for (usize i = 0U; i < strideBytes; ++i) {
                const u8 left = (i >= kBpp) ? outReconLine[i - kBpp] : 0U;
                outReconLine[i] = static_cast<u8>(rawLine[i] + left);
            }
            return true;
        case 2U: // Up
            for (usize i = 0U; i < strideBytes; ++i) {
                const u8 up = (prevReconLine != nullptr) ? prevReconLine[i] : 0U;
                outReconLine[i] = static_cast<u8>(rawLine[i] + up);
            }
            return true;
        case 3U: // Average
            for (usize i = 0U; i < strideBytes; ++i) {
                const u32 left = (i >= kBpp) ? outReconLine[i - kBpp] : 0U;
                const u32 up = (prevReconLine != nullptr) ? prevReconLine[i] : 0U;
                outReconLine[i] = static_cast<u8>(rawLine[i] + ((left + up) >> 1U));
            }
            return true;
        case 4U: // Paeth
            for (usize i = 0U; i < strideBytes; ++i) {
                const u8 left = (i >= kBpp) ? outReconLine[i - kBpp] : 0U;
                const u8 up = (prevReconLine != nullptr) ? prevReconLine[i] : 0U;
                const u8 upLeft = (prevReconLine != nullptr && i >= kBpp) ? prevReconLine[i - kBpp] : 0U;
                outReconLine[i] = static_cast<u8>(rawLine[i] + PaethPredictor(left, up, upLeft));
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
    if (filePath == nullptr || outPixels.size() < expectedByteSize) {
        return false;
    }

    std::FILE* fp = std::fopen(filePath, "rb");
    if (fp == nullptr) {
        return false;
    }

    static constexpr std::array<u8, 8> kPngSignature{{0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU}};
    std::array<u8, 8> sig{};
    if (std::fread(sig.data(), 1U, sig.size(), fp) != sig.size() || sig != kPngSignature) {
        std::fclose(fp);
        return false;
    }

    bool ihdrValid = false;
    bool zlibHeaderParsed = false;
    u16 blockRemaining = 0U;
    bool finalBlockSeen = false;

    const usize strideBytes = static_cast<usize>(expectedWidth) * 4U;
    const usize scanlineBytes = strideBytes + 1U;
    std::array<u8, 4096> curScanline{};
    if (scanlineBytes > curScanline.size()) {
        std::fclose(fp);
        return false;
    }

    usize scanlineWritePos = 0U;
    u16 currentRow = 0U;

    // Stream-decode stored DEFLATE blocks directly from IDAT chunks into outPixels with zero heap allocation
    u8 zlibHdrBytes[2]{};
    u8 zlibHdrRead = 0U;
    u8 blockHdrBytes[5]{};
    u8 blockHdrRead = 0U;

    auto feedIdatByte = [&](u8 byteVal) noexcept -> bool {
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

        if (currentRow >= expectedHeight) {
            return true; // Ignore trailing Adler32 checksum bytes after all rows are decoded
        }

        if (blockRemaining == 0U) {
            if (finalBlockSeen) {
                return true;
            }
            blockHdrBytes[blockHdrRead++] = byteVal;
            if (blockHdrRead == 5U) {
                const u8 bheader = blockHdrBytes[0];
                const u8 btype = static_cast<u8>((bheader >> 1U) & 0x03U);
                if (btype != 0U) {
                    return false; // Only stored/uncompressed DEFLATE blocks are supported without external zlib
                }
                finalBlockSeen = ((bheader & 0x01U) != 0U);
                const u16 len = ReadLeU16(&blockHdrBytes[1]);
                const u16 nlen = ReadLeU16(&blockHdrBytes[3]);
                if (static_cast<u16>(len ^ 0xFFFFU) != nlen) {
                    return false;
                }
                blockRemaining = len;
                blockHdrRead = 0U;
            }
            return true;
        }

        curScanline[scanlineWritePos++] = byteVal;
        --blockRemaining;

        if (scanlineWritePos == scanlineBytes) {
            u8* dstRow = outPixels.data() + static_cast<usize>(currentRow) * strideBytes;
            const u8* prevRow = (currentRow > 0U) ? (dstRow - strideBytes) : nullptr;
            if (!UnfilterScanline(curScanline[0], &curScanline[1], prevRow, dstRow, strideBytes)) {
                return false;
            }
            scanlineWritePos = 0U;
            ++currentRow;
        }
        return true;
    };

    std::array<u8, 4096> chunkBuf{};
    while (true) {
        std::array<u8, 8> chunkHdr{};
        if (std::fread(chunkHdr.data(), 1U, chunkHdr.size(), fp) != chunkHdr.size()) {
            break;
        }
        const u32 chunkLen = ReadBeU32(chunkHdr.data());
        const u32 chunkType = ReadBeU32(chunkHdr.data() + 4U);

        if (chunkType == 0x49484452U) { // 'IHDR'
            std::array<u8, 13> ihdr{};
            if (chunkLen != 13U || std::fread(ihdr.data(), 1U, ihdr.size(), fp) != ihdr.size()) {
                std::fclose(fp);
                return false;
            }
            const u32 w = ReadBeU32(&ihdr[0]);
            const u32 h = ReadBeU32(&ihdr[4]);
            const u8 bitDepth = ihdr[8];
            const u8 colorType = ihdr[9];
            const u8 interlace = ihdr[12];
            if (w != expectedWidth || h != expectedHeight || bitDepth != 8U || colorType != 6U || interlace != 0U) {
                std::fclose(fp);
                return false;
            }
            ihdrValid = true;
            std::fseek(fp, 4L, SEEK_CUR); // Skip CRC
        } else if (chunkType == 0x49444154U) { // 'IDAT'
            if (!ihdrValid) {
                std::fclose(fp);
                return false;
            }
            u32 remaining = chunkLen;
            while (remaining > 0U) {
                const u32 toRead = (remaining < chunkBuf.size()) ? remaining : static_cast<u32>(chunkBuf.size());
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
            std::fseek(fp, 4L, SEEK_CUR); // Skip CRC
        } else if (chunkType == 0x49454E44U) { // 'IEND'
            break;
        } else {
            std::fseek(fp, static_cast<long>(chunkLen + 4U), SEEK_CUR);
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
    if (filePath == nullptr || outPixels.size() < expectedByteSize) {
        return false;
    }

    std::FILE* fp = std::fopen(filePath, "rb");
    if (fp == nullptr) {
        return false;
    }

    std::array<u8, 54> hdr{};
    if (std::fread(hdr.data(), 1U, hdr.size(), fp) != hdr.size()) {
        std::fclose(fp);
        return false;
    }
    if (hdr[0] != 'B' || hdr[1] != 'M') {
        std::fclose(fp);
        return false;
    }

    const u32 pixelOffset = ReadLeU32(&hdr[10]);
    const i32 width = ReadLeI32(&hdr[18]);
    const i32 rawHeight = ReadLeI32(&hdr[22]);
    const bool topDown = (rawHeight < 0);
    const i32 height = topDown ? -rawHeight : rawHeight;
    const u16 bpp = ReadLeU16(&hdr[28]);
    const u32 compression = ReadLeU32(&hdr[30]);

    if (width != static_cast<i32>(expectedWidth) ||
        height != static_cast<i32>(expectedHeight) ||
        bpp != 32U ||
        (compression != 0U && compression != 3U)) {
        std::fclose(fp);
        return false;
    }

    if (std::fseek(fp, static_cast<long>(pixelOffset), SEEK_SET) != 0) {
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
        // Swizzle BGRA -> RGBA in place
        for (u16 x = 0U; x < expectedWidth; ++x) {
            const u8 b = rowPtr[x * 4U + 0U];
            const u8 red = rowPtr[x * 4U + 2U];
            rowPtr[x * 4U + 0U] = red;
            rowPtr[x * 4U + 2U] = b;
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
    char pathBuf[256]{};

    for (const char* prefix : kPrefixes) {
        if (!primaryPngPath.empty()) {
            std::snprintf(pathBuf, sizeof(pathBuf), "%s%.*s",
                          prefix, static_cast<int>(primaryPngPath.size()), primaryPngPath.data());
            if (LoadPngRgba(pathBuf, expectedWidth, expectedHeight, outPixels)) {
                return true;
            }
        }
        if (!fallbackBmpPath.empty()) {
            std::snprintf(pathBuf, sizeof(pathBuf), "%s%.*s",
                          prefix, static_cast<int>(fallbackBmpPath.size()), fallbackBmpPath.data());
            if (LoadBmpRgba(pathBuf, expectedWidth, expectedHeight, outPixels)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace vots
