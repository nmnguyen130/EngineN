#include "renderer/Renderer2D.h"
#include <SDL3/SDL.h>

namespace en {

#include <cstdio>

namespace {
constexpr u8 kFallbackWhitePixel[4]{255, 255, 255, 255};

// Compact 5x7 bitmap font rows (5 bits per row, 7 rows per glyph) for ASCII 32..95 (' ' .. '_')
constexpr u8 kFont5x7[64][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, // '!'
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, // '"'
    {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}, // '#'
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, // '$'
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, // '%'
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, // '&'
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00}, // '\''
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // '('
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // ')'
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, // '*'
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, // ','
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // '.'
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, // '/'
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // '0'
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // '1'
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // '2'
    {0x1F,0x02,0x04,0x06,0x01,0x11,0x0E}, // '3'
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // '4'
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // '5'
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // '6'
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // '7'
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // '8'
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // '9'
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, // ':'
    {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, // ';'
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // '<'
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // '='
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // '>'
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // '?'
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, // '@'
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'A'
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // 'B'
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // 'C'
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // 'D'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // 'E'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // 'F'
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // 'G'
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'H'
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // 'I'
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // 'J'
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // 'K'
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // 'L'
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // 'M'
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // 'N'
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'O'
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // 'P'
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // 'Q'
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // 'R'
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // 'S'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // 'T'
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'U'
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // 'V'
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // 'W'
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // 'X'
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // 'Y'
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // 'Z'
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // '['
    {0x10,0x10,0x08,0x04,0x02,0x01,0x01}, // '\\'
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // ']'
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // '_'
};

void BuildBuiltinFontAtlas(std::span<u8> outPixels) noexcept {
    // 128x48 RGBA32 (16 cols x 6 rows of 8x8 cells for ASCII 32..127)
    std::memset(outPixels.data(), 0, outPixels.size());
    auto putPx = [&](u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) noexcept {
        if (x >= 128U || y >= 48U) return;
        const usize off = (static_cast<usize>(y) * 128U + x) * 4U;
        outPixels[off + 0] = r;
        outPixels[off + 1] = g;
        outPixels[off + 2] = b;
        outPixels[off + 3] = a;
    };
    for (u32 glyph = 0; glyph < 96U; ++glyph) {
        const u32 ch = 32U + glyph;
        const u32 upperCh = (ch >= 'a' && ch <= 'z') ? (ch - 32U) : ch;
        const u32 tableIdx = (upperCh >= 32U && upperCh < 96U) ? (upperCh - 32U) : 0U;
        const u32 cellX = (glyph & 15U) * 8U;
        const u32 cellY = (glyph >> 4U) * 8U;
        for (u32 row = 0; row < 7U; ++row) {
            const u8 bits = kFont5x7[tableIdx][row];
            for (u32 col = 0; col < 5U; ++col) {
                if ((bits & (1U << (4U - col))) != 0U) {
                    // Subtle dark drop shadow at (+1, +1) for readability
                    putPx(cellX + col + 2U, cellY + row + 1U, 20, 24, 36, 210);
                }
            }
        }
        for (u32 row = 0; row < 7U; ++row) {
            const u8 bits = kFont5x7[tableIdx][row];
            for (u32 col = 0; col < 5U; ++col) {
                if ((bits & (1U << (4U - col))) != 0U) {
                    putPx(cellX + col + 1U, cellY + row, 255, 255, 255, 255);
                }
            }
        }
    }
}

void BuildBuiltinSoftLightTexture(std::span<u8> outPixels) noexcept {
    // 64x64 RGBA32 smooth radial cubic falloff glow
    for (u32 y = 0; y < 64U; ++y) {
        const f32 dy = (static_cast<f32>(y) + 0.5f - 32.0f) / 32.0f;
        for (u32 x = 0; x < 64U; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5f - 32.0f) / 32.0f;
            const f32 d2 = dx * dx + dy * dy;
            const f32 t = std::max(0.0f, 1.0f - d2);
            const f32 falloff = t * t * t;
            const u8 a = static_cast<u8>( std::clamp(falloff * 255.0f + 0.5f, 0.0f, 255.0f));
            const usize off = (static_cast<usize>(y) * 64U + x) * 4U;
            outPixels[off + 0] = 255;
            outPixels[off + 1] = 255;
            outPixels[off + 2] = 255;
            outPixels[off + 3] = a;
        }
    }
}
} // namespace

