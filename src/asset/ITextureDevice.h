#pragma once

#include "core/Types.h"
#include "core/Result.h"
#include "core/Error.h"
#include "renderer/RendererTypes.h"
#include "asset/TextureAsset.h"

namespace EngineN {

/**
 * Hardware texture device seam.
 * Abstracts texture allocation and pixel uploads from the asset subsystem,
 * enabling headless testing without an active SDL_Renderer or GPU context.
 */
class ITextureDevice {
public:
    virtual ~ITextureDevice() = default;

    [[nodiscard]] virtual Result<TextureHandle, EngineError> Create(const TextureDesc& desc) noexcept = 0;
    [[nodiscard]] virtual Result<void, EngineError> Upload(TextureHandle handle,
                                                           const URect& rect,
                                                           const void* pixels,
                                                           u32 pitch) noexcept = 0;
    virtual void Destroy(TextureHandle handle) noexcept = 0;
};

} // namespace EngineN
