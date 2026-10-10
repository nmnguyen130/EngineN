#pragma once

#include "core/Result.h"
#include "core/Error.h"
#include "renderer/FramePacket.h"

namespace EngineN {

/**
 * RenderBackend: Hardware presentation execution interface.
 * Operates strictly at the FramePacket boundary with no per-draw calls.
 */
class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    [[nodiscard]] virtual Result<void, EngineError> Execute(const FramePacket& packet) noexcept = 0;
};

} // namespace EngineN
