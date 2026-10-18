#include "renderer/Renderer.h"

#include "platform/Window.h"

#include <SDL3/SDL_blendmode.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace en::renderer {

namespace {

[[nodiscard]] SDL_BlendMode ToSdlBlendMode(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::AlphaPMA:
            return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
        case BlendMode::Additive:
            return SDL_BLENDMODE_ADD_PREMULTIPLIED;
        case BlendMode::Multiply: {
            static const SDL_BlendMode kMultiplyMode = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_DST_COLOR,
                SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO,
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDOPERATION_ADD);
            return kMultiplyMode;
        }
        case BlendMode::Opaque:
            return SDL_BLENDMODE_NONE;
    }
    return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
}

[[nodiscard]] constexpr SDL_PixelFormat ToSdlPixelFormat(TextureFormat fmt) noexcept {
    return (fmt == TextureFormat::BGRA8) ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32;
}

[[nodiscard]] constexpr SDL_ScaleMode ToSdlScaleMode(TextureFilter filter) noexcept {
    return (filter == TextureFilter::Linear) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
}

[[nodiscard]] SDL_Rect ToSdlRect(const FRect& r) noexcept {
    return SDL_Rect{
        static_cast<int>(std::lround(r.x)),
        static_cast<int>(std::lround(r.y)),
        static_cast<int>(std::lround(r.w)),
        static_cast<int>(std::lround(r.h))
    };
}

} // namespace

Renderer::~Renderer() noexcept {
    Shutdown();
}

Renderer::Renderer(Renderer&& other) noexcept
    : m_renderer(std::exchange(other.m_renderer, nullptr))
    , m_viewportSize(std::exchange(other.m_viewportSize, {}))
    , m_freeHead(std::exchange(other.m_freeHead, 0))
    , m_highWaterSlot(std::exchange(other.m_highWaterSlot, 1))
    , m_textures(std::exchange(other.m_textures, {}))
    , m_list(std::move(other.m_list)) {
    other.m_list.Reset({});
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_renderer      = std::exchange(other.m_renderer, nullptr);
        m_viewportSize  = std::exchange(other.m_viewportSize, {});
        m_freeHead      = std::exchange(other.m_freeHead, 0);
        m_highWaterSlot = std::exchange(other.m_highWaterSlot, 1);
        m_textures      = std::exchange(other.m_textures, {});
        m_list          = std::move(other.m_list);
        other.m_list.Reset({});
    }
    return *this;
}

