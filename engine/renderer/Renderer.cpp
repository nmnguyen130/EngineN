#include "renderer/Renderer.h"

#include "platform/Window.h"

#include <SDL3/SDL.h>

namespace en {

namespace {

constexpr u8 kFallbackWhitePixel[4]{255U, 255U, 255U, 255U};

[[nodiscard]] SDL_ScaleMode ToSdlScaleMode(TextureFilter filter) noexcept {
    switch (filter) {
        case TextureFilter::Linear:
            return SDL_SCALEMODE_LINEAR;
        case TextureFilter::PixelArt:
            #if defined(SDL_SCALEMODE_PIXELART)
                return SDL_SCALEMODE_PIXELART;
            #else
                return SDL_SCALEMODE_NEAREST;
            #endif
        case TextureFilter::Nearest:
        default:
            return SDL_SCALEMODE_NEAREST;
    }
}

[[nodiscard]] SDL_BlendMode ToSdlBlendMode(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Additive:
            return SDL_BLENDMODE_ADD_PREMULTIPLIED;
        case BlendMode::Multiply: {
            static const SDL_BlendMode kMultiply = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_DST_COLOR,
                SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO,
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDOPERATION_ADD);
            return kMultiply;
        }
        case BlendMode::Opaque:
            return SDL_BLENDMODE_NONE;
        case BlendMode::Alpha:
        default:
            return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
    }
}

[[nodiscard]] SDL_Rect ComputePixelViewport(ViewportExtent windowPx, Rect normVp) noexcept {
    const f32 fw = static_cast<f32>(windowPx.w);
    const f32 fh = static_cast<f32>(windowPx.h);
    return SDL_Rect{
        static_cast<i32>(normVp.x * fw + 0.5f),
        static_cast<i32>(normVp.y * fh + 0.5f),
        std::max(1, static_cast<i32>(normVp.w * fw + 0.5f)),
        std::max(1, static_cast<i32>(normVp.h * fh + 0.5f)),
    };
}

} // namespace

Renderer::Renderer(Renderer&& other) noexcept
    : m_renderer(std::exchange(other.m_renderer, nullptr)),
      m_viewportSize(other.m_viewportSize),
      m_lastStats(other.m_lastStats),
      m_highWaterSlot(std::exchange(other.m_highWaterSlot, 0U)),
      m_freeHead(std::exchange(other.m_freeHead, kInvalidSlot)),
      m_liveTextureCount(std::exchange(other.m_liveTextureCount, 0U)),
      m_activeRenderTarget(std::exchange(other.m_activeRenderTarget, 0U)),
      m_presentMode(other.m_presentMode),
      m_initialized(std::exchange(other.m_initialized, false)) {
    for (u16 i = 0U; i < m_highWaterSlot; ++i) {
        m_textures[i] = std::exchange(other.m_textures[i], TextureSlot{});
    }
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_renderer = std::exchange(other.m_renderer, nullptr);
        m_viewportSize = other.m_viewportSize;
        m_lastStats = other.m_lastStats;
        m_highWaterSlot = std::exchange(other.m_highWaterSlot, 0U);
        m_freeHead = std::exchange(other.m_freeHead, kInvalidSlot);
        m_liveTextureCount = std::exchange(other.m_liveTextureCount, 0U);
        m_activeRenderTarget = std::exchange(other.m_activeRenderTarget, 0U);
        m_presentMode = other.m_presentMode;
        m_initialized = std::exchange(other.m_initialized, false);
        for (u16 i = 0U; i < m_highWaterSlot; ++i) {
            m_textures[i] = std::exchange(other.m_textures[i], TextureSlot{});
        }
    }
    return *this;
}

