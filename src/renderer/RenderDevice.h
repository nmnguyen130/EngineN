#pragma once

#include "renderer/RendererTypes.h"
#include "renderer/Texture.h"
#include "renderer/RenderBackend.h"

#include <memory>
#include <vector>

namespace EngineN {

class Window;

/// Configuration parameters for initializing RenderDevice.
struct RenderDeviceConfig {
    VSyncMode        vsync = VSyncMode::On;
    PresentationMode presentation = PresentationMode::Letterbox;
    i32              virtualWidth = 1280;
    i32              virtualHeight = 720;
    Color            clearColor = Colors::Black;
    u32              maxTextures = 1024;
};

/// Manages GPU backend lifecycle, generational texture handles, and frame presentation.
class RenderDevice {
public:
    RenderDevice();
    ~RenderDevice();

    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;
    RenderDevice(RenderDevice&&) noexcept;
    RenderDevice& operator=(RenderDevice&&) noexcept;

    /// Initialize with hardware window (creates SdlRendererBackend).
    [[nodiscard]] Result<void> Create(Window& window, const RenderDeviceConfig& cfg = {});

    /// Initialize headless mock backend for testing and CI without OS window.
    [[nodiscard]] Result<void> CreateHeadless(const RenderDeviceConfig& cfg = {});

    /// Shutdown device and release all textures.
    void Destroy();

    [[nodiscard]] bool IsValid() const noexcept { return m_backend != nullptr; }

    // Frame Lifecycle
    [[nodiscard]] Result<void> BeginFrame();
    [[nodiscard]] Result<void> Submit(const BatchView& batch);
    [[nodiscard]] Result<void> Present();

    // Texture Lifecycle (Generational Handles)
    [[nodiscard]] Result<TextureHandle> CreateTexture(
        const TextureConfig& cfg,
        const void* initialPixels = nullptr);

    [[nodiscard]] Result<void> UpdateTexture(
        TextureHandle handle,
        const Rect& region,
        const void* pixels);

    void DestroyTexture(TextureHandle handle);

    [[nodiscard]] bool IsTextureValid(TextureHandle handle) const noexcept;
    [[nodiscard]] const TextureConfig* GetTextureConfig(TextureHandle handle) const noexcept;

    // Display & Coordinate Queries
    [[nodiscard]] Result<void> SetVSync(VSyncMode mode);
    [[nodiscard]] Result<void> SetPresentation(i32 w, i32 h, PresentationMode mode);
    void SetClearColor(Color color) noexcept { m_cfg.clearColor = color; }

    [[nodiscard]] Vec2 WindowToVirtual(Vec2 windowPx) const noexcept;
    [[nodiscard]] Vec2 VirtualToWindow(Vec2 virtualPx) const noexcept;
    [[nodiscard]] FRect GetPresentationRect() const noexcept;

    [[nodiscard]] const BackendCaps& GetCaps() const noexcept;
    [[nodiscard]] const RenderDeviceConfig& GetConfig() const noexcept { return m_cfg; }

private:
    struct TextureSlot {
        TextureConfig config{};
        u16           generation = 1;
        bool          inUse = false;
    };

    Result<void> InitBuiltInWhiteTexture();

    std::unique_ptr<IRenderBackend> m_backend;
    RenderDeviceConfig              m_cfg{};
    std::vector<TextureSlot>        m_textureSlots;

    bool m_isInsideFrame = false;
};

} // namespace EngineN
