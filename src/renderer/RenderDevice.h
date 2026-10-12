#pragma once

#include "core/Result.h"
#include "core/Error.h"
#include "platform/Window.h"
#include "renderer/RenderBackend.h"
#include "renderer/FramePacket.h"
#include "asset/ITextureDevice.h"

#include <SDL3/SDL.h>
#include <vector>

namespace EngineN {

struct RenderDeviceConfig {
    bool vsync{true};
};

/**
 * RenderDevice: Primary hardware presentation and rasterization coordinator using SDL_Renderer.
 * Translates FramePacket batches into GPU draw calls with 0 heap allocation per frame.
 * Implements ITextureDevice to bridge the asset subsystem with hardware texture memory.
 */
class RenderDevice final : public RenderBackend, public ITextureDevice {
public:
    RenderDevice() noexcept = default;
    ~RenderDevice() override;

    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;
    RenderDevice(RenderDevice&& other) noexcept;
    RenderDevice& operator=(RenderDevice&& other) noexcept;

    [[nodiscard]] Result<void, EngineError> Init(Window& window, const RenderDeviceConfig& config = {}) noexcept;
    void Shutdown() noexcept;

    // RenderBackend interface conformance: Execute draws; Present updates swapchain
    [[nodiscard]] Result<void, EngineError> Execute(const FramePacket& packet) noexcept override;
    [[nodiscard]] Result<PresentResult, EngineError> Present(const PresentationPolicy& policy) noexcept override;

    // Convenience one-shot call (Execute + Present)
    [[nodiscard]] Result<void, EngineError> Submit(const FramePacket& packet) noexcept;

    // ITextureDevice interface conformance
    [[nodiscard]] Result<TextureHandle, EngineError> Create(const TextureDesc& desc) noexcept override;
    [[nodiscard]] Result<void, EngineError> Upload(TextureHandle handle,
                                                   const URect& rect,
                                                   const void* pixels,
                                                   u32 pitch) noexcept override;
    void Destroy(TextureHandle handle) noexcept override;

    [[nodiscard]] Result<void, EngineError> SetVSync(bool enabled) noexcept;

    [[nodiscard]] bool IsValid() const noexcept { return m_renderer != nullptr; }
    [[nodiscard]] SDL_Renderer* Native() const noexcept { return m_renderer; }

private:
    static constexpr usize kMaxTextureSlots = 256;

    SDL_Renderer*           m_renderer{nullptr};
    std::vector<SDL_Vertex> m_vertexScratch;
    std::vector<int>        m_indexScratch;
    SDL_Texture*            m_textureSlots[kMaxTextureSlots]{};
    u16                     m_textureGens[kMaxTextureSlots]{};
    std::vector<u16>        m_freeTextureSlots;
};

} // namespace EngineN
