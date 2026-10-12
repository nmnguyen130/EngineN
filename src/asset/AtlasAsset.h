#pragma once

#include "core/Types.h"
#include "renderer/RendererTypes.h"
#include "asset/AssetTypes.h"
#include <span>

namespace EngineN {

/**
 * FrameHot: 16-byte packed layout for a single sprite frame.
 * Kept strictly in Array-of-Structures (AoS) format because all 5 fields
 * are always read together during visual state extraction.
 */
struct alignas(8) FrameHot {
    u16 uvRect[4]{0, 0, 65535, 65535}; // unorm16: [u0, v0, u1, v1]
    u16 width{0};
    u16 height{0};
    i16 pivotX{0};
    i16 pivotY{0};

    constexpr FrameHot() noexcept = default;
    constexpr FrameHot(u16 u0, u16 v0, u16 u1, u16 v1, u16 w, u16 h, i16 px = 0, i16 py = 0) noexcept
        : uvRect{u0, v0, u1, v1}, width(w), height(h), pivotX(px), pivotY(py) {}
};

static_assert(sizeof(FrameHot) == 16, "FrameHot must be exactly 16 bytes for cache line packing");

/**
 * Atlas asset pairing a source texture with a contiguous array of sprite frames.
 */
struct Atlas {
    /**
     * Hot presentation descriptor queried during extraction.
     */
    struct Hot {
        const FrameHot*      frames{nullptr};
        AssetHandle<Texture> texture{};
        u16                  frameCount{0};
        u16                  reserved{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return frames != nullptr && frameCount > 0;
        }
    };

    /**
     * Cold metadata for asset identification and diagnostics.
     */
    struct Cold {
        AssetId id{};
        u32     firstFrameIndex{0};
        char    debugPath[64]{};
    };
};

/**
 * Specification for uniform grid sprite sheet slicing.
 */
struct AtlasGridDesc {
    AssetHandle<Texture> texture{};
    u16                  frameWidth{0};
    u16                  frameHeight{0};
    u16                  frameCount{0};
    i16                  pivotX{0};
    i16                  pivotY{0};
};

/**
 * Specification for manual or non-uniform atlas frame construction.
 */
struct AtlasDesc {
    AssetHandle<Texture>      texture{};
    std::span<const FrameHot> frames{};
};

} // namespace EngineN
