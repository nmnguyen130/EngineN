#include <cstdio>
#include "renderer/Renderer.h"
#include "platform/Window.h"
#include <SDL3/SDL.h>
#include <cstring>

namespace en {

namespace {

constexpr u8 kFallbackWhitePixel[4]{255U, 255U, 255U, 255U};

// Compact 5x7 bitmap font rows for ASCII 32..95 (' ' .. '_')
constexpr u8 kFont5x7[64][7] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, // '!'
    {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}, // '"'
    {0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0x00}, // '#'
    {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}, // '$'
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, // '%'
    {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}, // '&'
    {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}, // '\''
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}, // '('
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}, // ')'
    {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}, // '*'
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}, // '+'
    {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08}, // ','
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, // '-'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, // '.'
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}, // '/'
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // '0'
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // '1'
    {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}, // '2'
    {0x1F, 0x02, 0x04, 0x06, 0x01, 0x11, 0x0E}, // '3'
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // '4'
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // '5'
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // '6'
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // '7'
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // '8'
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // '9'
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, // ':'
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}, // ';'
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}, // '<'
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}, // '='
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}, // '>'
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}, // '?'
    {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E}, // '@'
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // 'A'
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // 'B'
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // 'C'
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, // 'D'
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // 'E'
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // 'F'
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, // 'G'
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // 'H'
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 'I'
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, // 'J'
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // 'K'
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // 'L'
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // 'M'
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // 'N'
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // 'O'
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // 'P'
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // 'Q'
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // 'R'
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, // 'S'
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // 'T'
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // 'U'
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // 'V'
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}, // 'W'
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // 'X'
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, // 'Y'
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, // 'Z'
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}, // '['
    {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01}, // '\\'
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}, // ']'
    {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00}, // '^'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}, // '_'
};

void BuildBuiltinFontAtlas(std::span<u8> outPixels) noexcept {
    std::memset(outPixels.data(), 0, outPixels.size());
    static constexpr const char* kFontBmpPaths[] = {
        "data/fonts/pixel_game_font.bmp",
        "../data/fonts/pixel_game_font.bmp",
        "../../data/fonts/pixel_game_font.bmp"
    };
    for (const char* path : kFontBmpPaths) {
        if (std::FILE* fp = std::fopen(path, "rb")) {
            u8 hdr[54]{};
            if (std::fread(hdr, 1U, 54U, fp) == 54U && hdr[0] == 'B' && hdr[1] == 'M') {
                const u32 offset = static_cast<u32>(hdr[10]) | (static_cast<u32>(hdr[11]) << 8U);
                if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) == 0 &&
                    std::fread(outPixels.data(), 1U, outPixels.size(), fp) == outPixels.size()) {
                    for (usize i = 0U; i < outPixels.size(); i += 4U) {
                        const u8 b = outPixels[i + 0U];
                        outPixels[i + 0U] = outPixels[i + 2U];
                        outPixels[i + 2U] = b;
                    }
                    std::fclose(fp);
                    return;
                }
            }
            std::fclose(fp);
        }
    }
    auto putPx = [&](u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) noexcept {
        if (x >= 128U || y >= 48U) return;
        const usize off = (static_cast<usize>(y) * 128U + x) * 4U;
        outPixels[off + 0U] = r;
        outPixels[off + 1U] = g;
        outPixels[off + 2U] = b;
        outPixels[off + 3U] = a;
    };
    for (u32 glyph = 0U; glyph < 96U; ++glyph) {
        const u32 ch = 32U + glyph;
        const u32 upperCh = (ch >= 'a' && ch <= 'z') ? (ch - 32U) : ch;
        const u32 tableIdx = (upperCh >= 32U && upperCh < 96U) ? (upperCh - 32U) : 0U;
        const u32 cellX = (glyph & 15U) * 8U;
        const u32 cellY = (glyph >> 4U) * 8U;
        for (u32 row = 0U; row < 7U; ++row) {
            const u8 bits = kFont5x7[tableIdx][row];
            for (u32 col = 0U; col < 5U; ++col) {
                if ((bits & (1U << (4U - col))) != 0U) {
                    putPx(cellX + col + 2U, cellY + row + 1U, 17U, 20U, 30U, 210U);
                }
            }
        }
        for (u32 row = 0U; row < 7U; ++row) {
            const u8 bits = kFont5x7[tableIdx][row];
            for (u32 col = 0U; col < 5U; ++col) {
                if ((bits & (1U << (4U - col))) != 0U) {
                    putPx(cellX + col + 1U, cellY + row, 255U, 255U, 255U, 255U);
                }
            }
        }
    }
}