Result<void, RendererErrc> Renderer::Init(
    platform::Window& window,
    const RendererConfig& cfg) noexcept {
    if (!window.IsValid()) {
        return Err(RendererErrc::DeviceInitFailed);
    }

    Shutdown();

    m_renderer = SDL_CreateRenderer(window.Native(), nullptr);
    if (!m_renderer) {
        return Err(RendererErrc::DeviceInitFailed);
    }

    m_viewportSize = window.State().pixelSize;
    (void)SetPresentMode(cfg.presentMode);

    // Lazy high-water slot allocation avoids a 4096-slot initialization loop across 64 KB.
    m_freeHead      = 0;
    m_highWaterSlot = 1;

    SDL_Texture* whiteTex = SDL_CreateTexture(
        m_renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC,
        1,
        1);
    if (!whiteTex) {
        Shutdown();
        return Err(RendererErrc::TextureCreateFailed);
    }

    const u32 whitePixel = 0xFFFFFFFFu;
    SDL_UpdateTexture(whiteTex, nullptr, &whitePixel, 4);
    SDL_SetTextureScaleMode(whiteTex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(whiteTex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);

    m_textures[0] = TextureSlot{
        .texture     = whiteTex,
        .width       = 1,
        .height      = 1,
        .generation  = 1,
        .cachedBlend = BlendMode::AlphaPMA
    };

    return {};
}

void Renderer::Shutdown() noexcept {
    if (!m_renderer) {
        return;
    }

    for (u16 i = 0; i < m_highWaterSlot; ++i) {
        TextureSlot& slot = m_textures[i];
        if (slot.texture) {
            SDL_DestroyTexture(slot.texture);
            slot.texture = nullptr;
        }
        slot.width      = 0;
        slot.height     = 0;
        slot.generation = 0;
    }

    m_freeHead      = 0;
    m_highWaterSlot = 1;

    SDL_DestroyRenderer(m_renderer);
    m_renderer = nullptr;
}

Result<void, RendererErrc> Renderer::SetPresentMode(PresentMode mode) noexcept {
    if (!m_renderer) {
        return Err(RendererErrc::DeviceInitFailed);
    }

    int vsync = 1;
    if (mode == PresentMode::Immediate) {
        vsync = 0;
    } else if (mode == PresentMode::Adaptive) {
        vsync = SDL_RENDERER_VSYNC_ADAPTIVE;
    }

    if (!SDL_SetRenderVSync(m_renderer, vsync)) {
        if (mode == PresentMode::Adaptive && SDL_SetRenderVSync(m_renderer, 1)) {
            return {};
        }
        return Err(RendererErrc::DeviceInitFailed);
    }
    return {};
}

Result<TextureHandle, RendererErrc> Renderer::CreateTexture(const TextureDesc& desc) noexcept {
    if (!m_renderer) {
        return Err(RendererErrc::DeviceInitFailed);
    }
    if (desc.width == 0 || desc.height == 0) {
        return Err(RendererErrc::InvalidDimension);
    }
    if (m_freeHead == 0 && m_highWaterSlot >= kMaxTextures) {
        return Err(RendererErrc::PoolExhausted);
    }

    const usize minRowBytes    = static_cast<usize>(desc.width) * 4u;
    const usize effectivePitch = (desc.pitch > 0) ? static_cast<usize>(desc.pitch) : minRowBytes;
    if (!desc.pixels.empty()) {
        if (effectivePitch < minRowBytes) {
            return Err(RendererErrc::TextureUpdateFailed);
        }
        const usize requiredBytes = static_cast<usize>(desc.height - 1u) * effectivePitch + minRowBytes;
        if (desc.pixels.size() < requiredBytes) {
            return Err(RendererErrc::TextureUpdateFailed);
        }
    }

    SDL_Texture* tex = SDL_CreateTexture(
        m_renderer,
        ToSdlPixelFormat(desc.format),
        SDL_TEXTUREACCESS_STATIC,
        desc.width,
        desc.height);
    if (!tex) {
        return Err(RendererErrc::TextureCreateFailed);
    }

    if (!desc.pixels.empty()) {
        if (!SDL_UpdateTexture(tex, nullptr, desc.pixels.data(), static_cast<int>(effectivePitch))) {
            SDL_DestroyTexture(tex);
            return Err(RendererErrc::TextureUpdateFailed);
        }
    }

    SDL_SetTextureScaleMode(tex, ToSdlScaleMode(desc.filter));
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);

    u16 slotIdx = 0;
    if (m_freeHead != 0) {
        slotIdx    = m_freeHead;
        m_freeHead = m_textures[slotIdx].width;
    } else {
        slotIdx = m_highWaterSlot++;
    }

    TextureSlot& slot = m_textures[slotIdx];
    const u16 nextGen = static_cast<u16>(slot.generation + 1u);
    slot.texture     = tex;
    slot.width       = desc.width;
    slot.height      = desc.height;
    slot.generation  = (nextGen == 0) ? 1u : nextGen;
    slot.cachedBlend = BlendMode::AlphaPMA;

    return TextureHandle{slotIdx, slot.generation};
}

Result<void, RendererErrc> Renderer::UpdateTexture(
    TextureHandle handle,
    FRect region,
    std::span<const u8> pixels,
    u32 pitch) noexcept {
    if (handle.index == 0 || !IsTextureValid(handle) || pixels.empty()) {
        return Err(RendererErrc::InvalidHandle);
    }

    TextureSlot& slot = m_textures[handle.index];
    SDL_Rect sdlRect{};
    const SDL_Rect* rectPtr = nullptr;
    usize updateW = slot.width;
    usize updateH = slot.height;

    if (!region.IsEmpty()) {
        sdlRect = SDL_Rect{
            static_cast<int>(region.x),
            static_cast<int>(region.y),
            static_cast<int>(region.w),
            static_cast<int>(region.h)
        };
        if (sdlRect.x < 0 || sdlRect.y < 0 || sdlRect.w <= 0 || sdlRect.h <= 0 ||
            sdlRect.x + sdlRect.w > static_cast<int>(slot.width) ||
            sdlRect.y + sdlRect.h > static_cast<int>(slot.height)) {
            return Err(RendererErrc::InvalidDimension);
        }
        rectPtr = &sdlRect;
        updateW = static_cast<usize>(sdlRect.w);
        updateH = static_cast<usize>(sdlRect.h);
    }

    const usize minRowBytes    = updateW * 4u;
    const usize effectivePitch = (pitch > 0) ? static_cast<usize>(pitch) : minRowBytes;
    if (effectivePitch < minRowBytes) {
        return Err(RendererErrc::TextureUpdateFailed);
    }
    const usize requiredBytes = (updateH - 1u) * effectivePitch + minRowBytes;
    if (pixels.size() < requiredBytes) {
        return Err(RendererErrc::TextureUpdateFailed);
    }

    if (!SDL_UpdateTexture(slot.texture, rectPtr, pixels.data(), static_cast<int>(effectivePitch))) {
        return Err(RendererErrc::TextureUpdateFailed);
    }
    return {};
}

void Renderer::DestroyTexture(TextureHandle handle) noexcept {
    if (handle.index == 0 || !IsTextureValid(handle)) {
        return;
    }

    TextureSlot& slot = m_textures[handle.index];
    SDL_DestroyTexture(slot.texture);
    slot.texture = nullptr;

    const u16 invalidatedGen = static_cast<u16>(slot.generation + 1u);
    slot.generation = (invalidatedGen == 0) ? 1u : invalidatedGen;
    slot.width      = m_freeHead;
    slot.height     = 0;
    m_freeHead      = handle.index;
}

