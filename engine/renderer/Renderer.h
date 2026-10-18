#pragma once

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
// Move-only RAII owner of SDL_Renderer and 16-byte SDL_Texture pool; main-thread only.
// Invariant: Slot 0 permanently holds a valid 1x1 white texture while initialized.
class alignas(64) Renderer {
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
    constexpr void ResizeViewport(ViewportExtent pixelSize) noexcept { m_viewportSize = pixelSize; }
    [[nodiscard]] Result<void, RendererErrc> SetPresentMode(PresentMode mode) noexcept;

    [[nodiscard]] Result<TextureHandle, RendererErrc> CreateTexture(const TextureDesc& desc) noexcept;
    [[nodiscard]] Result<void, RendererErrc> UpdateTexture(
        TextureHandle handle,
        FRect region,
        std::span<const u8> pixels,
        u32 pitch = 0) noexcept;
    void DestroyTexture(TextureHandle handle) noexcept;
    [[nodiscard]] constexpr bool IsTextureValid(TextureHandle handle) const noexcept;

    [[nodiscard]] RenderList& BeginFrame(ViewportExtent pixelSize = {}) noexcept;
    void Submit() noexcept { Submit(m_list); }
    void Submit(RenderList& list) noexcept;

    [[nodiscard]] constexpr const RenderStats& LastFrameStats() const noexcept { return m_list.Stats(); }

private:
    // 16-byte GPU texture slot (4 slots per 64-byte L1 cache line, zero tail padding).
    // While free (texture == nullptr), width stores the nextFree index in the intrusive freelist.
    struct alignas(16) TextureSlot {
        SDL_Texture* texture{nullptr};
        u16 width{0};
        u16 height{0};
        u16 generation{0};
        BlendMode cachedBlend{BlendMode::AlphaPMA};
        u8 reserved{0};
    };
    static_assert(sizeof(TextureSlot) == 16 && alignof(TextureSlot) == 16);

    [[nodiscard]] SDL_Texture* ResolveAndBindTexture(TextureHandle handle, BlendMode blend) noexcept;

    // Cache Line 0 (offset 0..63): Hot device pointer, viewport size, lazy pool watermarks, and Slot 0..1.
    SDL_Renderer* m_renderer{nullptr};
    ViewportExtent m_viewportSize{};
    u16 m_freeHead{0};
    u16 m_highWaterSlot{1};

    alignas(32) std::array<TextureSlot, kMaxTextures> m_textures{};

    RenderList m_list{};
    alignas(64) std::array<Vertex2D, RenderList::kChunkQuads * 4> m_vertexScratch{};
};

constexpr bool Renderer::IsTextureValid(TextureHandle handle) const noexcept {
    return handle.index < m_highWaterSlot &&
           handle.generation != 0 &&
           m_textures[handle.index].generation == handle.generation &&
           m_textures[handle.index].texture != nullptr;
}

} // namespace en::renderer