void BuildBuiltinSoftLightTexture(std::span<u8> outPixels) noexcept {
    for (u32 y = 0U; y < 64U; ++y) {
        const f32 dy = (static_cast<f32>(y) + 0.5f - 32.0f) * (1.0f / 32.0f);
        for (u32 x = 0U; x < 64U; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5f - 32.0f) * (1.0f / 32.0f);
            const f32 t = std::max(0.0f, 1.0f - (dx * dx + dy * dy));
            const f32 falloff = t * t * t;
            const u8 a = static_cast<u8>(std::clamp(falloff * 255.0f + 0.5f, 0.0f, 255.0f));
            const usize off = (static_cast<usize>(y) * 64U + x) * 4U;
            outPixels[off + 0U] = a;
            outPixels[off + 1U] = a;
            outPixels[off + 2U] = a;
            outPixels[off + 3U] = a;
        }
    }
}

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

[[nodiscard]] SDL_Rect ComputePixelViewport(ViewportExtent windowPx, Rect normVp) noexcept {
    const f32 fw = static_cast<f32>(windowPx.w);
    const f32 fh = static_cast<f32>(windowPx.h);
    const i32 x = static_cast<i32>(normVp.x * fw + 0.5f);
    const i32 y = static_cast<i32>(normVp.y * fh + 0.5f);
    const i32 w = std::max(1, static_cast<i32>(normVp.w * fw + 0.5f));
    const i32 h = std::max(1, static_cast<i32>(normVp.h * fh + 0.5f));
    return SDL_Rect{x, y, w, h};
}

} // namespace

Renderer::Renderer(Renderer&& other) noexcept
    : m_renderer(std::exchange(other.m_renderer, nullptr)),
      m_viewportSize(other.m_viewportSize),
      m_lastStats(other.m_lastStats),
      m_blendModes(other.m_blendModes),
      m_highWaterSlot(std::exchange(other.m_highWaterSlot, 0U)),
      m_freeHead(std::exchange(other.m_freeHead, kInvalidSlot)),
      m_liveTextureCount(std::exchange(other.m_liveTextureCount, 0U)),
      m_activeRenderTarget(std::exchange(other.m_activeRenderTarget, 0U)),
      m_presentMode(other.m_presentMode),
      m_headless(other.m_headless),
      m_initialized(std::exchange(other.m_initialized, false)),
      m_vsyncActive(other.m_vsyncActive) {
    for (u16 i = 0U; i < m_highWaterSlot; ++i) {
        m_textures[i] = std::exchange(other.m_textures[i], TextureSlot{});
    }
    m_renderList.Reset(m_viewportSize);
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_renderer = std::exchange(other.m_renderer, nullptr);
        m_viewportSize = other.m_viewportSize;
        m_lastStats = other.m_lastStats;
        m_blendModes = other.m_blendModes;
        m_highWaterSlot = std::exchange(other.m_highWaterSlot, 0U);
        m_freeHead = std::exchange(other.m_freeHead, kInvalidSlot);
        m_liveTextureCount = std::exchange(other.m_liveTextureCount, 0U);
        m_activeRenderTarget = std::exchange(other.m_activeRenderTarget, 0U);
        m_presentMode = other.m_presentMode;
        m_headless = other.m_headless;
        m_initialized = std::exchange(other.m_initialized, false);
        m_vsyncActive = other.m_vsyncActive;
        for (u16 i = 0U; i < m_highWaterSlot; ++i) {
            m_textures[i] = std::exchange(other.m_textures[i], TextureSlot{});
        }
        m_renderList.Reset(m_viewportSize);
    }
    return *this;
}

