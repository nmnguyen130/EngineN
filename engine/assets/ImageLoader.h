#pragma once

#include "core/Types.h"

namespace en {

// Runtime RGBA8 image loader for uncompressed/stored-DEFLATE PNG and 32-bit RGBA/BGRA BMP files.
// Reads directly into caller-provided memory slices with zero heap allocation.
// Invariant: outPixels must have size >= expectedWidth * expectedHeight * 4 bytes.
class ImageLoader {
public:
    [[nodiscard]] static bool LoadRgbaImageFromDisk(
        std::string_view primaryPngPath,
        std::string_view fallbackBmpPath,
        u16 expectedWidth,
        u16 expectedHeight,
        std::span<u8> outPixels) noexcept;

    [[nodiscard]] static bool LoadPngRgba(
        const char* filePath,
        u16 expectedWidth,
        u16 expectedHeight,
        std::span<u8> outPixels) noexcept;

    [[nodiscard]] static bool LoadBmpRgba(
        const char* filePath,
        u16 expectedWidth,
        u16 expectedHeight,
        std::span<u8> outPixels) noexcept;
};

} // namespace en
