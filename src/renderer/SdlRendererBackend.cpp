#include "renderer/SdlRendererBackend.h"
#include "renderer/RenderDevice.h"
#include "platform/Window.h"
#include "core/Log.h"

#include <SDL3/SDL.h>
#include <cstring>
#include <algorithm>

namespace EngineN {

namespace {

SDL_BlendMode ToSdlBlendMode(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Opaque:        return SDL_BLENDMODE_NONE;
        case BlendMode::Alpha:         return SDL_BLENDMODE_BLEND;
        case BlendMode::Premultiplied: return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
        case BlendMode::Additive:      return SDL_BLENDMODE_ADD_PREMULTIPLIED;
        case BlendMode::Multiply:      return SDL_BLENDMODE_MUL;
    }
    return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
}

SDL_RendererLogicalPresentation ToSdlPresentation(PresentationMode mode) noexcept {
    switch (mode) {
        case PresentationMode::Disabled:     return SDL_LOGICAL_PRESENTATION_DISABLED;
        case PresentationMode::Stretch:      return SDL_LOGICAL_PRESENTATION_STRETCH;
        case PresentationMode::Letterbox:    return SDL_LOGICAL_PRESENTATION_LETTERBOX;
        case PresentationMode::Overscan:     return SDL_LOGICAL_PRESENTATION_OVERSCAN;
        case PresentationMode::IntegerScale: return SDL_LOGICAL_PRESENTATION_INTEGER_SCALE;
    }
    return SDL_LOGICAL_PRESENTATION_LETTERBOX;
}

} // namespace

SdlRendererBackend::SdlRendererBackend() = default;

SdlRendererBackend::~SdlRendererBackend() {
    Shutdown();
}

Result<void> SdlRendererBackend::Init(Window* window, const RenderDeviceConfig& cfg) {
    Shutdown();
    m_cfg = cfg;

    if (!window || !window->IsValid()) {
        return Unexpected(MakeRenderError(
            RenderErrc::DeviceCreationFailed,
            "Cannot create SdlRendererBackend without a valid Window"));
    }

    m_renderer = SDL_CreateRenderer(window->Native(), nullptr);
    if (!m_renderer) {
        ENGINE_LOG(Error, Render, "SDL_CreateRenderer failed: {}", SDL_GetError());
        return Unexpected(MakeRenderError(
            RenderErrc::DeviceCreationFailed,
            SDL_GetError()));
    }

    m_textures.resize(std::max(16u, m_cfg.maxTextures), nullptr);

    auto vsyncRes = SetVSync(m_cfg.vsync);
    if (!vsyncRes) {
        ENGINE_LOG(Warn, Render, "Failed to apply VSync mode, falling back to standard");
    }

    auto presRes = SetLogicalPresentation(m_cfg.virtualWidth, m_cfg.virtualHeight, m_cfg.presentation);
    if (!presRes) {
        ENGINE_LOG(Warn, Render, "Failed to apply logical presentation");
    }

    const char* driverName = SDL_GetRendererName(m_renderer);
    m_caps = BackendCaps{
        .backendName = driverName ? driverName : "SDL_Renderer",
        .maxTextureDimension = 4096,
        .vsyncAdaptive = true,
        .supportsCustomShaders = false
    };

    ENGINE_LOG(Info, Render, "SdlRendererBackend initialized successfully (Driver: {})", m_caps.backendName);
    return Ok;
}

void SdlRendererBackend::Shutdown() {
    for (usize i = 0; i < m_textures.size(); ++i) {
        if (m_textures[i]) {
            SDL_DestroyTexture(m_textures[i]);
            m_textures[i] = nullptr;
        }
    }
    m_textures.clear();

    if (m_renderer) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
}