Result<void, RendererErrc> Renderer::Init(Window& window, const RendererConfig& cfg) noexcept {
    Shutdown();

    if (window.State().pixelSize.IsValid()) {
        m_viewportSize = window.State().pixelSize;
    } else {
        m_viewportSize = {1280, 720};
    }

    m_headless = cfg.headless || (window.Native() == nullptr);
    m_presentMode = cfg.presentMode;
    m_freeHead = kInvalidSlot;
    m_highWaterSlot = 0U;
    m_liveTextureCount = 0U;
    m_activeRenderTarget = 0U;

    m_blendModes[static_cast<u8>(BlendMode::Alpha)] = SDL_BLENDMODE_BLEND_PREMULTIPLIED;
    m_blendModes[static_cast<u8>(BlendMode::Additive)] = SDL_BLENDMODE_ADD_PREMULTIPLIED;
    m_blendModes[static_cast<u8>(BlendMode::Multiply)] = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_DST_COLOR,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    m_blendModes[static_cast<u8>(BlendMode::Opaque)] = SDL_BLENDMODE_NONE;

    if (!m_headless) {
        m_renderer = SDL_CreateRenderer(window.Native(), nullptr);
        if (!m_renderer) {
            return Err(RendererErrc::DeviceInitFailed);
        }
        (void)SetPresentMode(cfg.presentMode);
    }

    // Slot 0: permanent 1x1 white fallback texture (kWhiteTexture = {0, 1})
    TextureDesc whiteDesc{};
    whiteDesc.width = 1U;
    whiteDesc.height = 1U;
    whiteDesc.pitch = 4U;
    whiteDesc.pixels = std::span<const u8>(kFallbackWhitePixel, 4U);
    whiteDesc.filter = TextureFilter::Nearest;
    const auto whiteRes = CreateTexture(whiteDesc);
    if (!whiteRes.has_value() || whiteRes.value() != kWhiteTexture) {
        Shutdown();
        return Err(RendererErrc::TextureCreateFailed);
    }

    // Slot 1: 512x192 TrueType-rasterized ASCII font atlas (kBuiltinFontTexture = {1, 1})
    static std::array<u8, 512U * 192U * 4U> s_fontPixels{};
    BuildBuiltinFontAtlas(s_fontPixels);
    TextureDesc fontDesc{};
    fontDesc.width = 512U;
    fontDesc.height = 192U;
    fontDesc.pitch = 512U * 4U;
    fontDesc.pixels = s_fontPixels;
    fontDesc.filter = TextureFilter::Linear;
    (void)CreateTexture(fontDesc);

    // Slot 2: built-in 64x64 radial soft light texture (kBuiltinSoftLightTexture = {2, 1})
    std::array<u8, 64U * 64U * 4U> lightPixels{};
    BuildBuiltinSoftLightTexture(lightPixels);
    TextureDesc lightDesc{};
    lightDesc.width = 64U;
    lightDesc.height = 64U;
    lightDesc.pitch = 64U * 4U;
    lightDesc.pixels = lightPixels;
    lightDesc.filter = TextureFilter::Linear;
    (void)CreateTexture(lightDesc);

    m_renderList.Reset(m_viewportSize);
    m_lastStats = RenderStats{};
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
    m_highWaterSlot = 0U;
    m_freeHead = kInvalidSlot;
    m_liveTextureCount = 0U;
    m_activeRenderTarget = 0U;
    m_headless = false;
    m_initialized = false;
    m_vsyncActive = false;
}

Result<void, RendererErrc> Renderer::SetPresentMode(PresentMode mode) noexcept {
    m_presentMode = mode;
    m_vsyncActive = (mode != PresentMode::Immediate);
    if (m_headless || !m_renderer) return {};
    int vsyncVal = 0;
    if (mode == PresentMode::VSync) {
        vsyncVal = 1;
    } else if (mode == PresentMode::AdaptiveVSync) {
        vsyncVal = SDL_RENDERER_VSYNC_ADAPTIVE;
    }
    if (!SDL_SetRenderVSync(m_renderer, vsyncVal) && mode == PresentMode::AdaptiveVSync) {
        if (!SDL_SetRenderVSync(m_renderer, 1)) {
            return Err(RendererErrc::DeviceInitFailed);
        }
    }
    return {};
}