Result<void, RendererErrc> Renderer::Init(Window& window, const RendererConfig& cfg) noexcept {
    Shutdown();

    m_viewportSize = window.State().pixelSize.IsValid() ? ViewportExtent{window.State().pixelSize}
                                                        : ViewportExtent{1280, 720};
    m_presentMode = cfg.presentMode;

    if (!cfg.headless && window.Native() != nullptr) {
        m_renderer = SDL_CreateRenderer(window.Native(), nullptr);
        if (!m_renderer) return Err(RendererErrc::DeviceInitFailed);
        (void)SetPresentMode(cfg.presentMode);
    }

    const auto whiteRes = CreateTexture(TextureDesc{
        .pixels = {kFallbackWhitePixel, 4U},
        .width = 1U,
        .height = 1U,
        .pitch = 4U,
        .filter = TextureFilter::Nearest,
    });
    if (!whiteRes.has_value() || whiteRes.value() != kWhiteTexture) {
        Shutdown();
        return Err(RendererErrc::TextureCreateFailed);
    }

    m_initialized = true;
    return {};
}

void Renderer::Shutdown() noexcept {
    for (u16 i = 0U; i < m_highWaterSlot; ++i) {
        if (m_textures[i].texture && m_renderer) {
            SDL_DestroyTexture(m_textures[i].texture);
        }
        m_textures[i] = TextureSlot{};
    }
    if (m_renderer) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    m_lastStats = RenderStats{};
    m_highWaterSlot = 0U;
    m_freeHead = kInvalidSlot;
    m_liveTextureCount = 0U;
    m_activeRenderTarget = 0U;
    m_initialized = false;
}

Result<void, RendererErrc> Renderer::SetPresentMode(PresentMode mode) noexcept {
    m_presentMode = mode;
    if (!m_renderer) return {};

    int vsyncVal = 0;
    if (mode == PresentMode::VSync) vsyncVal = 1;
    else if (mode == PresentMode::AdaptiveVSync) vsyncVal = SDL_RENDERER_VSYNC_ADAPTIVE;

    if (!SDL_SetRenderVSync(m_renderer, vsyncVal)) {
        if (mode == PresentMode::AdaptiveVSync && SDL_SetRenderVSync(m_renderer, 1)) return {};
        return Err(RendererErrc::DeviceInitFailed);
    }
    return {};
}

