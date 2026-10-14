#pragma once

#include "platform/Window.h"
#include "renderer/RendererList.h"

#include <array>
#include <expected>

struct SDL_Renderer;
struct SDL_Texture;

namespace en::renderer {

struct RendererConfig {
    PresentMode presentMode{PresentMode::VSync};
};

// Hardware presentation adapter and generational GPU texture sink.
// Move-only RAII owner of SDL_Renderer and SDL_Texture pool; main-thread only.
// Invariant: Slot 0 permanently holds a valid 1x1 white texture while initialized.
class Renderer {
public:
    static constexpr u32 kMaxTextures = 4096;

    Renderer() = default;
    ~Renderer() noexcept;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&& other) noexcept;
    Renderer& operator=(Renderer&& other) noexcept;

    [[nodiscard]] std::expected<void, RendererErrc> Init(
        platform::Window& window,
        const RendererConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_renderer != nullptr; }
    void ResizeViewport(platform::Extent2i pixelSize) noexcept;
    [[nodiscard]] std::expected<void, RendererErrc> SetPresentMode(PresentMode mode) noexcept;

    // Asset Sink API (Consumed by Asset Manager for texture lifecycle management)
    [[nodiscard]] std::expected<TextureHandle, RendererErrc> CreateTexture(const TextureDesc& desc) noexcept;
    [[nodiscard]] std::expected<void, RendererErrc> UpdateTexture(
        TextureHandle handle,
        FRect region,
        std::span<const u8> pixels,
        u32 pitch = 0) noexcept;
    void DestroyTexture(TextureHandle handle) noexcept;
    [[nodiscard]] constexpr bool IsTextureValid(TextureHandle handle) const noexcept;

    // Per-frame recording and execution entry points
    [[nodiscard]] RenderList& BeginFrame() noexcept;
    void Submit() noexcept;
    void Submit(RenderList& list) noexcept;

    [[nodiscard]] constexpr const RenderStats& LastFrameStats() const noexcept { return m_list.Stats(); }

private:
    struct TextureSlot {
        SDL_Texture* texture{nullptr};
        u16 width{0};
        u16 height{0};
        u16 generation{0};
        u16 nextFree{0};
        BlendMode cachedBlend{BlendMode::AlphaPMA};
    };

    void ApplyCameraView(const Camera2D& view) noexcept;
    [[nodiscard]] SDL_Texture* ResolveAndBindTexture(TextureHandle handle, BlendMode blend) noexcept;

    SDL_Renderer* m_renderer{nullptr};
    platform::Extent2i m_viewportSize{};

    std::array<TextureSlot, kMaxTextures> m_textures{};
    u16 m_freeHead{1};

    RenderList m_list{};
    alignas(64) std::array<Vertex2D, RenderList::kChunkQuads * 4> m_vertexScratch{};
};

constexpr bool Renderer::IsTextureValid(TextureHandle handle) const noexcept {
    return handle.index < kMaxTextures &&
           handle.generation != 0 &&
           m_textures[handle.index].generation == handle.generation &&
           m_textures[handle.index].texture != nullptr;
}

} // namespace en::renderer