#pragma once

#include "renderer/RenderBackend.h"
#include "renderer/RenderDevice.h"
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_FColor;

namespace EngineN {

class Window;

/// Hardware-accelerated renderer backend powered by SDL3 SDL_Renderer
/// and SDL_RenderGeometryRaw.
class SdlRendererBackend final : public IRenderBackend {
public:
    SdlRendererBackend();
    ~SdlRendererBackend() override;

    [[nodiscard]] Result<void> Init(Window* window, const RenderDeviceConfig& cfg) override;
    void Shutdown() override;

    [[nodiscard]] Result<void> CreateTexture(
        TextureHandle handle,
        const TextureConfig& cfg,
        const void* initialPixels) override;

    [[nodiscard]] Result<void> UpdateTexture(
        TextureHandle handle,
        const Rect& region,
        const void* pixels) override;

    void DestroyTexture(TextureHandle handle) override;

    [[nodiscard]] Result<void> SetVSync(VSyncMode mode) override;
    [[nodiscard]] Result<void> SetLogicalPresentation(
        i32 w, i32 h, PresentationMode mode) override;

    [[nodiscard]] Result<void> BeginFrame(Color clearColor) override;
    [[nodiscard]] Result<void> Submit(const BatchView& batch) override;
    [[nodiscard]] Result<void> Present() override;

    [[nodiscard]] const BackendCaps& GetCaps() const noexcept override;
    [[nodiscard]] Vec2 WindowToVirtual(Vec2 windowPx) const noexcept override;
    [[nodiscard]] Vec2 VirtualToWindow(Vec2 virtualPx) const noexcept override;
    [[nodiscard]] FRect GetPresentationRect() const noexcept override;

private:
    SDL_Renderer*             m_renderer = nullptr;
    RenderDeviceConfig        m_cfg{};
    BackendCaps               m_caps{};
    std::vector<SDL_Texture*> m_textures;
    std::vector<SDL_FColor>   m_colorScratch;
};

} // namespace EngineN