Result<TextureHandle, RendererErrc> Renderer::CreateTexture(const TextureDesc& desc) noexcept {
    if (desc.width == 0U || desc.height == 0U || desc.width > 16384U || desc.height > 16384U) {
        return Err(RendererErrc::InvalidDimension);
    }
    const u32 rowBytes = desc.width * 4U;
    const u32 pitch = desc.pitch != 0U ? desc.pitch : rowBytes;
    if (pitch < rowBytes) return Err(RendererErrc::InvalidDimension);

    if (!desc.pixels.empty()) {
        const usize minBytes = static_cast<usize>(desc.height - 1U) * pitch + rowBytes;
        if (desc.pixels.size() < minBytes) return Err(RendererErrc::TextureCreateFailed);
    }
    if (m_freeHead == kInvalidSlot && m_highWaterSlot >= kMaxTextures) {
        return Err(RendererErrc::PoolExhausted);
    }

    SDL_Texture* sdlTex = nullptr;
    if (m_renderer) {
        const SDL_PixelFormat fmt =
            (desc.format == TextureFormat::BGRA8) ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32;
        const SDL_TextureAccess access =
            desc.renderTarget ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STATIC;
        sdlTex = SDL_CreateTexture(
            m_renderer, fmt, access, static_cast<int>(desc.width), static_cast<int>(desc.height));
        if (!sdlTex) return Err(RendererErrc::TextureCreateFailed);

        if (!desc.pixels.empty() &&
            !SDL_UpdateTexture(sdlTex, nullptr, desc.pixels.data(), static_cast<int>(pitch))) {
            SDL_DestroyTexture(sdlTex);
            return Err(RendererErrc::TextureUpdateFailed);
        }
        SDL_SetTextureScaleMode(sdlTex, ToSdlScaleMode(desc.filter));
        SDL_SetTextureBlendMode(sdlTex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }

    u16 slotIdx = m_freeHead;
    if (slotIdx != kInvalidSlot) {
        m_freeHead = m_textures[slotIdx].width;
    } else {
        slotIdx = m_highWaterSlot++;
    }

    TextureSlot& slot = m_textures[slotIdx];
    if (slot.generation == 0U) slot.generation = 1U;
    slot.texture = sdlTex;
    slot.width = static_cast<u16>(desc.width);
    slot.height = static_cast<u16>(desc.height);
    slot.flags = static_cast<u8>(kSlotOccupied | (desc.renderTarget ? kSlotRenderTarget : 0U));
    ++m_liveTextureCount;
    return TextureHandle{slotIdx, slot.generation};
}

Result<void, RendererErrc> Renderer::UpdateTexture(
    TextureHandle handle, Rect region, std::span<const u8> pixels, u32 pitch) noexcept {
    if (!IsTextureValid(handle)) return Err(RendererErrc::InvalidHandle);

    const TextureSlot& slot = m_textures[handle.index];
    const i32 rx = region.IsEmpty() ? 0 : static_cast<i32>(region.x);
    const i32 ry = region.IsEmpty() ? 0 : static_cast<i32>(region.y);
    const i32 rw = region.IsEmpty() ? static_cast<i32>(slot.width) : static_cast<i32>(region.w);
    const i32 rh = region.IsEmpty() ? static_cast<i32>(slot.height) : static_cast<i32>(region.h);

    if (rx < 0 || ry < 0 || rw <= 0 || rh <= 0 ||
        (rx + rw) > static_cast<i32>(slot.width) ||
        (ry + rh) > static_cast<i32>(slot.height)) {
        return Err(RendererErrc::InvalidDimension);
    }

    const u32 rowBytes = static_cast<u32>(rw) * 4U;
    const u32 actualPitch = pitch != 0U ? pitch : rowBytes;
    if (actualPitch < rowBytes) return Err(RendererErrc::InvalidDimension);

    const usize minBytes = static_cast<usize>(rh - 1) * actualPitch + rowBytes;
    if (pixels.size() < minBytes) return Err(RendererErrc::TextureUpdateFailed);

    if (m_renderer && slot.texture) {
        const SDL_Rect sdlRect{rx, ry, rw, rh};
        if (!SDL_UpdateTexture(slot.texture, &sdlRect, pixels.data(), static_cast<int>(actualPitch))) {
            return Err(RendererErrc::TextureUpdateFailed);
        }
    }
    return {};
}

void Renderer::DestroyTexture(TextureHandle handle) noexcept {
    if (handle.index == 0U || !IsTextureValid(handle)) return;

    TextureSlot& slot = m_textures[handle.index];
    if (slot.texture && m_renderer) {
        SDL_DestroyTexture(slot.texture);
        slot.texture = nullptr;
    }
    slot.flags = 0U;
    if (++slot.generation == 0U) slot.generation = 1U;
    slot.width = m_freeHead;
    m_freeHead = handle.index;
    if (m_liveTextureCount > 0U) --m_liveTextureCount;
}

void Renderer::BindRenderTarget(u16 targetTexIndex) noexcept {
    if (m_activeRenderTarget == targetTexIndex) return;
    m_activeRenderTarget = targetTexIndex;

    SDL_Texture* targetSdlTex = nullptr;
    if (targetTexIndex > 0U && targetTexIndex < m_highWaterSlot &&
        m_textures[targetTexIndex].IsOccupied() &&
        m_textures[targetTexIndex].IsRenderTarget()) {
        targetSdlTex = m_textures[targetTexIndex].texture;
    }
    SDL_SetRenderTarget(m_renderer, targetSdlTex);
}

void Renderer::Submit(RenderList& list) noexcept {
    list.Compile();
    m_lastStats = list.Stats();
    if (!m_renderer) return;

    const ViewportExtent vpExtent = list.Viewport();
    constexpr Rect kFullNormalizedRect{0.0f, 0.0f, 1.0f, 1.0f};
    u8 pendingClears = list.ClearPassMask();
    u8 activePassId = 0xFFU;
    u8 activeViewSlot = 0xFFU;
    Rect activeViewport{-1.0f, -1.0f, -1.0f, -1.0f};
    Rect activeScissor{-1.0f, -1.0f, -1.0f, -1.0f};

    auto flushClearsUpTo = [&](u8 targetPassId) noexcept {
        const u8 upToMask = static_cast<u8>((2U << (targetPassId & 0x03U)) - 1U);
        u8 toClear = static_cast<u8>(pendingClears & upToMask);
        while (toClear != 0U) {
            const u8 p = static_cast<u8>(std::countr_zero(toClear));
            toClear &= static_cast<u8>(toClear - 1U);
            const RenderPassDesc& pDesc = list.Pass(p);
            BindRenderTarget(pDesc.targetTexIndex);
            SDL_SetRenderViewport(m_renderer, nullptr);
            SDL_SetRenderClipRect(m_renderer, nullptr);
            SDL_SetRenderDrawColorFloat(
                m_renderer, pDesc.clearColor.r, pDesc.clearColor.g, pDesc.clearColor.b, pDesc.clearColor.a);
            SDL_RenderClear(m_renderer);
            activeViewSlot = 0xFFU;
            activeViewport = kFullNormalizedRect;
            activeScissor = kFullNormalizedRect;
        }
        pendingClears &= static_cast<u8>(~upToMask);
    };

    for (const DrawBatch& batch : list.Batches()) {
        if (batch.passId != activePassId) {
            flushClearsUpTo(batch.passId);
            activePassId = batch.passId;
            BindRenderTarget(list.Pass(activePassId).targetTexIndex);
        }

        if (batch.viewSlot != activeViewSlot) {
            activeViewSlot = batch.viewSlot;
            const Camera2D& cam = list.View(activeViewSlot);

            if (cam.viewport != activeViewport) {
                activeViewport = cam.viewport;
                if (cam.viewport == kFullNormalizedRect) {
                    SDL_SetRenderViewport(m_renderer, nullptr);
                } else {
                    const SDL_Rect vpRect = ComputePixelViewport(vpExtent, cam.viewport);
                    SDL_SetRenderViewport(m_renderer, &vpRect);
                }
            }

            if (cam.scissor != activeScissor) {
                activeScissor = cam.scissor;
                if (cam.scissor.x <= 0.0f && cam.scissor.y <= 0.0f &&
                    cam.scissor.w >= 1.0f && cam.scissor.h >= 1.0f) {
                    SDL_SetRenderClipRect(m_renderer, nullptr);
                } else {
                    const SDL_Rect vpRect = ComputePixelViewport(vpExtent, cam.viewport);
                    const SDL_Rect clipRect{
                        static_cast<i32>(cam.scissor.x * static_cast<f32>(vpRect.w) + 0.5f),
                        static_cast<i32>(cam.scissor.y * static_cast<f32>(vpRect.h) + 0.5f),
                        std::max(1, static_cast<i32>(cam.scissor.w * static_cast<f32>(vpRect.w) + 0.5f)),
                        std::max(1, static_cast<i32>(cam.scissor.h * static_cast<f32>(vpRect.h) + 0.5f)),
                    };
                    SDL_SetRenderClipRect(m_renderer, &clipRect);
                }
            }
        }

        const u16 resolvedIdx = IsTextureValid(batch.texture) ? batch.texture.index : 0U;
        SDL_Texture* sdlTex = m_textures[resolvedIdx].texture;
        if (sdlTex) {
            SDL_SetTextureBlendMode(sdlTex, ToSdlBlendMode(batch.blend));
        }

        u32 remaining = batch.quadCount;
        u32 currentQuad = batch.firstQuad;
        while (remaining > 0U) {
            const u32 chunk = std::min(remaining, RenderList::kChunkQuads);
            list.ExpandVertices(currentQuad, chunk, m_vertices);
            const std::span<const u16> indices = RenderList::SharedQuadIndices(chunk);

            SDL_RenderGeometryRaw(
                m_renderer,
                sdlTex,
                &m_vertices[0].x,
                static_cast<int>(sizeof(Vertex2D)),
                reinterpret_cast<const SDL_FColor*>(&m_vertices[0].color),
                static_cast<int>(sizeof(Vertex2D)),
                &m_vertices[0].u,
                static_cast<int>(sizeof(Vertex2D)),
                static_cast<int>(chunk * 4U),
                indices.data(),
                static_cast<int>(indices.size()),
                static_cast<int>(sizeof(u16)));

            currentQuad += chunk;
            remaining -= chunk;
        }
    }

    if (pendingClears != 0U) {
        flushClearsUpTo(RenderList::kMaxPasses - 1U);
    }
    BindRenderTarget(0U);
    SDL_RenderPresent(m_renderer);
}

} // namespace en
