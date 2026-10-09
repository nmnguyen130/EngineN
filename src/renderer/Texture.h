#pragma once

#include "renderer/RendererTypes.h"

namespace EngineN {

/// Generational texture handle preventing dangling pointer dereferences.
/// Slot 0 is reserved as invalid/null.
/// Slot 1 is reserved as the built-in 1x1 white texture.
struct TextureHandle {
    u16 index = 0;
    u16 generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return index > 0 && generation > 0;
    }

    static constexpr TextureHandle Null() noexcept { return {0, 0}; }
    static constexpr TextureHandle White() noexcept { return {1, 1}; }

    constexpr bool operator==(const TextureHandle&) const noexcept = default;
};

struct TextureConfig {
    u32 width = 0;
    u32 height = 0;
    TextureFilter filter = TextureFilter::Nearest;
    bool premultiplyAlpha = true;
};

/// Represents a rectangular sub-region of a GPU texture (for spritesheets and atlases).
struct TextureRegion {
    TextureHandle handle = TextureHandle::Null();
    FRect         uvRect{0.0f, 0.0f, 1.0f, 1.0f};
    u32           pixelWidth = 0;
    u32           pixelHeight = 0;

    static TextureRegion FromPixels(
        TextureHandle h,
        Rect pixelRegion,
        u32 textureWidth,
        u32 textureHeight) noexcept
    {
        if (textureWidth == 0 || textureHeight == 0 || pixelRegion.IsEmpty()) {
            return {};
        }

        const f32 invW = 1.0f / static_cast<f32>(textureWidth);
        const f32 invH = 1.0f / static_cast<f32>(textureHeight);

        return {
            .handle = h,
            .uvRect = {
                static_cast<f32>(pixelRegion.x) * invW,
                static_cast<f32>(pixelRegion.y) * invH,
                static_cast<f32>(pixelRegion.w) * invW,
                static_cast<f32>(pixelRegion.h) * invH
            },
            .pixelWidth = static_cast<u32>(pixelRegion.w),
            .pixelHeight = static_cast<u32>(pixelRegion.h)
        };
    }

    [[nodiscard]] TextureRegion Flipped(SpriteFlip flip) const noexcept {
        TextureRegion res = *this;
        const u8 flipMask = static_cast<u8>(flip);

        if (flipMask & static_cast<u8>(SpriteFlip::FlipH)) {
            res.uvRect.x += res.uvRect.w;
            res.uvRect.w = -res.uvRect.w;
        }
        if (flipMask & static_cast<u8>(SpriteFlip::FlipV)) {
            res.uvRect.y += res.uvRect.h;
            res.uvRect.h = -res.uvRect.h;
        }

        return res;
    }
};

} // namespace EngineN