Result<void, PlatformErrc> Renderer2D::Init(Window& window, const RendererConfig& cfg) noexcept {
    Shutdown();
    if (!m_renderList.Init() || !m_texArena.Init(4 * 1024 * 1024)) {
        return Err(PlatformErrc::InvalidState);
    }
    m_headless = cfg.headless || !window.IsValid();
    m_pixelsPerMeter = cfg.pixelsPerMeter > 1.0f ? cfg.pixelsPerMeter : 48.0f;

    SDL_Texture* whiteSdlTex = nullptr;
    if (!m_headless) {
        m_renderer = SDL_CreateRenderer(window.Native(), nullptr);
        if (!m_renderer) return Err(PlatformErrc::WindowCreateFailed);
        SDL_SetRenderVSync(m_renderer, cfg.vsync ? 1 : 0);
        whiteSdlTex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
        if (whiteSdlTex) {
            SDL_UpdateTexture(whiteSdlTex, nullptr, kFallbackWhitePixel, 4);
            SDL_SetTextureScaleMode(whiteSdlTex, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(whiteSdlTex, SDL_BLENDMODE_BLEND);
        }
    }

    m_textures[0] = GpuTexSlot{whiteSdlTex, kFallbackWhitePixel, 1, 1, 1, 0};
    m_highWaterTex = 1;
    m_highWaterFbo = 1;

    // Slot 1: kBuiltinFontTexture (128x48 RGBA32)
    std::array<u8, 128 * 48 * 4> fontBuf{};
    BuildBuiltinFontAtlas(fontBuf);
    (void)CreateTexture(128, 48, fontBuf);

    // Slot 2: kBuiltinSoftLightTexture (64x64 RGBA32)
    std::array<u8, 64 * 64 * 4> lightBuf{};
    BuildBuiltinSoftLightTexture(lightBuf);
    (void)CreateTexture(64, 64, lightBuf);

    m_swFramebuffer.fill(0xFF170F0DU);
    m_initialized = true;
    return {};
}

void Renderer2D::Shutdown() noexcept {
    if (m_renderer) {
        for (u16 i = 0; i < m_highWaterTex; ++i) {
            if (m_textures[i].texture) {
                SDL_DestroyTexture(m_textures[i].texture);
                m_textures[i].texture = nullptr;
            }
        }
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    m_texArena.Shutdown();
    m_renderList.Shutdown();
    m_highWaterTex = 1;
    m_highWaterFbo = 1;
    m_initialized = false;
}

GpuTextureHandle Renderer2D::CreateTexture(u16 width, u16 height, std::span<const u8> rgbaPixels) noexcept {
    if (width == 0 || height == 0 || m_highWaterTex >= kMaxGpuTextures) return kWhiteGpuTexture;
    const u16 idx = m_highWaterTex++;
    const usize byteCount = static_cast<usize>(width) * height * 4U;
    const u8* cpuCopy = kFallbackWhitePixel;
    if (rgbaPixels.size() >= byteCount) {
        std::span<u8> carved = m_texArena.Carve<u8>(byteCount);
        if (!carved.empty()) {
            std::memcpy(carved.data(), rgbaPixels.data(), byteCount);
            cpuCopy = carved.data();
        }
    }

    SDL_Texture* sdlTex = nullptr;
    if (m_renderer && !m_headless) {
        sdlTex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
        if (sdlTex && rgbaPixels.size() >= byteCount) {
            SDL_UpdateTexture(sdlTex, nullptr, rgbaPixels.data(), static_cast<int>(width) * 4);
            SDL_SetTextureScaleMode(sdlTex, SDL_SCALEMODE_NEAREST);
        }
    }
    m_textures[idx] = GpuTexSlot{sdlTex, cpuCopy, width, height, 1, 0};
    return GpuTextureHandle{idx, 1};
}

RenderTargetHandle Renderer2D::CreateRenderTarget(u16 width, u16 height) noexcept {
    if (width == 0 || height == 0 || m_highWaterFbo >= kMaxRenderTargets) return {};
    const usize bytes = static_cast<usize>(width) * height * 4U;
    std::span<u8> blank = m_texArena.Carve<u8>(bytes);
    const GpuTextureHandle tex = CreateTexture(width, height, blank);
    const u16 fboIdx = m_highWaterFbo++;
    m_renderTargets[fboIdx] = RenderTargetSlot{tex, width, height, 1, true};
    return RenderTargetHandle{fboIdx, 1};
}

GpuTextureHandle Renderer2D::GetRenderTargetTexture(RenderTargetHandle fbo) const noexcept {
    if (fbo.index > 0 && fbo.index < m_highWaterFbo &&
        m_renderTargets[fbo.index].generation == fbo.generation &&
        m_renderTargets[fbo.index].active) {
        return m_renderTargets[fbo.index].colorTex;
    }
    return kWhiteGpuTexture;
}

void Renderer2D::DestroyTexture(GpuTextureHandle handle) noexcept {
    if (!IsTextureValid(handle) || handle.index == 0) return;
    GpuTexSlot& slot = m_textures[handle.index];
    if (slot.texture && m_renderer) {
        SDL_DestroyTexture(slot.texture);
        slot.texture = nullptr;
    }
    ++slot.generation;
}

RenderList& Renderer2D::BeginFrame(ViewportExtent viewportSize) noexcept {
    m_renderList.Reset(viewportSize);
    return m_renderList;
}

void Renderer2D::RasterizeSoftwareFramebuffer(const RenderList& list) noexcept {
    const RenderPassDesc& pass0 = list.Pass(0);
    if (pass0.clear) {
        const u32 r = static_cast<u32>(std::clamp(pass0.clearColor.r, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 g = static_cast<u32>(std::clamp(pass0.clearColor.g, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 b = static_cast<u32>(std::clamp(pass0.clearColor.b, 0.0f, 1.0f) * 255.0f + 0.5f);
        const u32 packedClear = 0xFF000000U | (r << 16U) | (g << 8U) | b;
        m_swFramebuffer.fill(packedClear);
    }

    const ViewportExtent vp = list.Viewport();
    const f32 scaleX = static_cast<f32>(kSwFbWidth) / static_cast<f32>(std::max(1, vp.w));
    const f32 scaleY = static_cast<f32>(kSwFbHeight) / static_cast<f32>(std::max(1, vp.h));

    std::array<Vertex2D, 4> qv{};
    for (const DrawBatch& batch : list.Batches()) {
        const GpuTexSlot& texSlot = (batch.texture.index < m_highWaterTex)
                                        ? m_textures[batch.texture.index]
                                        : m_textures[0];
        const u8* texBytes = texSlot.cpuPixels ? texSlot.cpuPixels : kFallbackWhitePixel;
        const u16 tw = std::max<u16>(1U, texSlot.width);
        const u16 th = std::max<u16>(1U, texSlot.height);

        for (u32 q = 0; q < batch.quadCount; ++q) {
            list.ExpandVertices(batch.firstQuad + q, 1, qv);
            const f32 minXf = std::min({qv[0].x, qv[1].x, qv[2].x, qv[3].x}) * scaleX;
            const f32 maxXf = std::max({qv[0].x, qv[1].x, qv[2].x, qv[3].x}) * scaleX;
            const f32 minYf = std::min({qv[0].y, qv[1].y, qv[2].y, qv[3].y}) * scaleY;
            const f32 maxYf = std::max({qv[0].y, qv[1].y, qv[2].y, qv[3].y}) * scaleY;

            const i32 x0 = std::clamp(static_cast<i32>(minXf), 0, static_cast<i32>(kSwFbWidth) - 1);
            const i32 x1 = std::clamp(static_cast<i32>(maxXf + 0.99f), 0, static_cast<i32>(kSwFbWidth) - 1);
            const i32 y0 = std::clamp(static_cast<i32>(minYf), 0, static_cast<i32>(kSwFbHeight) - 1);
            const i32 y1 = std::clamp(static_cast<i32>(maxYf + 0.99f), 0, static_cast<i32>(kSwFbHeight) - 1);

            const f32 spanW = std::max(1.0f, maxXf - minXf);
            const f32 spanH = std::max(1.0f, maxYf - minYf);
            const Color vc = qv[0].color;

            for (i32 py = y0; py <= y1; ++py) {
                const f32 ty = std::clamp((static_cast<f32>(py) + 0.5f - minYf) / spanH, 0.0f, 1.0f);
                const f32 vCoord = qv[0].v + (qv[3].v - qv[0].v) * ty;
                const u32 texY = std::min<u32>(static_cast<u32>(std::clamp(vCoord, 0.0f, 0.9999f) * th), th - 1U);

                for (i32 px = x0; px <= x1; ++px) {
                    const f32 tx = std::clamp((static_cast<f32>(px) + 0.5f - minXf) / spanW, 0.0f, 1.0f);
                    const f32 uCoord = qv[0].u + (qv[1].u - qv[0].u) * tx;
                    const u32 texX = std::min<u32>(static_cast<u32>(std::clamp(uCoord, 0.0f, 0.9999f) * tw), tw - 1U);

                    const usize texOff = (static_cast<usize>(texY) * tw + texX) * 4U;
                    const f32 tr = static_cast<f32>(texBytes[texOff + 0]) * (1.0f / 255.0f);
                    const f32 tg = static_cast<f32>(texBytes[texOff + 1]) * (1.0f / 255.0f);
                    const f32 tb = static_cast<f32>(texBytes[texOff + 2]) * (1.0f / 255.0f);
                    const f32 ta = static_cast<f32>(texBytes[texOff + 3]) * (1.0f / 255.0f);

                    const f32 srcA = std::clamp(vc.a * ta, 0.0f, 1.0f);
                    if (srcA <= 0.01f) continue;

                    const f32 srcR = std::clamp(vc.r * tr, 0.0f, 1.0f);
                    const f32 srcG = std::clamp(vc.g * tg, 0.0f, 1.0f);
                    const f32 srcB = std::clamp(vc.b * tb, 0.0f, 1.0f);

                    u32& dstPacked = m_swFramebuffer[static_cast<usize>(py) * kSwFbWidth + static_cast<usize>(px)];
                    const f32 dstR = static_cast<f32>((dstPacked >> 16U) & 0xFFU) * (1.0f / 255.0f);
                    const f32 dstG = static_cast<f32>((dstPacked >> 8U) & 0xFFU) * (1.0f / 255.0f);
                    const f32 dstB = static_cast<f32>(dstPacked & 0xFFU) * (1.0f / 255.0f);

                    f32 outR = srcR;
                    f32 outG = srcG;
                    f32 outB = srcB;
                    if (batch.blend == BlendMode::Additive) {
                        outR = std::min(1.0f, dstR + srcR * srcA);
                        outG = std::min(1.0f, dstG + srcG * srcA);
                        outB = std::min(1.0f, dstB + srcB * srcA);
                    } else if (batch.blend == BlendMode::Alpha) {
                        outR = dstR * (1.0f - srcA) + srcR * srcA;
                        outG = dstG * (1.0f - srcA) + srcG * srcA;
                        outB = dstB * (1.0f - srcA) + srcB * srcA;
                    }
                    const u32 r8 = static_cast<u32>(outR * 255.0f + 0.5f);
                    const u32 g8 = static_cast<u32>(outG * 255.0f + 0.5f);
                    const u32 b8 = static_cast<u32>(outB * 255.0f + 0.5f);
                    dstPacked = 0xFF000000U | (r8 << 16U) | (g8 << 8U) | b8;
                }
            }
        }
    }
}

u32 Renderer2D::CountNonBlackPixels() const noexcept {
    u32 count = 0;
    for (u32 px : m_swFramebuffer) {
        if ((px & 0x00FFFFFFU) != 0U && px != 0xFF170F0DU) {
            ++count;
        }
    }
    return count;
}

bool Renderer2D::SaveFramebufferBmp(const char* filePath) const noexcept {
    if (!filePath) return false;
    std::FILE* fp = std::fopen(filePath, "wb");
    if (!fp) return false;
    const u32 rowBytes = kSwFbWidth * 3U;
    const u32 dataSize = rowBytes * kSwFbHeight;
    const u32 fileSize = 54U + dataSize;
    u8 header[54]{};
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header + 2, &fileSize, 4);
    const u32 dataOffset = 54U;
    std::memcpy(header + 10, &dataOffset, 4);
    const u32 dibSize = 40U;
    std::memcpy(header + 14, &dibSize, 4);
    const i32 w = kSwFbWidth;
    const i32 h = kSwFbHeight;
    std::memcpy(header + 18, &w, 4);
    std::memcpy(header + 22, &h, 4);
    const u16 planes = 1;
    const u16 bpp = 24;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &dataSize, 4);
    (void)std::fwrite(header, 1, 54, fp);

    std::array<u8, kSwFbWidth * 3> row{};
    for (i32 y = static_cast<i32>(kSwFbHeight) - 1; y >= 0; --y) {
        for (u16 x = 0; x < kSwFbWidth; ++x) {
            const u32 p = m_swFramebuffer[static_cast<usize>(y) * kSwFbWidth + x];
            row[x * 3U + 0] = static_cast<u8>(p & 0xFFU);
            row[x * 3U + 1] = static_cast<u8>((p >> 8U) & 0xFFU);
            row[x * 3U + 2] = static_cast<u8>((p >> 16U) & 0xFFU);
        }
        (void)std::fwrite(row.data(), 1, row.size(), fp);
    }
    std::fclose(fp);
    return true;
}

void Renderer2D::Submit(RenderList& list) noexcept {
    list.Compile();
    RasterizeSoftwareFramebuffer(list);
    if (m_headless || !m_renderer) return;

    const RenderPassDesc& pass0 = list.Pass(0);
    if (pass0.clear) {
        SDL_SetRenderDrawColorFloat(
            m_renderer, pass0.clearColor.r, pass0.clearColor.g, pass0.clearColor.b, pass0.clearColor.a);
        SDL_RenderClear(m_renderer);
    }

    static constexpr u32 kChunkQuads = 512;
    std::array<Vertex2D, kChunkQuads * 4> verts{};
    std::array<int, kChunkQuads * 6> indices{};
    for (u32 q = 0; q < kChunkQuads; ++q) {
        const int b = static_cast<int>(q * 4U);
        indices[q * 6U + 0] = b + 0;
        indices[q * 6U + 1] = b + 1;
        indices[q * 6U + 2] = b + 2;
        indices[q * 6U + 3] = b + 0;
        indices[q * 6U + 4] = b + 2;
        indices[q * 6U + 5] = b + 3;
    }

    for (const DrawBatch& batch : list.Batches()) {
        SDL_Texture* sdlTex = (batch.texture.index < m_highWaterTex)
                                  ? m_textures[batch.texture.index].texture
                                  : m_textures[0].texture;
        if (sdlTex) {
            SDL_BlendMode bm = SDL_BLENDMODE_BLEND;
            if (batch.blend == BlendMode::Additive) bm = SDL_BLENDMODE_ADD;
            else if (batch.blend == BlendMode::Opaque) bm = SDL_BLENDMODE_NONE;
            SDL_SetTextureBlendMode(sdlTex, bm);
        }
        u32 rem = batch.quadCount;
        u32 cur = batch.firstQuad;
        while (rem > 0) {
            const u32 chunk = std::min(rem, kChunkQuads);
            list.ExpandVertices(cur, chunk, verts);
            SDL_RenderGeometryRaw(
                m_renderer,
                sdlTex,
                &verts[0].x, sizeof(Vertex2D),
                reinterpret_cast<const SDL_FColor*>(&verts[0].color), sizeof(Vertex2D),
                &verts[0].u, sizeof(Vertex2D),
                static_cast<int>(chunk * 4U),
                indices.data(), static_cast<int>(chunk * 6U), sizeof(int));
            cur += chunk;
            rem -= chunk;
        }
    }
#ifdef EN_SDL3_SHIM_ACTIVE
    SDL_PresentSoftwareFramebuffer(m_renderer, m_swFramebuffer.data(), kSwFbWidth, kSwFbHeight);
#endif
    SDL_RenderPresent(m_renderer);
}

void Renderer2D::ExtractWorld(
    const World& world, const AssetBank& assetBank, const ParticlePool2D& particles,
    const RenderTimeView& timeView, RenderList& outList, f32 pixelsPerMeter) noexcept {
    // 1. Extract active CameraRig2D views.
    bool foundCamera = false;
    world.Each<CameraRig2D>([&](EntityId id, const CameraRig2D& rig) noexcept {
        if (!rig.active) return;
        const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
        const PrevWorldSnapshot2D& prev = world.GetPrevWorldByIndex(id.index);
        const f32 a = timeView.Alpha(TimeChannel::Camera);
        Camera2D cam{};
        cam.viewport = rig.viewport;
        cam.worldCenterX = prev.x + (wt.x - prev.x) * a;
        cam.worldCenterY = prev.y + (wt.y - prev.y) * a;
        cam.zoom = rig.zoom;
        cam.pixelsPerMeter = pixelsPerMeter;
        cam.screenSpace = rig.screenSpace;
        outList.SetView(rig.viewSlot, cam);
        foundCamera = true;
    });
    if (!foundCamera) {
        Camera2D defaultCam{};
        defaultCam.pixelsPerMeter = pixelsPerMeter;
        outList.SetView(0, defaultCam);
    }

    // 2. Extract interpolated world Sprite2D entities (stationary-optimized via MovedWords).
    const std::span<const u64> movedWords = world.MovedWords();
    world.Each<Sprite2D>([&](EntityId id, const Sprite2D& sp) noexcept {
        if (!sp.Visible()) return;
        const u16 idx = id.index;
        const WorldTransform2D& wt = world.GetWorldByIndex(idx);
        f32 rx = wt.x;
        f32 ry = wt.y;
        f32 sx = wt.scaleX;
        f32 sy = wt.scaleY;
        f32 c = wt.cosRot;
        f32 s = wt.sinRot;

        const bool moved = (movedWords[idx >> 6U] & (1ULL << (idx & 63U))) != 0ULL;
        if (moved) {
            const PrevWorldSnapshot2D& pv = world.GetPrevWorldByIndex(idx);
            const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(idx));
            const f32 alpha = timeView.Alpha(ch);
            rx = pv.x + (wt.x - pv.x) * alpha;
            ry = pv.y + (wt.y - pv.y) * alpha;
            sx = pv.scaleX + (wt.scaleX - pv.scaleX) * alpha;
            sy = pv.scaleY + (wt.scaleY - pv.scaleY) * alpha;
            const f32 rot = pv.worldRotRad + (wt.worldRotRad - pv.worldRotRad) * alpha;
            FastSinCos(rot, c, s);
        }

        SpriteFrame frame = assetBank.GetSprite(sp.sprite);
        if (const SpriteAnimator2D* anim = world.TryGet<SpriteAnimator2D>(id)) {
            if (anim->atlas.IsValid()) {
                frame = assetBank.GetAtlasFrame(
                    anim->atlas, static_cast<u16>(anim->firstFrame + anim->currentFrame));
            }
        }

        QuadCmd cmd{};
        cmd.center = {rx, ry};
        cmd.size = {sp.width * sx, sp.height * sy};
        cmd.cosRot = c;
        cmd.sinRot = s;
        cmd.uv = frame.uv;
        cmd.color = sp.tint;
        cmd.texture = frame.gpuTexture;
        cmd.depth = sp.baseDepth;
        cmd.layer = sp.layer;
        cmd.flipX = sp.FlipX();
        cmd.flipY = sp.FlipY();
        outList.PushQuad(cmd);
    });

    // 3. Extract SOA particles.
    const u32 pCount = particles.ActiveCount();
    const auto px = particles.PosX();
    const auto py = particles.PosY();
    const auto ps = particles.Size();
    const auto pc = particles.Colors();
    for (u32 i = 0; i < pCount; ++i) {
        QuadCmd cmd{};
        cmd.center = {px[i], py[i]};
        cmd.size = {ps[i], ps[i]};
        cmd.color = pc[i];
        cmd.layer = 18;
        cmd.blend = BlendMode::Additive;
        outList.PushQuad(cmd);
    }

    // 4. Extract dynamic PointLight2D halos.
    world.Each<PointLight2D>([&](EntityId id, const PointLight2D& light) noexcept {
        const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
        f32 pulse = 1.0f;
        if (light.pulseSpeed != 0.0f) {
            f32 c = 1.0f, s = 0.0f;
            FastSinCos(light.pulsePhase, c, s);
            pulse = 0.85f + 0.15f * s;
        }
        LightingAndPostFx::EmitPointLightHalo(
            outList, {wt.x, wt.y}, light.radius * pulse, light.color, light.intensity * pulse);
    });
}

} // namespace en
