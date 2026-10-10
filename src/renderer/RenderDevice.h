#pragma once

#include "core/Result.h"
#include "core/Error.h"
#include "platform/Window.h"
#include "renderer/RenderBackend.h"
#include "renderer/FramePacket.h"

#include <SDL3/SDL.h>
#include <vector>

namespace EngineN {

struct RenderDeviceConfig {
    bool vsync{true};
};

/**
 * RenderDevice: Primary hardware presentation and rasterization coordinator.
 * Translates FramePacket batches into GPU draw calls with 0 heap allocation per frame.
 */
class RenderDevice final : public RenderBackend {
public:
    RenderDevice() noexcept = default;
    ~RenderDevice() override;

    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;
    RenderDevice(RenderDevice&& other) noexcept;
    RenderDevice& operator=(RenderDevice&& other) noexcept;

    [[nodiscard]] Result<void, EngineError> Init(Window& window, const RenderDeviceConfig& config = {}) noexcept;
    void Shutdown() noexcept;

    // Submits the finalized frame packet to hardware and presents to the window
    [[nodiscard]] Result<void, EngineError> Submit(const FramePacket& packet) noexcept;

    // RenderBackend interface conformance
    [[nodiscard]] Result<void, EngineError> Execute(const FramePacket& packet) noexcept override {
        return Submit(packet);
    }

    [[nodiscard]] Result<void, EngineError> SetVSync(bool enabled) noexcept;

    [[nodiscard]] bool IsValid() const noexcept { return m_renderer != nullptr; }
    [[nodiscard]] SDL_Renderer* Native() const noexcept { return m_renderer; }

private:
    SDL_Renderer*           m_renderer{nullptr};
    std::vector<SDL_Vertex> m_vertexScratch;
    std::vector<int>        m_indexScratch;
};

} // namespace EngineN
