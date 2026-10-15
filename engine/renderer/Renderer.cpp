#include "renderer/Renderer.h"

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
            return SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                SDL_BLENDOPERATION_ADD);
        case BlendMode::Additive:
            return SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO,
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDOPERATION_ADD);
        case BlendMode::Multiply:
            return SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_DST_COLOR,
                SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO,
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDOPERATION_ADD);
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
        return std::unexpected(RendererErrc::DeviceInitFailed);
    }

    Shutdown();

    m_renderer = SDL_CreateRenderer(window.Native(), nullptr);
    if (!m_renderer) {
        return std::unexpected(RendererErrc::DeviceInitFailed);
    }

    m_viewportSize = window.State().pixelSize;
    (void)SetPresentMode(cfg.presentMode);

    // Initialize free-list for slots [1 .. kMaxTextures - 1].
    for (u16 i = 1; i < kMaxTextures - 1; ++i) {
        m_textures[i].nextFree   = static_cast<u16>(i + 1);
        m_textures[i].generation = 0;
    }
    m_textures[kMaxTextures - 1].nextFree   = 0;
    m_textures[kMaxTextures - 1].generation = 0;
    m_freeHead = 1;

    // Permanently allocate Slot 0 as a 1x1 opaque white texture for solid geometry.
    SDL_Texture* whiteTex = SDL_CreateTexture(
        m_renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC,
        1,
        1);
    if (!whiteTex) {
        Shutdown();
        return std::unexpected(RendererErrc::TextureCreateFailed);
    }

    const u32 whitePixel = 0xFFFFFFFFu;
    SDL_UpdateTexture(whiteTex, nullptr, &whitePixel, 4);
    SDL_SetTextureScaleMode(whiteTex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(whiteTex, ToSdlBlendMode(BlendMode::AlphaPMA));

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
        return std::unexpected(RendererErrc::DeviceInitFailed);
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
        return std::unexpected(RendererErrc::DeviceInitFailed);
    }
    return {};
}

Result<TextureHandle, RendererErrc> Renderer::CreateTexture(const TextureDesc& desc) noexcept {
    if (!m_renderer) {
        return std::unexpected(RendererErrc::DeviceInitFailed);
    }
    if (desc.width == 0 || desc.height == 0) {
        return std::unexpected(RendererErrc::InvalidDimension);
    }
    if (m_freeHead == 0) {
        return std::unexpected(RendererErrc::PoolExhausted);
    }

    SDL_Texture* tex = SDL_CreateTexture(
        m_renderer,
        ToSdlPixelFormat(desc.format),
        SDL_TEXTUREACCESS_STATIC,
        desc.width,
        desc.height);
    if (!tex) {
        return std::unexpected(RendererErrc::TextureCreateFailed);
    }

    if (!desc.pixels.empty()) {
        const int pitch = (desc.pitch > 0) ? static_cast<int>(desc.pitch) : static_cast<int>(desc.width * 4);
        if (!SDL_UpdateTexture(tex, nullptr, desc.pixels.data(), pitch)) {
            SDL_DestroyTexture(tex);
            return std::unexpected(RendererErrc::TextureUpdateFailed);
        }
    }

    SDL_SetTextureScaleMode(tex, ToSdlScaleMode(desc.filter));
    SDL_SetTextureBlendMode(tex, ToSdlBlendMode(BlendMode::AlphaPMA));

    const u16 slotIdx = m_freeHead;
    TextureSlot& slot = m_textures[slotIdx];
    m_freeHead        = slot.nextFree;

    u16 nextGen = static_cast<u16>(slot.generation + 1);
    if (nextGen == 0) {
        nextGen = 1;
    }

    slot.texture     = tex;
    slot.width       = desc.width;
    slot.height      = desc.height;
    slot.generation  = nextGen;
    slot.nextFree    = 0;
    slot.cachedBlend = BlendMode::AlphaPMA;

    return TextureHandle{slotIdx, nextGen};
}

Result<void, RendererErrc> Renderer::UpdateTexture(
    TextureHandle handle,
    FRect region,
    std::span<const u8> pixels,
    u32 pitch) noexcept {
    if (handle.index == 0 || !IsTextureValid(handle) || pixels.empty()) {
        return std::unexpected(RendererErrc::InvalidHandle);
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
        return std::unexpected(RendererErrc::TextureUpdateFailed);
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

    // Increment generation on destruction so stale handles fail validation immediately.
    u16 invalidatedGen = static_cast<u16>(slot.generation + 1);
    if (invalidatedGen == 0) {
        invalidatedGen = 1;
    }
    slot.generation = invalidatedGen;
    slot.nextFree   = m_freeHead;
    m_freeHead      = handle.index;
}

RenderList& Renderer::BeginFrame() noexcept {
    m_list.Reset(
        static_cast<f32>(m_viewportSize.w),
        static_cast<f32>(m_viewportSize.h)
    );
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

    // Clear enabled passes even if no geometry was submitted to them.
    if (batches.empty()) {
        for (u8 p = 0; p < RenderList::kMaxPasses; ++p) {
            const RenderPassDesc& pass = list.Pass(p);
            if (pass.enabled && pass.clear) {
                SDL_SetRenderViewport(m_renderer, nullptr);
                SDL_SetRenderClipRect(m_renderer, nullptr);
                SDL_SetRenderDrawColorFloat(
                    m_renderer,
                    pass.clearColor.r,
                    pass.clearColor.g,
                    pass.clearColor.b,
                    pass.clearColor.a);
                SDL_RenderClear(m_renderer);
            }
        }
        SDL_RenderPresent(m_renderer);
        return;
    }

    u8 activePass = 0xFFu;
    u8 activeView = 0xFFu;

    for (const DrawBatch& batch : batches) {
        if (batch.passId != activePass) {
            activePass = batch.passId;
            const RenderPassDesc& pass = list.Pass(activePass);
            if (pass.clear) {
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
    // Fallback to Slot 0 (1x1 White Texture) if the handle was unloaded or invalidated.
    const u16 idx = IsTextureValid(handle) ? handle.index : 0u;
    TextureSlot& slot = m_textures[idx];

    if (slot.cachedBlend != blend) {
        SDL_SetTextureBlendMode(slot.texture, ToSdlBlendMode(blend));
        slot.cachedBlend = blend;
    }

    return slot.texture;
}

} // namespace en::renderer