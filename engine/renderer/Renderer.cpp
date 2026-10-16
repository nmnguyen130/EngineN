#include "renderer/Renderer.h"

#include "platform/Window.h"

#include <SDL3/SDL_blendmode.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>

#include <algorithm>
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

} // namespace

Renderer::~Renderer() noexcept {
    Shutdown();
}

Renderer::Renderer(Renderer&& other) noexcept
    : m_renderer(std::exchange(other.m_renderer, nullptr))
    , m_viewportSize(other.m_viewportSize)
    , m_textures(std::exchange(other.m_textures, {}))
    , m_freeHead(other.m_freeHead)
    , m_list(std::move(other.m_list)) {}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_renderer     = std::exchange(other.m_renderer, nullptr);
        m_viewportSize = other.m_viewportSize;
        m_textures     = std::exchange(other.m_textures, {});
        m_freeHead     = other.m_freeHead;
        m_list         = std::move(other.m_list);
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

    for (u16 i = 1; i < kMaxTextures - 1; ++i) {
        m_textures[i].nextFree   = static_cast<u16>(i + 1);
        m_textures[i].generation = 0;
    }
    m_textures[kMaxTextures - 1].nextFree   = 0;
    m_textures[kMaxTextures - 1].generation = 0;
    m_freeHead = 1;

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
        .nextFree    = 0,
        .cachedBlend = BlendMode::AlphaPMA
    };

    return {};
}

void Renderer::Shutdown() noexcept {
    if (!m_renderer) {
        return;
    }

    for (TextureSlot& slot : m_textures) {
        if (slot.texture) {
            SDL_DestroyTexture(slot.texture);
            slot.texture = nullptr;
        }
        slot.generation = 0;
    }

    SDL_DestroyRenderer(m_renderer);
    m_renderer = nullptr;
}

void Renderer::ResizeViewport(platform::Extent2i pixelSize) noexcept {
    m_viewportSize = pixelSize;
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
    if (m_freeHead == 0) {
        return Err(RendererErrc::PoolExhausted);
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
        const int pitch = (desc.pitch > 0) ? static_cast<int>(desc.pitch) : static_cast<int>(desc.width * 4);
        if (!SDL_UpdateTexture(tex, nullptr, desc.pixels.data(), pitch)) {
            SDL_DestroyTexture(tex);
            return Err(RendererErrc::TextureUpdateFailed);
        }
    }

    SDL_SetTextureScaleMode(tex, ToSdlScaleMode(desc.filter));
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);

    const u16 slotIdx = m_freeHead;
    TextureSlot& slot = m_textures[slotIdx];
    m_freeHead        = slot.nextFree;

    const u16 nextGen = static_cast<u16>(slot.generation + 1);
    slot.texture     = tex;
    slot.width       = desc.width;
    slot.height      = desc.height;
    slot.generation  = (nextGen == 0) ? 1u : nextGen;
    slot.nextFree    = 0;
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

    if (!region.IsEmpty()) {
        sdlRect = SDL_Rect{
            static_cast<int>(region.x),
            static_cast<int>(region.y),
            static_cast<int>(region.w),
            static_cast<int>(region.h)
        };
        rectPtr = &sdlRect;
    }

    const int rowPitch = (pitch > 0)
        ? static_cast<int>(pitch)
        : static_cast<int>((rectPtr ? sdlRect.w : slot.width) * 4);

    if (!SDL_UpdateTexture(slot.texture, rectPtr, pixels.data(), rowPitch)) {
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

    const u16 invalidatedGen = static_cast<u16>(slot.generation + 1);
    slot.generation = (invalidatedGen == 0) ? 1u : invalidatedGen;
    slot.nextFree   = m_freeHead;
    m_freeHead      = handle.index;
}

RenderList& Renderer::BeginFrame() noexcept {
    m_list.Reset(m_viewportSize);
    return m_list;
}

void Renderer::Submit() noexcept {
    Submit(m_list);
}

void Renderer::Submit(RenderList& list) noexcept {
    if (!m_renderer || m_viewportSize.w <= 0 || m_viewportSize.h <= 0) {
        return;
    }

    list.Compile();

    const auto batches = list.Batches();
    const auto sharedIndices = RenderList::SharedQuadIndices(RenderList::kChunkQuads);

    u8 nextPassToClear = 0;
    u8 activeView = 0xFFu;

    auto clearPassesUpTo = [&](u8 targetPass) noexcept {
        while (nextPassToClear <= targetPass && nextPassToClear < RenderList::kMaxPasses) {
            const RenderPassDesc& pass = list.Pass(nextPassToClear++);
            if (pass.enabled && pass.clear) {
                SDL_SetRenderViewport(m_renderer, nullptr);
                SDL_SetRenderClipRect(m_renderer, nullptr);
                activeView = 0xFFu;
                SDL_SetRenderDrawColorFloat(
                    m_renderer,
                    pass.clearColor.r,
                    pass.clearColor.g,
                    pass.clearColor.b,
                    pass.clearColor.a);
                SDL_RenderClear(m_renderer);
            }
        }
    };

    for (const DrawBatch& batch : batches) {
        if (batch.passId >= nextPassToClear) {
            clearPassesUpTo(batch.passId);
        }

        if (batch.viewSlot != activeView) {
            activeView = batch.viewSlot;
            ApplyCameraView(list.View(activeView));
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

    clearPassesUpTo(static_cast<u8>(RenderList::kMaxPasses - 1));
    SDL_RenderPresent(m_renderer);
}

void Renderer::ApplyCameraView(const Camera2D& view) noexcept {
    if (view.viewport.IsEmpty()) {
        SDL_SetRenderViewport(m_renderer, nullptr);
    } else {
        const SDL_Rect vp{
            static_cast<int>(std::lround(view.viewport.x)),
            static_cast<int>(std::lround(view.viewport.y)),
            static_cast<int>(std::lround(view.viewport.w)),
            static_cast<int>(std::lround(view.viewport.h))
        };
        SDL_SetRenderViewport(m_renderer, &vp);
    }

    if (view.scissor.IsEmpty()) {
        SDL_SetRenderClipRect(m_renderer, nullptr);
    } else {
        const SDL_Rect clip{
            static_cast<int>(std::lround(view.scissor.x)),
            static_cast<int>(std::lround(view.scissor.y)),
            static_cast<int>(std::lround(view.scissor.w)),
            static_cast<int>(std::lround(view.scissor.h))
        };
        SDL_SetRenderClipRect(m_renderer, &clip);
    }
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