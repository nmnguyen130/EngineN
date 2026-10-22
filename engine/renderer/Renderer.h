#pragma once

#include "renderer/RenderList.h"
#include <utility>

struct SDL_Renderer;
struct SDL_Texture;

namespace en {

class Window;

struct alignas(4) RendererConfig {
    PresentMode presentMode{PresentMode::VSync};
    bool headless{false};
    u16 reserved{0};
};
static_assert(sizeof(RendererConfig) == 4);

// Move-only 64B-aligned SDL3 hardware presentation adapter and generational GPU texture/FBO pool.
// Isolates <SDL3/SDL.h> and platform/Window.h strictly to Renderer.cpp; zero heap allocations after Init().
// Invariant: Slot 0 is always a valid 1x1 white fallback texture (kWhiteTexture = {0, 1}) and cannot be destroyed.
class alignas(64) Renderer {
public:
    static constexpr u32 kMaxTextures = 4096;
    static constexpr u16 kInvalidSlot = 0xFFFFU;
    static constexpr u8 kSlotOccupied     = 1U << 0;
    static constexpr u8 kSlotRenderTarget = 1U << 1;

    // 16-byte (4 slots per 64B cache line, zero straddling, zero padding) generational texture/FBO slot.
    // While unoccupied, width stores the intrusive nextFree index in [1..kMaxTextures).
    struct alignas(16) TextureSlot {
        SDL_Texture* texture{nullptr};
        u16 width{kInvalidSlot};
        u16 height{0};
        u16 generation{1};
        TextureFilter filter{TextureFilter::Nearest};
        u8 flags{0};

        [[nodiscard]] constexpr bool IsOccupied() const noexcept { return (flags & kSlotOccupied) != 0U; }
        [[nodiscard]] constexpr bool IsRenderTarget() const noexcept { return (flags & kSlotRenderTarget) != 0U; }
    };
    static_assert(sizeof(TextureSlot) == 16);

    Renderer() noexcept = default;
    ~Renderer() noexcept { Shutdown(); }

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Renderer(Renderer&& other) noexcept;
    Renderer& operator=(Renderer&& other) noexcept;

    [[nodiscard]] Result<void, RendererErrc> Init(
        Window& window,
        const RendererConfig& cfg = {}) noexcept;

    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_initialized; }
    [[nodiscard]] constexpr bool IsHeadless() const noexcept { return m_headless; }
    [[nodiscard]] constexpr ViewportExtent ViewportSize() const noexcept { return m_viewportSize; }
    [[nodiscard]] constexpr PresentMode GetPresentMode() const noexcept { return m_presentMode; }

    constexpr void ResizeViewport(ViewportExtent pixelSize) noexcept {
        if (pixelSize.IsValid()) {
            m_viewportSize = pixelSize;
        }
    }

    [[nodiscard]] Result<void, RendererErrc> SetPresentMode(PresentMode mode) noexcept;

    [[nodiscard]] Result<TextureHandle, RendererErrc> CreateTexture(const TextureDesc& desc) noexcept;

    [[nodiscard]] Result<TextureHandle, RendererErrc> CreateRenderTarget(
        u32 width,
        u32 height,
        TextureFilter filter = TextureFilter::Linear) noexcept {
        TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.filter = filter;
        desc.renderTarget = true;
        return CreateTexture(desc);
    }

    [[nodiscard]] Result<void, RendererErrc> UpdateTexture(
        TextureHandle handle,
        Rect region,
        std::span<const u8> pixels,
        u32 pitch = 0) noexcept;

    void DestroyTexture(TextureHandle handle) noexcept;

    [[nodiscard]] constexpr bool IsTextureValid(TextureHandle handle) const noexcept {
        return handle.IsValid() &&
               handle.index < m_highWaterSlot &&
               m_textures[handle.index].IsOccupied() &&
               m_textures[handle.index].generation == handle.generation;
    }

    [[nodiscard]] constexpr ViewportExtent GetTextureSize(TextureHandle handle) const noexcept {
        if (!IsTextureValid(handle)) return {1, 1};
        const TextureSlot& slot = m_textures[handle.index];
        return {static_cast<i32>(slot.width), static_cast<i32>(slot.height)};
    }

    [[nodiscard]] constexpr u16 HighWaterTextureSlots() const noexcept { return m_highWaterSlot; }
    [[nodiscard]] constexpr u16 LiveTextureCount() const noexcept { return m_liveTextureCount; }

    [[nodiscard]] RenderList& BeginFrame(ViewportExtent pixelSize = {}) noexcept {
        if (pixelSize.IsValid()) {
            m_viewportSize = pixelSize;
        }
        m_renderList.Reset(m_viewportSize);
        return m_renderList;
    }

    void Submit() noexcept { Submit(m_renderList); }
    void Submit(RenderList& list) noexcept;

    [[nodiscard]] constexpr RenderList& List() noexcept { return m_renderList; }
    [[nodiscard]] constexpr const RenderList& List() const noexcept { return m_renderList; }
    [[nodiscard]] constexpr const RenderStats& LastFrameStats() const noexcept { return m_lastStats; }

private:
    [[nodiscard]] Result<TextureHandle, RendererErrc> AllocateTextureSlot(
        u16 slotIdx,
        const TextureDesc& desc) noexcept;

    void BindRenderTarget(u16 targetTexIndex) noexcept;

    // Cache Line 0 (offset 0..63, 64B exact, 0B padding, 0 dead variables)
    SDL_Renderer* m_renderer{nullptr};
    ViewportExtent m_viewportSize{1280, 720};
    RenderStats m_lastStats{};
    std::array<u32, 4> m_blendModes{};
    u16 m_highWaterSlot{0};
    u16 m_freeHead{kInvalidSlot};
    u16 m_liveTextureCount{0};
    u16 m_activeRenderTarget{0};
    PresentMode m_presentMode{PresentMode::VSync};
    bool m_headless{false};
    bool m_initialized{false};
    bool m_vsyncActive{false};

    // Cache-line aligned lazy high-water texture pool (64 KB) + embedded RenderList
    alignas(64) std::array<TextureSlot, kMaxTextures> m_textures{};
    alignas(64) RenderList m_renderList{};
};
static_assert(sizeof(Renderer) == 1393856);

} // namespace en
