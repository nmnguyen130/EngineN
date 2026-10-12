#pragma once

#include "core/Types.h"
#include "renderer/RendererTypes.h"
#include <string_view>

namespace EngineN {

/**
 * Stable 64-bit identity for virtual assets, computed via compile-time or runtime FNV-1a.
 * Maps virtual asset paths (e.g. "textures/player.png") to unchanging numerical keys.
 */
struct AssetId {
    u64 value{0};

    [[nodiscard]] constexpr bool Valid() const noexcept { return value != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return Valid(); }
    friend constexpr auto operator<=>(AssetId, AssetId) = default;
};

namespace Detail {
    inline constexpr u64 kFnv1aOffsetBasis = 14695981039346656037ULL;
    inline constexpr u64 kFnv1aPrime       = 1099511628211ULL;

    constexpr u64 HashPath(std::string_view path) noexcept {
        u64 hash = kFnv1aOffsetBasis;
        for (char c : path) {
            // Normalize path separators to forward slash for stable cross-platform keys
            const u8 byte = static_cast<u8>(c == '\\' ? '/' : c);
            hash ^= static_cast<u64>(byte);
            hash *= kFnv1aPrime;
        }
        return hash;
    }
} // namespace Detail

[[nodiscard]] constexpr AssetId MakeAssetId(std::string_view path) noexcept {
    return AssetId{Detail::HashPath(path)};
}

/**
 * Constexpr literal for compile-time asset IDs: "textures/player.png"_aid
 */
[[nodiscard]] consteval AssetId operator""_aid(const char* str, std::size_t len) {
    return AssetId{Detail::HashPath(std::string_view{str, len})};
}

enum class AssetKind : u8 {
    Unknown = 0,
    Texture,
    Atlas,
    Audio,
    Font,
    Count
};

enum class AssetState : u8 {
    Unloaded = 0,
    Loading,
    Ready,
    Failed
};

/**
 * AssetHandle<T>: Lightweight 32-bit generational handle.
 * Layout: 20 bits index (1M capacity), 12 bits generation (4096 generations).
 * Slot 0 is reserved for the permanent fallback asset.
 * Null handles have raw value 0 (index 0, gen 0).
 */
template <typename T>
struct AssetHandle {
    static constexpr u32 kIndexBits = 20;
    static constexpr u32 kGenBits   = 12;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;
    static constexpr u32 kGenMask   = (1u << kGenBits) - 1u;

    u32 raw{0};

    constexpr AssetHandle() noexcept = default;
    constexpr explicit AssetHandle(u32 rawHandle) noexcept : raw(rawHandle) {}

    [[nodiscard]] constexpr u32 Index() const noexcept {
        return raw & kIndexMask;
    }

    [[nodiscard]] constexpr u16 Generation() const noexcept {
        return static_cast<u16>((raw >> kIndexBits) & kGenMask);
    }

    [[nodiscard]] constexpr bool IsNull() const noexcept {
        return raw == 0;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return raw != 0;
    }

    [[nodiscard]] static constexpr AssetHandle<T> Null() noexcept {
        return AssetHandle<T>{0};
    }

    [[nodiscard]] static constexpr AssetHandle<T> FromParts(u32 index, u16 gen) noexcept {
        return AssetHandle<T>{(index & kIndexMask) | ((static_cast<u32>(gen) & kGenMask) << kIndexBits)};
    }

    constexpr auto operator<=>(const AssetHandle&) const = default;
};

// Forward declaration for atlas handle types
struct Atlas;

/**
 * SpriteRef: Lightweight identifier for a single frame inside an atlas.
 */
struct SpriteRef {
    AssetHandle<Atlas> atlas{};
    u16                frameIndex{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return atlas.IsValid(); }
    constexpr auto operator<=>(const SpriteRef&) const = default;
};

/**
 * SpriteView: Resolved visual data ready for sprite instance generation.
 */
struct SpriteView {
    TextureHandle texture{};
    u16           uvRect[4]{0, 0, 65535, 65535};
    u16           size[2]{0, 0};
    i16           pivot[2]{0, 0};
};

} // namespace EngineN
