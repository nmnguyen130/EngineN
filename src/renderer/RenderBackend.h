#pragma once

#include "core/Result.h"
#include "core/Error.h"
#include "renderer/FramePacket.h"

namespace EngineN {

enum class PacingMode : u8 {
    VSync = 0,
    Capped,
    Uncapped
};

struct PresentationPolicy {
    PacingMode mode = PacingMode::VSync;
    u32 capFps = 0;
};

struct PresentResult {
    bool occluded = false;
    u64  cpuNs = 0;
};

/**
 * RenderBackend: Hardware presentation execution interface.
 * Decouples GPU submission Execute() from swapchain Present().
 */
class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    virtual void WaitForNextFrame() noexcept {}
    [[nodiscard]] virtual Result<void, EngineError> Resize(u32 pixelW, u32 pixelH) noexcept {
        (void)pixelW; (void)pixelH;
        return {};
    }
    [[nodiscard]] virtual Result<void, EngineError> Execute(const FramePacket& packet) noexcept = 0;
    [[nodiscard]] virtual Result<PresentResult, EngineError> Present(const PresentationPolicy& policy) noexcept = 0;
};

} // namespace EngineN