Result<TextureHandle, RendererErrc> Renderer::AllocateTextureSlot(
    u16 slotIdx,
    const TextureDesc& desc) noexcept {
    const u32 rowBytes = desc.width * 4U;
    const u32 pitch = desc.pitch != 0U ? desc.pitch : rowBytes;
    if (pitch < rowBytes) {
        return Err(RendererErrc::InvalidDimension);
    }
    if (!desc.pixels.empty()) {
        const usize minBytes = static_cast<usize>(desc.height - 1U) * pitch + rowBytes;
        if (desc.pixels.size() < minBytes) {
            return Err(RendererErrc::TextureCreateFailed);
        }
    }

    SDL_Texture* sdlTex = nullptr;
    if (!m_headless && m_renderer) {
        const SDL_PixelFormat fmt =
            (desc.format == TextureFormat::BGRA8) ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32;
#if !defined(EN_SDL3_SHIM_ACTIVE)
        const SDL_TextureAccess access =
            desc.renderTarget ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STATIC;
#else
        const SDL_TextureAccess access = SDL_TEXTUREACCESS_STATIC;
#endif
        sdlTex = SDL_CreateTexture(
            m_renderer,
            fmt,
            access,
            static_cast<int>(desc.width),
            static_cast<int>(desc.height));
        if (!sdlTex) {
            return Err(RendererErrc::TextureCreateFailed);
        }
        if (!desc.pixels.empty()) {
            if (!SDL_UpdateTexture(sdlTex, nullptr, desc.pixels.data(), static_cast<int>(pitch))) {
                SDL_DestroyTexture(sdlTex);
                return Err(RendererErrc::TextureUpdateFailed);
            }
        }
        SDL_SetTextureScaleMode(sdlTex, ToSdlScaleMode(desc.filter));
        SDL_SetTextureBlendMode(sdlTex, m_blendModes[static_cast<u8>(BlendMode::Alpha)]);
    }

    TextureSlot& slot = m_textures[slotIdx];
    if (slot.generation == 0U) slot.generation = 1U;
    slot.texture = sdlTex;
    slot.width = static_cast<u16>(desc.width);
    slot.height = static_cast<u16>(desc.height);
    slot.filter = desc.filter;
    slot.flags = static_cast<u8>(kSlotOccupied | (desc.renderTarget ? kSlotRenderTarget : 0U));
    ++m_liveTextureCount;
    return TextureHandle{slotIdx, slot.generation};
}

Result<TextureHandle, RendererErrc> Renderer::CreateTexture(const TextureDesc& desc) noexcept {
    if (desc.width == 0U || desc.height == 0U || desc.width > 16384U || desc.height > 16384U) {
        return Err(RendererErrc::InvalidDimension);
    }

    u16 slotIdx = kInvalidSlot;
    if (m_freeHead != kInvalidSlot) {
        slotIdx = m_freeHead;
        m_freeHead = m_textures[slotIdx].width;
    } else if (m_highWaterSlot < kMaxTextures) {
        slotIdx = m_highWaterSlot++;
        m_textures[slotIdx] = TextureSlot{};
    } else {
        return Err(RendererErrc::PoolExhausted);
    }

    auto res = AllocateTextureSlot(slotIdx, desc);
    if (!res.has_value()) {
        m_textures[slotIdx].flags = 0U;
        m_textures[slotIdx].width = m_freeHead;
        m_freeHead = slotIdx;
    }
    return res;
}