Result<void> SdlRendererBackend::CreateTexture(
    TextureHandle handle,
    const TextureConfig& cfg,
    const void* initialPixels)
{
    if (!m_renderer) {
        return Unexpected(MakeRenderError(RenderErrc::DeviceCreationFailed, "Backend not initialized"));
    }

    if (handle.index >= m_textures.size()) {
        m_textures.resize(handle.index + 16, nullptr);
    }

    SDL_Texture* tex = SDL_CreateTexture(
        m_renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC,
        static_cast<int>(cfg.width),
        static_cast<int>(cfg.height)
    );

    if (!tex) {
        ENGINE_LOG(Error, Render, "SDL_CreateTexture failed: {}", SDL_GetError());
        return Unexpected(MakeRenderError(RenderErrc::TextureCreationFailed, SDL_GetError()));
    }

    const SDL_ScaleMode scaleMode = (cfg.filter == TextureFilter::Nearest)
        ? SDL_SCALEMODE_NEAREST
        : SDL_SCALEMODE_LINEAR;
    SDL_SetTextureScaleMode(tex, scaleMode);

    if (initialPixels) {
        if (cfg.premultiplyAlpha) {
            const u8* src = reinterpret_cast<const u8*>(initialPixels);
            const usize totalPixels = static_cast<usize>(cfg.width) * cfg.height;
            std::vector<u8> staging(totalPixels * 4);

            for (usize i = 0; i < totalPixels; ++i) {
                const u8 a = src[i * 4 + 3];
                staging[i * 4 + 0] = static_cast<u8>((src[i * 4 + 0] * a + 127) / 255);
                staging[i * 4 + 1] = static_cast<u8>((src[i * 4 + 1] * a + 127) / 255);
                staging[i * 4 + 2] = static_cast<u8>((src[i * 4 + 2] * a + 127) / 255);
                staging[i * 4 + 3] = a;
            }

            SDL_UpdateTexture(tex, nullptr, staging.data(), static_cast<int>(cfg.width * 4));
        } else {
            SDL_UpdateTexture(tex, nullptr, initialPixels, static_cast<int>(cfg.width * 4));
        }
    }

    m_textures[handle.index] = tex;
    return Ok;
}

Result<void> SdlRendererBackend::UpdateTexture(
    TextureHandle handle,
    const Rect& region,
    const void* pixels)
{
    if (handle.index >= m_textures.size() || !m_textures[handle.index]) {
        return Unexpected(MakeRenderError(RenderErrc::TextureNotFound, "Invalid texture handle"));
    }

    const SDL_Rect r{
        region.x, region.y, region.w, region.h
    };

    if (!SDL_UpdateTexture(m_textures[handle.index], &r, pixels, region.w * 4)) {
        return Unexpected(MakeRenderError(RenderErrc::BackendError, SDL_GetError()));
    }

    return Ok;
}

void SdlRendererBackend::DestroyTexture(TextureHandle handle) {
    if (handle.index < m_textures.size() && m_textures[handle.index]) {
        SDL_DestroyTexture(m_textures[handle.index]);
        m_textures[handle.index] = nullptr;
    }
}

Result<void> SdlRendererBackend::SetVSync(VSyncMode mode) {
    m_cfg.vsync = mode;
    if (!m_renderer) return Ok;

    int vsyncVal = 1;
    if (mode == VSyncMode::Off) {
        vsyncVal = 0;
    } else if (mode == VSyncMode::Adaptive) {
        vsyncVal = SDL_RENDERER_VSYNC_ADAPTIVE;
    }

    if (!SDL_SetRenderVSync(m_renderer, vsyncVal)) {
        // Fallback to standard vsync on failure
        SDL_SetRenderVSync(m_renderer, 1);
    }
    return Ok;
}

Result<void> SdlRendererBackend::SetLogicalPresentation(
    i32 w, i32 h, PresentationMode mode)
{
    m_cfg.virtualWidth = w;
    m_cfg.virtualHeight = h;
    m_cfg.presentation = mode;

    if (!m_renderer) return Ok;

    if (!SDL_SetRenderLogicalPresentation(m_renderer, w, h, ToSdlPresentation(mode))) {
        return Unexpected(MakeRenderError(RenderErrc::BackendError, SDL_GetError()));
    }

    return Ok;
}

Result<void> SdlRendererBackend::BeginFrame(Color clearColor) {
    if (!m_renderer) {
        return Unexpected(MakeRenderError(RenderErrc::DeviceCreationFailed, "Backend not initialized"));
    }

    SDL_SetRenderDrawColorFloat(m_renderer, clearColor.r, clearColor.g, clearColor.b, clearColor.a);
    SDL_RenderClear(m_renderer);
    return Ok;
}

