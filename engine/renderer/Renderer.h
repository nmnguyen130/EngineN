#pragma once

#include "platform/PlatformTypes.h"
#include "renderer/RenderList.h"

#include <array>

struct SDL_Renderer;
struct SDL_Texture;

namespace en::platform { class Window; }

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

    [[nodiscard]] Result<void, RendererErrc> Init(
        platform::Window& window,
        const RendererConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_renderer != nullptr; }
    void ResizeViewport(platform::Extent2i pixelSize) noexcept;
    [[nodiscard]] Result<void, RendererErrc> SetPresentMode(PresentMode mode) noexcept;

    [[nodiscard]] Result<TextureHandle, RendererErrc> CreateTexture(const TextureDesc& desc) noexcept;
    [[nodiscard]] Result<void, RendererErrc> UpdateTexture(
        TextureHandle handle,
        FRect region,
        std::span<const u8> pixels,
        u32 pitch = 0) noexcept;
    void DestroyTexture(TextureHandle handle) noexcept;
    [[nodiscard]] constexpr bool IsTextureValid(TextureHandle handle) const noexcept;

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