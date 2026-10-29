#include "renderer/Renderer.h"

#include "platform/Window.h"

#include <SDL3/SDL.h>

namespace en {
namespace {

constexpr u8 kWhitePx[4]{255, 255, 255, 255};

[[nodiscard]] SDL_ScaleMode ToScale(TextureFilter filter) noexcept {
    switch (filter) {
        case TextureFilter::Linear: return SDL_SCALEMODE_LINEAR;
        case TextureFilter::PixelArt:
#if defined(SDL_SCALEMODE_PIXELART)
            return SDL_SCALEMODE_PIXELART;
#else
            return SDL_SCALEMODE_NEAREST;
#endif
        default: return SDL_SCALEMODE_NEAREST;
    }
}

[[nodiscard]] SDL_BlendMode ToBlend(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Additive: return SDL_BLENDMODE_ADD_PREMULTIPLIED;
        case BlendMode::Multiply: {
            static const SDL_BlendMode kMultiply = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_DST_COLOR, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
            return kMultiply;
        }
        case BlendMode::Opaque: return SDL_BLENDMODE_NONE;
        default: return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
    }
}

[[nodiscard]] SDL_Rect ToPixelViewport(Viewport windowPx, Rect norm) noexcept {
    const f32 fw = static_cast<f32>(windowPx.w);
    const f32 fh = static_cast<f32>(windowPx.h);
    return {
        static_cast<i32>(norm.x * fw + 0.5f),
        static_cast<i32>(norm.y * fh + 0.5f),
        std::max(1, static_cast<i32>(norm.w * fw + 0.5f)),
        std::max(1, static_cast<i32>(norm.h * fh + 0.5f)),
    };
}

} // namespace

Renderer::Renderer(Renderer&& other) noexcept
    : m_sdl(std::exchange(other.m_sdl, nullptr)),
      m_stats(other.m_stats),
      m_highWater(std::exchange(other.m_highWater, 0U)),
      m_freeHead(std::exchange(other.m_freeHead, kInvalidSlot)),
      m_liveCount(std::exchange(other.m_liveCount, 0U)),
      m_activeTarget(std::exchange(other.m_activeTarget, 0U)),
      m_presentMode(other.m_presentMode),
      m_initialized(std::exchange(other.m_initialized, false)) {
    for (u16 i = 0; i < m_highWater; ++i) {
        m_slots[i] = std::exchange(other.m_slots[i], Slot{});
    }
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_sdl          = std::exchange(other.m_sdl, nullptr);
        m_stats        = other.m_stats;
        m_highWater    = std::exchange(other.m_highWater, 0U);
        m_freeHead     = std::exchange(other.m_freeHead, kInvalidSlot);
        m_liveCount    = std::exchange(other.m_liveCount, 0U);
        m_activeTarget = std::exchange(other.m_activeTarget, 0U);
        m_presentMode  = other.m_presentMode;
        m_initialized  = std::exchange(other.m_initialized, false);
        for (u16 i = 0; i < m_highWater; ++i) {
            m_slots[i] = std::exchange(other.m_slots[i], Slot{});
        }
    }
    return *this;
}

Result<void, RendererErrc> Renderer::Init(Window& window, const RendererConfig& cfg) noexcept {
    Shutdown();
    if (!cfg.headless && !window.IsValid()) return Err(RendererErrc::DeviceInitFailed);

    m_presentMode = cfg.presentMode;
    if (!cfg.headless && window.Native()) {
        m_sdl = SDL_CreateRenderer(window.Native(), nullptr);
        if (!m_sdl) return Err(RendererErrc::DeviceInitFailed);
        (void)SetPresentMode(cfg.presentMode);
    }
    m_initialized = true;

    const auto white = CreateTexture(TextureDesc{
        .pixels = {kWhitePx, 4}, .width = 1, .height = 1, .pitch = 4,
        .filter = TextureFilter::Nearest,
    });
    if (!white || white.value() != kWhiteTexture) {
        Shutdown();
        return Err(RendererErrc::TextureCreateFailed);
    }
    return {};
}

void Renderer::Shutdown() noexcept {
    for (u16 i = 0; i < m_highWater; ++i) {
        if (m_slots[i].texture && m_sdl) SDL_DestroyTexture(m_slots[i].texture);
        m_slots[i] = {};
    }
    if (m_sdl) {
        SDL_DestroyRenderer(m_sdl);
        m_sdl = nullptr;
    }
    m_stats        = {};
    m_highWater    = 0;
    m_freeHead     = kInvalidSlot;
    m_liveCount    = 0;
    m_activeTarget = 0;
    m_initialized  = false;
}

Result<void, RendererErrc> Renderer::SetPresentMode(PresentMode mode) noexcept {
    m_presentMode = mode;
    if (!m_sdl) return {};
    int vsync = 0;
    if (mode == PresentMode::VSync) vsync = 1;
    else if (mode == PresentMode::AdaptiveVSync) vsync = SDL_RENDERER_VSYNC_ADAPTIVE;
    if (!SDL_SetRenderVSync(m_sdl, vsync)) {
        if (mode == PresentMode::AdaptiveVSync && SDL_SetRenderVSync(m_sdl, 1)) return {};
        return Err(RendererErrc::DeviceInitFailed);
    }
    return {};
}

