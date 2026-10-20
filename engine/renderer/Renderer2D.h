#pragma once

#include "assets/AssetBank.h"
#include "world/World.h"
#include "gameplay/ParticleAndCamera.h"
#include "renderer/LightingAndPostFx.h"
#include "renderer/UiBuilder.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace en {

struct RendererConfig {
    bool vsync{true};
    bool headless{false};
    f32 pixelsPerMeter{48.0f};
};

// Hardware 2D batch presentation adapter, generational GPU texture pool, and WorldExtractor.
// Move-only RAII owner of SDL_Renderer and RenderList.
// Invariant: Slot 0 is always a valid 1x1 white fallback texture.
class alignas(64) Renderer2D {
public:
    static constexpr usize kMaxGpuTextures = 1024;
    static constexpr usize kMaxRenderTargets = 8;
    static constexpr u16 kSwFbWidth = 640;
    static constexpr u16 kSwFbHeight = 360;

    struct alignas(16) GpuTexSlot {
        SDL_Texture* texture{nullptr};
        const u8* cpuPixels{nullptr};
        u16 width{0};
        u16 height{0};
        u16 generation{0};
        u16 flags{0};
    };

    struct RenderTargetSlot {
        GpuTextureHandle colorTex{kWhiteGpuTexture};
        u16 width{0};
        u16 height{0};
        u16 generation{0};
        bool active{false};
    };

    Renderer2D() noexcept = default;
    ~Renderer2D() noexcept { Shutdown(); }

    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    [[nodiscard]] Result<void, PlatformErrc> Init(Window& window, const RendererConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] GpuTextureHandle CreateTexture(u16 width, u16 height, std::span<const u8> rgbaPixels) noexcept;
    void DestroyTexture(GpuTextureHandle handle) noexcept;
    [[nodiscard]] constexpr bool IsTextureValid(GpuTextureHandle handle) const noexcept {
        return handle.index < m_highWaterTex && m_textures[handle.index].generation == handle.generation;
    }

    [[nodiscard]] RenderTargetHandle CreateRenderTarget(u16 width, u16 height) noexcept;
    [[nodiscard]] GpuTextureHandle GetRenderTargetTexture(RenderTargetHandle fbo) const noexcept;

    [[nodiscard]] RenderList& BeginFrame(ViewportExtent viewportSize = {1280, 720}) noexcept;
    void Submit() noexcept { Submit(m_renderList); }
    void Submit(RenderList& list) noexcept;

    [[nodiscard]] std::span<const u32> FramebufferPixels() const noexcept { return m_swFramebuffer; }
    [[nodiscard]] u32 SampleFramebufferPixel(u16 x, u16 y) const noexcept {
        if (x >= kSwFbWidth || y >= kSwFbHeight) return 0;
        return m_swFramebuffer[static_cast<usize>(y) * kSwFbWidth + x];
    }
    [[nodiscard]] u32 CountNonBlackPixels() const noexcept;
    bool SaveFramebufferBmp(const char* filePath) const noexcept;

    static void ExtractWorld(
        const World& world,
        const AssetBank& assetBank,
        const ParticlePool2D& particles,
        const RenderTimeView& timeView,
        RenderList& outList,
        f32 pixelsPerMeter = 48.0f) noexcept;

    [[nodiscard]] constexpr RenderList& List() noexcept { return m_renderList; }
    [[nodiscard]] constexpr const RenderStats& LastFrameStats() const noexcept { return m_renderList.Stats(); }

private:
    void RasterizeSoftwareFramebuffer(const RenderList& list) noexcept;

    SDL_Renderer* m_renderer{nullptr};
    f32 m_pixelsPerMeter{48.0f};
    u16 m_highWaterTex{1};
    u16 m_highWaterFbo{1};
    bool m_headless{false};
    bool m_initialized{false};

    std::array<GpuTexSlot, kMaxGpuTextures> m_textures{};
    std::array<RenderTargetSlot, kMaxRenderTargets> m_renderTargets{};
    std::array<u32, kSwFbWidth * kSwFbHeight> m_swFramebuffer{};
    AlignedArena m_texArena{};
    RenderList m_renderList{};
};

} // namespace en
