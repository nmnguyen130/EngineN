#pragma once

#include "renderer/RenderList.h"

#include <utility>

struct SDL_Renderer;
struct SDL_Texture;

namespace en {

class Window;

struct RendererConfig {
    PresentMode presentMode{PresentMode::VSync};
    bool        headless{false};
};
static_assert(sizeof(RendererConfig) == 2);

// SDL3 2D rendering backend and generational GPU texture pool (slot 0 = permanent 1x1 white).
class Renderer {
public:
    static constexpr u32 kMaxTextures = 512;
    static constexpr u16 kInvalidSlot = 0xFFFFU;
    static constexpr u8  kSlotOccupied = 1U << 0;
    static constexpr u8  kSlotRenderTarget = 1U << 1;

    struct Slot {
        SDL_Texture* texture{nullptr};
        u16          width{kInvalidSlot};
        u16          height{0};
        u16          generation{1};
        u8           flags{0};
        u8           pad{0};

        [[nodiscard]] constexpr bool Occupied() const noexcept {
            return (flags & kSlotOccupied) != 0U;
        }
        [[nodiscard]] constexpr bool IsRenderTarget() const noexcept {
            return (flags & kSlotRenderTarget) != 0U;
        }
    };
    static_assert(sizeof(Slot) == 16);

    Renderer() noexcept = default;
    ~Renderer() noexcept { Shutdown(); }

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&& other) noexcept;
    Renderer& operator=(Renderer&& other) noexcept;

    [[nodiscard]] Result<void, RendererErrc> Init(Window& window, const RendererConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_initialized; }
    [[nodiscard]] constexpr bool IsHeadless() const noexcept { return m_sdl == nullptr; }
    [[nodiscard]] constexpr PresentMode GetPresentMode() const noexcept { return m_presentMode; }

    [[nodiscard]] Result<void, RendererErrc> SetPresentMode(PresentMode mode) noexcept;

    [[nodiscard]] Result<TextureHandle, RendererErrc> CreateTexture(const TextureDesc& desc) noexcept;
    [[nodiscard]] Result<TextureHandle, RendererErrc> CreateRenderTarget(
        u32 width, u32 height, TextureFilter filter = TextureFilter::Linear) noexcept {
        return CreateTexture(TextureDesc{
            .width = width, .height = height, .filter = filter, .renderTarget = true});
    }

    [[nodiscard]] Result<void, RendererErrc> UpdateTexture(
        TextureHandle handle, Rect region, std::span<const u8> pixels, u32 pitch = 0) noexcept;

    void DestroyTexture(TextureHandle handle) noexcept;

    [[nodiscard]] constexpr bool IsTextureValid(TextureHandle handle) const noexcept {
        return handle.IsValid() &&
               handle.index < m_highWater &&
               m_slots[handle.index].Occupied() &&
               m_slots[handle.index].generation == handle.generation;
    }

    [[nodiscard]] constexpr Viewport TextureSize(TextureHandle handle) const noexcept {
        if (!IsTextureValid(handle)) return {1, 1};
        const Slot& slot = m_slots[handle.index];
        return {static_cast<i32>(slot.width), static_cast<i32>(slot.height)};
    }

    [[nodiscard]] constexpr u16 HighWater() const noexcept { return m_highWater; }
    [[nodiscard]] constexpr u16 LiveCount() const noexcept { return m_liveCount; }

    void Submit(RenderList& list) noexcept;

    [[nodiscard]] constexpr const RenderStats& Stats() const noexcept { return m_stats; }

private:
    void BindRenderTarget(u16 slot) noexcept;

    SDL_Renderer* m_sdl{nullptr};
    RenderStats   m_stats{};
    u16           m_highWater{0};
    u16           m_freeHead{kInvalidSlot};
    u16           m_liveCount{0};
    u16           m_activeTarget{0};
    PresentMode   m_presentMode{PresentMode::VSync};
    bool          m_initialized{false};

    std::array<Slot, kMaxTextures>                      m_slots{};
    std::array<Vertex2D, RenderList::kChunkQuads * 4U>  m_verts{};
};
static_assert(sizeof(Renderer) == 73768);

} // namespace en