Result<void> SdlRendererBackend::Submit(const BatchView& batch) {
    if (!m_renderer) {
        return Unexpected(MakeRenderError(RenderErrc::DeviceCreationFailed, "Backend not initialized"));
    }

    const usize vCount = batch.vertices.size();
    if (vCount == 0 || batch.runs.empty()) {
        return Ok;
    }

    if (m_colorScratch.size() < vCount) {
        m_colorScratch.resize(vCount);
    }

    // Flat SIMD-friendly loop converting 32-bit packed RGBA to SDL_FColor
    constexpr f32 inv255 = 1.0f / 255.0f;
    for (usize i = 0; i < vCount; ++i) {
        const u32 c = batch.vertices[i].rgba;
        m_colorScratch[i] = SDL_FColor{
            static_cast<f32>(c & 0xFF) * inv255,
            static_cast<f32>((c >> 8) & 0xFF) * inv255,
            static_cast<f32>((c >> 16) & 0xFF) * inv255,
            static_cast<f32>((c >> 24) & 0xFF) * inv255
        };
    }

    u16 activeScissorId = 0xFFFF;

    for (const auto& run : batch.runs) {
        if (run.indexCount == 0) continue;

        SDL_Texture* tex = nullptr;
        if (run.textureSlot > 0 && run.textureSlot < m_textures.size()) {
            tex = m_textures[run.textureSlot];
        }

        const SDL_BlendMode sdlBlend = ToSdlBlendMode(run.blend);
        if (tex) {
            SDL_SetTextureBlendMode(tex, sdlBlend);
        } else {
            SDL_SetRenderDrawBlendMode(m_renderer, sdlBlend);
        }

        // Scissor state switching
        if (run.scissorId != activeScissorId) {
            activeScissorId = run.scissorId;
            if (run.scissorId == 0 || run.scissorId >= batch.scissors.size()) {
                SDL_SetRenderClipRect(m_renderer, nullptr);
            } else {
                const Rect& sr = batch.scissors[run.scissorId];
                const SDL_Rect clipRect{sr.x, sr.y, sr.w, sr.h};
                SDL_SetRenderClipRect(m_renderer, &clipRect);
            }
        }

        SDL_RenderGeometryRaw(
            m_renderer,
            tex,
            &batch.vertices[0].x, sizeof(Vertex),
            &m_colorScratch[0], sizeof(SDL_FColor),
            &batch.vertices[0].u, sizeof(Vertex),
            static_cast<int>(vCount),
            &batch.indices[run.firstIndex], static_cast<int>(run.indexCount), sizeof(u32)
        );
    }

    if (activeScissorId != 0) {
        SDL_SetRenderClipRect(m_renderer, nullptr);
    }

    return Ok;
}

Result<void> SdlRendererBackend::Present() {
    if (!m_renderer) {
        return Unexpected(MakeRenderError(RenderErrc::DeviceCreationFailed, "Backend not initialized"));
    }
    SDL_RenderPresent(m_renderer);
    return Ok;
}

const BackendCaps& SdlRendererBackend::GetCaps() const noexcept {
    return m_caps;
}

Vec2 SdlRendererBackend::WindowToVirtual(Vec2 windowPx) const noexcept {
    if (!m_renderer) return windowPx;
    float vx = 0.0f;
    float vy = 0.0f;
    SDL_RenderCoordinatesFromWindow(m_renderer, windowPx.x, windowPx.y, &vx, &vy);
    return {vx, vy};
}

Vec2 SdlRendererBackend::VirtualToWindow(Vec2 virtualPx) const noexcept {
    if (!m_renderer) return virtualPx;
    float wx = 0.0f;
    float wy = 0.0f;
    SDL_RenderCoordinatesToWindow(m_renderer, virtualPx.x, virtualPx.y, &wx, &wy);
    return {wx, wy};
}

FRect SdlRendererBackend::GetPresentationRect() const noexcept {
    if (!m_renderer) {
        return FRect{0.0f, 0.0f, static_cast<f32>(m_cfg.virtualWidth), static_cast<f32>(m_cfg.virtualHeight)};
    }
    SDL_FRect r{0.0f, 0.0f, 0.0f, 0.0f};
    SDL_GetRenderLogicalPresentationRect(m_renderer, &r);
    return FRect{r.x, r.y, r.w, r.h};
}

} // namespace EngineN