Result<TextureHandle, RendererErrc> Renderer::CreateTexture(const TextureDesc& desc) noexcept {
    if (!m_initialized) return Err(RendererErrc::DeviceInitFailed);
    if (desc.width == 0 || desc.height == 0 || desc.width > 16384 || desc.height > 16384) {
        return Err(RendererErrc::InvalidDimension);
    }
    const u32 rowBytes = desc.width * 4;
    const u32 pitch    = desc.pitch ? desc.pitch : rowBytes;
    if (pitch < rowBytes) return Err(RendererErrc::InvalidDimension);
    if (!desc.pixels.empty()) {
        const usize need = static_cast<usize>(desc.height - 1) * pitch + rowBytes;
        if (desc.pixels.size() < need) return Err(RendererErrc::TextureCreateFailed);
    }
    if (m_freeHead == kInvalidSlot && m_highWater >= kMaxTextures) {
        return Err(RendererErrc::PoolExhausted);
    }

    SDL_Texture* sdlTex = nullptr;
    if (m_sdl) {
        const SDL_PixelFormat fmt =
            (desc.format == TextureFormat::BGRA8) ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32;
        const SDL_TextureAccess access =
            desc.renderTarget ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STATIC;
        sdlTex = SDL_CreateTexture(
            m_sdl, fmt, access, static_cast<int>(desc.width), static_cast<int>(desc.height));
        if (!sdlTex) return Err(RendererErrc::TextureCreateFailed);
        if (!desc.pixels.empty() &&
            !SDL_UpdateTexture(sdlTex, nullptr, desc.pixels.data(), static_cast<int>(pitch))) {
            SDL_DestroyTexture(sdlTex);
            return Err(RendererErrc::TextureUpdateFailed);
        }
        SDL_SetTextureScaleMode(sdlTex, ToScale(desc.filter));
        SDL_SetTextureBlendMode(sdlTex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }

    u16 slotIndex = m_freeHead;
    if (slotIndex != kInvalidSlot) {
        m_freeHead = m_slots[slotIndex].width;
    } else {
        slotIndex = m_highWater++;
    }
    Slot& slot = m_slots[slotIndex];
    if (slot.generation == 0) slot.generation = 1;
    slot.texture = sdlTex;
    slot.width   = static_cast<u16>(desc.width);
    slot.height  = static_cast<u16>(desc.height);
    slot.flags   = static_cast<u8>(kSlotOccupied | (desc.renderTarget ? kSlotRenderTarget : 0));
    ++m_liveCount;
    return TextureHandle{slotIndex, slot.generation};
}

Result<void, RendererErrc> Renderer::UpdateTexture(
    TextureHandle handle, Rect region, std::span<const u8> pixels, u32 pitch) noexcept {
    if (!IsTextureValid(handle)) return Err(RendererErrc::InvalidHandle);
    const Slot& slot = m_slots[handle.index];
    const bool full = (region.x == 0 && region.y == 0 && region.w == 0 && region.h == 0);
    if (!full && (region.x < 0 || region.y < 0 || region.w < 1 || region.h < 1)) {
        return Err(RendererErrc::InvalidDimension);
    }

    const i32 rx = full ? 0 : static_cast<i32>(region.x);
    const i32 ry = full ? 0 : static_cast<i32>(region.y);
    const i32 rw = full ? static_cast<i32>(slot.width)  : static_cast<i32>(region.w);
    const i32 rh = full ? static_cast<i32>(slot.height) : static_cast<i32>(region.h);
    if (rx < 0 || ry < 0 || rw <= 0 || rh <= 0 ||
        rx + rw > static_cast<i32>(slot.width) || ry + rh > static_cast<i32>(slot.height)) {
        return Err(RendererErrc::InvalidDimension);
    }

    const u32 rowBytes    = static_cast<u32>(rw) * 4;
    const u32 actualPitch = pitch ? pitch : rowBytes;
    if (actualPitch < rowBytes) return Err(RendererErrc::InvalidDimension);
    const usize need = static_cast<usize>(rh - 1) * actualPitch + rowBytes;
    if (pixels.size() < need) return Err(RendererErrc::TextureUpdateFailed);

    if (m_sdl && slot.texture) {
        const SDL_Rect rect{rx, ry, rw, rh};
        if (!SDL_UpdateTexture(slot.texture, &rect, pixels.data(), static_cast<int>(actualPitch))) {
            return Err(RendererErrc::TextureUpdateFailed);
        }
    }
    return {};
}

void Renderer::DestroyTexture(TextureHandle handle) noexcept {
    if (handle.index == 0 || !IsTextureValid(handle)) return;
    if (m_activeTarget == handle.index) BindRenderTarget(0);

    Slot& slot = m_slots[handle.index];
    if (slot.texture && m_sdl) {
        SDL_DestroyTexture(slot.texture);
        slot.texture = nullptr;
    }
    slot.flags = 0;
    if (++slot.generation == 0) slot.generation = 1;
    slot.width = m_freeHead;
    m_freeHead = handle.index;
    if (m_liveCount > 0) --m_liveCount;
}

void Renderer::BindRenderTarget(u16 slotIndex) noexcept {
    if (m_activeTarget == slotIndex) return;
    m_activeTarget = slotIndex;
    SDL_Texture* target = nullptr;
    if (slotIndex > 0 && slotIndex < m_highWater &&
        m_slots[slotIndex].Occupied() && m_slots[slotIndex].IsRenderTarget()) {
        target = m_slots[slotIndex].texture;
    }
    if (m_sdl) SDL_SetRenderTarget(m_sdl, target);
}

void Renderer::Submit(RenderList& list) noexcept {
    list.Compile();
    m_stats = list.Stats();
    if (!m_sdl) return;

    const Viewport vp = list.GetViewport();
    constexpr Rect kFull{0, 0, 1, 1};
    u8 pendingClears    = list.ClearPassMask();
    u8 activePass       = 0xFF;
    u8 activeView       = 0xFF;
    Rect activeViewport{-1, -1, -1, -1};
    Rect activeScissor{-1, -1, -1, -1};

    auto flushClears = [&](u8 upToPass) noexcept {
        const u8 mask = static_cast<u8>((2U << (upToPass & 0x03U)) - 1U);
        u8 todo = static_cast<u8>(pendingClears & mask);
        while (todo) {
            const u8 passId = static_cast<u8>(std::countr_zero(todo));
            todo &= static_cast<u8>(todo - 1);
            const PassDesc& desc = list.Pass(passId);
            BindRenderTarget(desc.target);
            SDL_SetRenderViewport(m_sdl, nullptr);
            SDL_SetRenderClipRect(m_sdl, nullptr);
            SDL_SetRenderDrawColorFloat(
                m_sdl, desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a);
            SDL_RenderClear(m_sdl);
            activeView     = 0xFF;
            activeViewport = kFull;
            activeScissor  = kFull;
        }
        pendingClears &= static_cast<u8>(~mask);
    };

    for (const DrawBatch& batch : list.Batches()) {
        if (batch.pass != activePass) {
            flushClears(batch.pass);
            activePass = batch.pass;
            BindRenderTarget(list.Pass(activePass).target);
        }

        if (batch.view != activeView) {
            activeView = batch.view;
            const Camera2D& cam = list.View(activeView);

            if (cam.viewport != activeViewport) {
                activeViewport = cam.viewport;
                if (cam.viewport == kFull) {
                    SDL_SetRenderViewport(m_sdl, nullptr);
                } else {
                    const SDL_Rect rect = ToPixelViewport(vp, cam.viewport);
                    SDL_SetRenderViewport(m_sdl, &rect);
                }
            }
            if (cam.scissor != activeScissor) {
                activeScissor = cam.scissor;
                if (cam.scissor.x <= 0 && cam.scissor.y <= 0 &&
                    cam.scissor.w >= 1 && cam.scissor.h >= 1) {
                    SDL_SetRenderClipRect(m_sdl, nullptr);
                } else {
                    const SDL_Rect viewRect = ToPixelViewport(vp, cam.viewport);
                    const SDL_Rect clipRect{
                        static_cast<i32>(cam.scissor.x * static_cast<f32>(viewRect.w) + 0.5f),
                        static_cast<i32>(cam.scissor.y * static_cast<f32>(viewRect.h) + 0.5f),
                        std::max(0, static_cast<i32>(cam.scissor.w * static_cast<f32>(viewRect.w) + 0.5f)),
                        std::max(0, static_cast<i32>(cam.scissor.h * static_cast<f32>(viewRect.h) + 0.5f)),
                    };
                    SDL_SetRenderClipRect(m_sdl, &clipRect);
                }
            }
        }

        const u16 texIndex  = IsTextureValid(batch.texture) ? batch.texture.index : 0U;
        SDL_Texture* sdlTex = m_slots[texIndex].texture;
        if (sdlTex) SDL_SetTextureBlendMode(sdlTex, ToBlend(batch.blend));

        u32 remaining = batch.count;
        u32 current   = batch.first;
        while (remaining) {
            const u32 chunk = std::min(remaining, RenderList::kChunkQuads);
            list.Expand(current, chunk, m_verts);
            const auto indices = RenderList::Indices(chunk);
            SDL_RenderGeometryRaw(
                m_sdl, sdlTex,
                &m_verts[0].x, static_cast<int>(sizeof(Vertex2D)),
                reinterpret_cast<const SDL_FColor*>(&m_verts[0].r),
                static_cast<int>(sizeof(Vertex2D)),
                &m_verts[0].u, static_cast<int>(sizeof(Vertex2D)),
                static_cast<int>(chunk * 4),
                indices.data(), static_cast<int>(indices.size()), static_cast<int>(sizeof(u16)));
            current   += chunk;
            remaining -= chunk;
        }
    }

    if (pendingClears) flushClears(RenderList::kMaxPasses - 1);
    BindRenderTarget(0);
    SDL_RenderPresent(m_sdl);
}

} // namespace en