RenderList& Renderer::BeginFrame(ViewportExtent pixelSize) noexcept {
    if (pixelSize.IsValid()) {
        m_viewportSize = pixelSize;
    }
    m_list.Reset(m_viewportSize);
    return m_list;
}

void Renderer::Submit(RenderList& list) noexcept {
    if (!m_renderer || !m_viewportSize.IsValid()) {
        return;
    }

    list.Compile();

    const auto batches       = list.Batches();
    const auto sharedIndices = RenderList::SharedQuadIndices(RenderList::kChunkQuads);

    const FRect fullViewport{
        0.0f,
        0.0f,
        static_cast<f32>(m_viewportSize.w),
        static_cast<f32>(m_viewportSize.h)
    };
    FRect activeViewport = fullViewport;
    FRect activeScissor{};
    u8 activeViewSlot    = 0xFFu;
    u16 pendingClearMask = list.ClearPassMask();

    SDL_SetRenderViewport(m_renderer, nullptr);
    SDL_SetRenderClipRect(m_renderer, nullptr);

    auto executeDueClears = [&](u16 dueMask) noexcept {
        if (dueMask == 0) {
            return;
        }
        if (activeViewport != fullViewport) {
            SDL_SetRenderViewport(m_renderer, nullptr);
            activeViewport = fullViewport;
            activeViewSlot = 0xFFu;
        }
        if (!activeScissor.IsEmpty()) {
            SDL_SetRenderClipRect(m_renderer, nullptr);
            activeScissor  = {};
            activeViewSlot = 0xFFu;
        }
        for (u32 m = dueMask; m != 0; m &= (m - 1u)) {
            const u8 passIdx = static_cast<u8>(std::countr_zero(m));
            const Color& c   = list.Pass(passIdx).clearColor;
            SDL_SetRenderDrawColorFloat(m_renderer, c.r, c.g, c.b, c.a);
            SDL_RenderClear(m_renderer);
        }
        pendingClearMask &= static_cast<u16>(~dueMask);
    };

    for (const DrawBatch& batch : batches) {
        const u16 passReachMask = static_cast<u16>((1u << (batch.passId + 1u)) - 1u);
        executeDueClears(pendingClearMask & passReachMask);

        if (batch.viewSlot != activeViewSlot) {
            activeViewSlot = batch.viewSlot;
            const Camera2D& view = list.View(activeViewSlot);
            const FRect targetVp = view.viewport.IsEmpty() ? fullViewport : view.viewport;

            if (targetVp != activeViewport) {
                activeViewport = targetVp;
                if (targetVp == fullViewport) {
                    SDL_SetRenderViewport(m_renderer, nullptr);
                } else {
                    const SDL_Rect vp = ToSdlRect(targetVp);
                    SDL_SetRenderViewport(m_renderer, &vp);
                }
            }

            if (view.scissor != activeScissor) {
                activeScissor = view.scissor;
                if (activeScissor.IsEmpty()) {
                    SDL_SetRenderClipRect(m_renderer, nullptr);
                } else {
                    const SDL_Rect clip = ToSdlRect(activeScissor);
                    SDL_SetRenderClipRect(m_renderer, &clip);
                }
            }
        }

        SDL_Texture* sdlTex = ResolveAndBindTexture(batch.texture, batch.blend);

        u32 remaining = batch.quadCount;
        u32 offset    = batch.firstQuad;

        while (remaining > 0) {
            const u32 chunk = std::min(remaining, RenderList::kChunkQuads);
            list.ExpandVertices(offset, chunk, m_vertexScratch);

            const Vertex2D* vtx = m_vertexScratch.data();
            SDL_RenderGeometryRaw(
                m_renderer,
                sdlTex,
                &vtx->x,
                sizeof(Vertex2D),
                reinterpret_cast<const SDL_FColor*>(&vtx->color),
                sizeof(Vertex2D),
                &vtx->u,
                sizeof(Vertex2D),
                static_cast<int>(chunk * 4),
                sharedIndices.data(),
                static_cast<int>(chunk * 6),
                sizeof(u16));

            remaining -= chunk;
            offset    += chunk;
        }
    }

    executeDueClears(pendingClearMask);
    SDL_RenderPresent(m_renderer);
}

SDL_Texture* Renderer::ResolveAndBindTexture(TextureHandle handle, BlendMode blend) noexcept {
    const u16 idx = IsTextureValid(handle) ? handle.index : 0u;
    TextureSlot& slot = m_textures[idx];

    if (slot.cachedBlend != blend) {
        SDL_SetTextureBlendMode(slot.texture, ToSdlBlendMode(blend));
        slot.cachedBlend = blend;
    }

    return slot.texture;
}

} // namespace en::renderer