Result<void, RendererErrc> Renderer::UpdateTexture(
    TextureHandle handle,
    Rect region,
    std::span<const u8> pixels,
    u32 pitch) noexcept {
    if (!IsTextureValid(handle)) {
        return Err(RendererErrc::InvalidHandle);
    }
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
    if (actualPitch < rowBytes) {
        return Err(RendererErrc::InvalidDimension);
    }
    const usize minBytes = static_cast<usize>(rh - 1) * actualPitch + rowBytes;
    if (pixels.size() < minBytes) {
        return Err(RendererErrc::TextureUpdateFailed);
    }

    if (!m_headless && m_renderer && slot.texture) {
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
    slot.height = 0U;
    ++slot.generation;
    if (slot.generation == 0U) slot.generation = 1U;
    slot.width = m_freeHead;
    m_freeHead = handle.index;
    if (m_liveTextureCount > 0U) --m_liveTextureCount;
}

void Renderer::BindRenderTarget(u16 targetTexIndex) noexcept {
    if (m_activeRenderTarget == targetTexIndex) return;
    m_activeRenderTarget = targetTexIndex;
#if !defined(EN_SDL3_SHIM_ACTIVE)
    SDL_Texture* targetSdlTex = nullptr;
    if (targetTexIndex > 0U && targetTexIndex < m_highWaterSlot &&
        m_textures[targetTexIndex].IsOccupied() &&
        m_textures[targetTexIndex].IsRenderTarget()) {
        targetSdlTex = m_textures[targetTexIndex].texture;
    }
    SDL_SetRenderTarget(m_renderer, targetSdlTex);
#endif
}

void Renderer::Submit(RenderList& list) noexcept {
    list.Compile();
    m_lastStats = list.Stats();
    if (m_headless || !m_renderer) return;

    const ViewportExtent vpExtent = list.Viewport();
    u16 pendingClears = list.ClearPassMask();
    u8 activePassId = 0xFFU;
    u8 activeViewSlot = 0xFFU;
    Rect activeViewport{-1.0f, -1.0f, -1.0f, -1.0f};
    Rect activeScissor{-1.0f, -1.0f, -1.0f, -1.0f};

    auto flushClearsUpTo = [&](u8 targetPassId) noexcept {
        const u16 upToMask = static_cast<u16>((2U << (targetPassId & 0x0FU)) - 1U);
        u16 toClear = static_cast<u16>(pendingClears & upToMask);
        while (toClear != 0U) {
            const u8 p = static_cast<u8>(std::countr_zero(toClear));
            toClear &= static_cast<u16>(toClear - 1U);
            const RenderPassDesc& pDesc = list.Pass(p);
            BindRenderTarget(pDesc.targetTexIndex);
            SDL_SetRenderViewport(m_renderer, nullptr);
            SDL_SetRenderClipRect(m_renderer, nullptr);
            SDL_SetRenderDrawColorFloat(
                m_renderer, pDesc.clearColor.r, pDesc.clearColor.g, pDesc.clearColor.b, pDesc.clearColor.a);
            SDL_RenderClear(m_renderer);
            activeViewSlot = 0xFFU;
            activeViewport = {-1.0f, -1.0f, -1.0f, -1.0f};
            activeScissor = {-1.0f, -1.0f, -1.0f, -1.0f};
        }
        pendingClears &= static_cast<u16>(~upToMask);
    };

    alignas(64) std::array<Vertex2D, RenderList::kChunkQuads * 4U> vertices{};

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
                if (cam.viewport.x == 0.0f && cam.viewport.y == 0.0f &&
                    cam.viewport.w == 1.0f && cam.viewport.h == 1.0f) {
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
            const u32 bm = m_blendModes[static_cast<u8>(batch.blend) & 3U];
            SDL_SetTextureBlendMode(sdlTex, bm);
        }

        u32 remaining = batch.quadCount;
        u32 currentQuad = batch.firstQuad;
        while (remaining > 0U) {
            const u32 chunk = std::min(remaining, RenderList::kChunkQuads);
            list.ExpandVertices(currentQuad, chunk, vertices);
            const std::span<const u16> indices = RenderList::SharedQuadIndices(chunk);

            SDL_RenderGeometryRaw(
                m_renderer,
                sdlTex,
                &vertices[0].x,
                static_cast<int>(sizeof(Vertex2D)),
                reinterpret_cast<const SDL_FColor*>(&vertices[0].color),
                static_cast<int>(sizeof(Vertex2D)),
                &vertices[0].u,
                static_cast<int>(sizeof(Vertex2D)),
                static_cast<int>(chunk * 4U),
                indices.data(),
                static_cast<int>(indices.size()),
                static_cast<int>(sizeof(u16)));

            currentQuad += chunk;
            remaining -= chunk;
        }
    }

    // Flush any remaining enabled clear passes (including empty passes) and restore default backbuffer
    if (pendingClears != 0U) {
        flushClearsUpTo(15U);
    }
    BindRenderTarget(0U);
    SDL_RenderPresent(m_renderer);
}

} // namespace en
