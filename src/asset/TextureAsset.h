#pragma once

#include "core/Types.h"
#include "renderer/RendererTypes.h"
#include "asset/AssetTypes.h"

namespace EngineN {

/**
 * Geometric integer rectangle for pixel coordinates and texture uploads.
 */
struct URect {
    u32 x{0};
    u32 y{0};
    u32 w{0};
    u32 h{0};

    constexpr URect() noexcept = default;
    constexpr URect(u32 inX, u32 inY, u32 inW, u32 inH) noexcept
        : x(inX), y(inY), w(inW), h(inH) {}
};

enum class TextureFormat : u8 {
    RGBA8 = 0,
    BGRA8,
    Count
};

/**
 * Creation specifications for hardware texture allocation.
 */
struct TextureDesc {
    u32           width{0};
    u32           height{0};
    TextureFormat format{TextureFormat::RGBA8};
    bool          isStatic{true};
};

/**
 * Texture asset definition separating hot rendering descriptors from cold metadata.
 */
struct Texture {
    /**
     * Hot cache line representation queried directly by the renderer.
     * Keep size small (16 bytes) and trivially copyable.
     */
    struct Hot {
        TextureHandle gpuHandle{TextureHandle::Invalid()};
        u16           width{0};
        u16           height{0};
        u32           flags{0};
        u32           reserved{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return gpuHandle.IsValid();
        }
    };

    /**
     * Cold metadata accessed only during inspection, loading, or diagnostics.
     */
    struct Cold {
        AssetId id{};
        u32     cpuBytes{0};
        u32     gpuBytes{0};
        char    debugPath[64]{};
    };
};

} // namespace EngineN
