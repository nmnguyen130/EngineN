#pragma once

#include "renderer/RendererTypes.h"
#include "renderer/Texture.h"

namespace EngineN {

class Window;
struct RenderDeviceConfig;

/// A contiguous run of primitives sharing the same GPU state (single draw call).
struct DrawRun {
    u16       textureSlot = 0;
    BlendMode blend = BlendMode::Premultiplied;
    u8        material = 0;
    u16       scissorId = 0;     ///< 0 = Scissor disabled; >0 = Index in scissor table
    u32       firstIndex = 0;    ///< Offset into sorted indices buffer
    u32       indexCount = 0;    ///< Number of indices in this run
    u32       vertexOffset = 0;  ///< Base vertex offset
    u32       vertexCount = 0;   ///< Number of referenced vertices
};

/// Immutable read-only view of finalized frame geometry handed to IRenderBackend.
struct BatchView {
    std::span<const Vertex>  vertices;
    std::span<const u32>      indices;
    std::span<const DrawRun>  runs;
    std::span<const Rect>     scissors;
    FrameStats                stats;
};

struct BackendCaps {
    const char* backendName = "Unknown";
    u32         maxTextureDimension = 4096;
    bool        vsyncAdaptive = false;
    bool        supportsCustomShaders = false;
};

/// Hardware Backend Abstraction Interface.
/// Milestone 03 implements SdlRendererBackend and NullBackend (for headless tests/CI).
/// Future milestones can introduce SdlGpuBackend without changing game code.
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    [[nodiscard]] virtual Result<void> Init(Window* window, const RenderDeviceConfig& cfg) = 0;
    virtual void Shutdown() = 0;

    [[nodiscard]] virtual Result<void> CreateTexture(
        TextureHandle handle,
        const TextureConfig& cfg,
        const void* initialPixels) = 0;

    [[nodiscard]] virtual Result<void> UpdateTexture(
        TextureHandle handle,
        const Rect& region,
        const void* pixels) = 0;

    virtual void DestroyTexture(TextureHandle handle) = 0;

    [[nodiscard]] virtual Result<void> SetVSync(VSyncMode mode) = 0;
    [[nodiscard]] virtual Result<void> SetLogicalPresentation(
        i32 w, i32 h, PresentationMode mode) = 0;

    [[nodiscard]] virtual Result<void> BeginFrame(Color clearColor) = 0;
    [[nodiscard]] virtual Result<void> Submit(const BatchView& batch) = 0;
    [[nodiscard]] virtual Result<void> Present() = 0;

    [[nodiscard]] virtual const BackendCaps& GetCaps() const noexcept = 0;
    [[nodiscard]] virtual Vec2 WindowToVirtual(Vec2 windowPx) const noexcept = 0;
    [[nodiscard]] virtual Vec2 VirtualToWindow(Vec2 virtualPx) const noexcept = 0;
    [[nodiscard]] virtual FRect GetPresentationRect() const noexcept = 0;
};

} // namespace EngineN
