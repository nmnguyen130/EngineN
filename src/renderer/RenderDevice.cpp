#include "renderer/RenderDevice.h"
#include "renderer/FrameArena.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace EngineN {

namespace {

static SDL_BlendMode ToSdlBlend(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Opaque:
            return SDL_BLENDMODE_NONE;
        case BlendMode::Multiply:
            return SDL_BLENDMODE_MUL;
        case BlendMode::Additive:
        case BlendMode::Premultiplied:
        default:
            // Premultiplied alpha handles straight alpha and additive (alpha == 0) uniformly
            return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
    }
}

} // namespace

RenderDevice::~RenderDevice() {
    Shutdown();
}

RenderDevice::RenderDevice(RenderDevice&& other) noexcept
    : m_renderer(other.m_renderer),
      m_vertexScratch(std::move(other.m_vertexScratch)),
      m_indexScratch(std::move(other.m_indexScratch)),
      m_freeTextureSlots(std::move(other.m_freeTextureSlots)) {
    other.m_renderer = nullptr;
    std::memcpy(m_textureSlots, other.m_textureSlots, sizeof(m_textureSlots));
    std::memcpy(m_textureGens, other.m_textureGens, sizeof(m_textureGens));
    std::memset(other.m_textureSlots, 0, sizeof(other.m_textureSlots));
    std::memset(other.m_textureGens, 0, sizeof(other.m_textureGens));
}

RenderDevice& RenderDevice::operator=(RenderDevice&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_renderer = other.m_renderer;
        m_vertexScratch = std::move(other.m_vertexScratch);
        m_indexScratch = std::move(other.m_indexScratch);
        m_freeTextureSlots = std::move(other.m_freeTextureSlots);
        std::memcpy(m_textureSlots, other.m_textureSlots, sizeof(m_textureSlots));
        std::memcpy(m_textureGens, other.m_textureGens, sizeof(m_textureGens));

        other.m_renderer = nullptr;
        std::memset(other.m_textureSlots, 0, sizeof(other.m_textureSlots));
        std::memset(other.m_textureGens, 0, sizeof(other.m_textureGens));
    }
    return *this;
}

Result<void, EngineError> RenderDevice::Init(Window& window, const RenderDeviceConfig& config) noexcept {
    if (!window.IsValid()) {
        return Err(EngineError{"Cannot initialize RenderDevice with invalid Window"});
    }

    m_renderer = SDL_CreateRenderer(window.Native(), nullptr);
    if (!m_renderer) {
        return Err(EngineError{SDL_GetError()});
    }

    if (config.vsync) {
        (void)SetVSync(true);
    }

    // Pre-allocate scratch geometry to arena capacity to guarantee 0 heap allocations
    m_vertexScratch.resize(FrameArenaSlot::kMaxInstances * 4);
    m_indexScratch.resize(FrameArenaSlot::kMaxInstances * 6);

    // Initialize generational texture pool (slot 0 reserved for untextured geometry)
    m_freeTextureSlots.reserve(kMaxTextureSlots);
    for (usize i = kMaxTextureSlots - 1; i >= 1; --i) {
        m_freeTextureSlots.push_back(static_cast<u16>(i));
        m_textureGens[i] = 1;
    }

    // Create 1x1 white texture in slot 1 for fallback drawing
    m_freeTextureSlots.pop_back(); // slot 1
    m_textureSlots[1] = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
    if (m_textureSlots[1]) {
        SDL_SetTextureBlendMode(m_textureSlots[1], SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        const u32 whitePixel = 0xFFFFFFFF;
        SDL_UpdateTexture(m_textureSlots[1], nullptr, &whitePixel, sizeof(u32));
    }

    return {};
}

void RenderDevice::Shutdown() noexcept {
    if (!m_renderer) {
        return;
    }

    for (usize i = 0; i < kMaxTextureSlots; ++i) {
        if (m_textureSlots[i]) {
            SDL_DestroyTexture(m_textureSlots[i]);
            m_textureSlots[i] = nullptr;
        }
    }

    SDL_DestroyRenderer(m_renderer);
    m_renderer = nullptr;
    m_freeTextureSlots.clear();
}

Result<TextureHandle, EngineError> RenderDevice::Create(const TextureDesc& desc) noexcept {
    if (!m_renderer) {
        return Err(EngineError{"RenderDevice is not initialized"});
    }
    if (m_freeTextureSlots.empty()) {
        return Err(EngineError{"RenderDevice texture slot capacity exhausted"});
    }

    const u16 slot = m_freeTextureSlots.back();
    m_freeTextureSlots.pop_back();

    const SDL_PixelFormat format = (desc.format == TextureFormat::BGRA8)
                                       ? SDL_PIXELFORMAT_BGRA32
                                       : SDL_PIXELFORMAT_RGBA32;
    const SDL_TextureAccess access = desc.isStatic
                                         ? SDL_TEXTUREACCESS_STATIC
                                         : SDL_TEXTUREACCESS_STREAMING;

    SDL_Texture* tex = SDL_CreateTexture(
        m_renderer,
        format,
        access,
        static_cast<int>(desc.width),
        static_cast<int>(desc.height)
    );
    if (!tex) {
        m_freeTextureSlots.push_back(slot);
        return Err(EngineError{SDL_GetError()});
    }

    // Premultiplied alpha by contract prevents color fringing on transparent edges
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    m_textureSlots[slot] = tex;
    return TextureHandle{slot, m_textureGens[slot]};
}

Result<void, EngineError> RenderDevice::Upload(TextureHandle handle,
                                               const URect& rect,
                                               const void* pixels,
                                               u32 pitch) noexcept {
    const u16 slot = handle.slot;
    if (slot == 0 || slot >= kMaxTextureSlots || m_textureGens[slot] != handle.generation || !m_textureSlots[slot]) {
        return Err(EngineError{"Invalid or stale TextureHandle for upload"});
    }

    const SDL_Rect updateRect{
        static_cast<int>(rect.x),
        static_cast<int>(rect.y),
        static_cast<int>(rect.w),
        static_cast<int>(rect.h)
    };

    if (!SDL_UpdateTexture(m_textureSlots[slot], &updateRect, pixels, static_cast<int>(pitch))) {
        return Err(EngineError{SDL_GetError()});
    }

    return {};
}

void RenderDevice::Destroy(TextureHandle handle) noexcept {
    const u16 slot = handle.slot;
    if (slot == 0 || slot >= kMaxTextureSlots || m_textureGens[slot] != handle.generation || !m_textureSlots[slot]) {
        return;
    }

    SDL_DestroyTexture(m_textureSlots[slot]);
    m_textureSlots[slot] = nullptr;

    if (m_textureGens[slot] < 65535) {
        m_textureGens[slot]++;
        m_freeTextureSlots.push_back(slot);
    } else {
        m_textureGens[slot] = 0;
    }
}

Result<void, EngineError> RenderDevice::SetVSync(bool enabled) noexcept {
    if (!m_renderer) {
        return Err(EngineError{"RenderDevice is not initialized"});
    }
    if (!SDL_SetRenderVSync(m_renderer, enabled ? 1 : 0)) {
        return Err(EngineError{SDL_GetError()});
    }
    return {};
}

Result<void, EngineError> RenderDevice::Execute(const FramePacket& packet) noexcept {
    if (!m_renderer) {
        return Err(EngineError{"RenderDevice is not initialized"});
    }

    // 1. Clear background using primary pass clear color
    Color clearCol = Color::FromRGBA8(18, 18, 24, 255);
    if (!packet.passes.empty()) {
        clearCol = packet.passes[0].clearColor;
    }

    const u8 cr = static_cast<u8>(std::clamp(clearCol.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    const u8 cg = static_cast<u8>(std::clamp(clearCol.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    const u8 cb = static_cast<u8>(std::clamp(clearCol.b, 0.0f, 1.0f) * 255.0f + 0.5f);
    const u8 ca = static_cast<u8>(std::clamp(clearCol.a, 0.0f, 1.0f) * 255.0f + 0.5f);

    SDL_SetRenderDrawColor(m_renderer, cr, cg, cb, ca);
    SDL_RenderClear(m_renderer);

    // 2. Iterate each coalesced draw batch
    for (const auto& batch : packet.batches) {
        const usize quadCount = batch.instanceCount;
        if (quadCount == 0) {
            continue;
        }

        // Resolve hardware texture handle: slot 0 is untextured solid geometry
        SDL_Texture* batchTexture = nullptr;
        const u16 texSlot = batch.textureSlot;
        if (texSlot > 0 && texSlot < kMaxTextureSlots) {
            batchTexture = m_textureSlots[texSlot];
        }

        // Configure blend mode: textured batches use the texture's blend mode
        const SDL_BlendMode bm = ToSdlBlend(static_cast<BlendMode>(batch.blendMode));
        if (batchTexture) {
            SDL_SetTextureBlendMode(batchTexture, bm);
        } else {
            SDL_SetRenderDrawBlendMode(m_renderer, bm);
        }

        // Expand quad instances into 4 vertices & 6 indices each without allocation
        for (usize i = 0; i < quadCount; ++i) {
            const auto& q = packet.instances[batch.instanceOffset + i];

            const float cx = q.pos[0];
            const float cy = q.pos[1];
            const float hw = q.size[0] * 0.5f;
            const float hh = q.size[1] * 0.5f;
            const float cosT = q.rotCosSin[0];
            const float sinT = q.rotCosSin[1];

            // Local rotated offsets
            const float lx0 = -hw * cosT - (-hh) * sinT;
            const float ly0 = -hw * sinT + (-hh) * cosT;

            const float lx1 =  hw * cosT - (-hh) * sinT;
            const float ly1 =  hw * sinT + (-hh) * cosT;

            const float lx2 =  hw * cosT - hh * sinT;
            const float ly2 =  hw * sinT + hh * cosT;

            const float lx3 = -hw * cosT - hh * sinT;
            const float ly3 = -hw * sinT + hh * cosT;

            // Unpack color (premultiplied RGBA8)
            const float r = static_cast<float>(q.colorPremul & 0xFF) / 255.0f;
            const float g = static_cast<float>((q.colorPremul >> 8) & 0xFF) / 255.0f;
            const float b = static_cast<float>((q.colorPremul >> 16) & 0xFF) / 255.0f;
            const float a = static_cast<float>((q.colorPremul >> 24) & 0xFF) / 255.0f;
            const SDL_FColor color{r, g, b, a};

            // UV coordinates with FlipX / FlipY support
            constexpr float kInv65535 = 1.0f / 65535.0f;
            float u0 = static_cast<float>(q.uvRect[0]) * kInv65535;
            float v0 = static_cast<float>(q.uvRect[1]) * kInv65535;
            float u1 = static_cast<float>(q.uvRect[2]) * kInv65535;
            float v1 = static_cast<float>(q.uvRect[3]) * kInv65535;

            if (q.kindFlags & QuadFlag_FlipX) {
                std::swap(u0, u1);
            }
            if (q.kindFlags & QuadFlag_FlipY) {
                std::swap(v0, v1);
            }

            const usize vBase = i * 4;
            m_vertexScratch[vBase + 0] = {{cx + lx0, cy + ly0}, color, {u0, v0}};
            m_vertexScratch[vBase + 1] = {{cx + lx1, cy + ly1}, color, {u1, v0}};
            m_vertexScratch[vBase + 2] = {{cx + lx2, cy + ly2}, color, {u1, v1}};
            m_vertexScratch[vBase + 3] = {{cx + lx3, cy + ly3}, color, {u0, v1}};

            const int iBase = static_cast<int>(vBase);
            const usize idxOffset = i * 6;
            m_indexScratch[idxOffset + 0] = iBase + 0;
            m_indexScratch[idxOffset + 1] = iBase + 1;
            m_indexScratch[idxOffset + 2] = iBase + 2;
            m_indexScratch[idxOffset + 3] = iBase + 2;
            m_indexScratch[idxOffset + 4] = iBase + 3;
            m_indexScratch[idxOffset + 5] = iBase + 0;
        }

        // Hardware-accelerated triangle batch draw
        SDL_RenderGeometry(
            m_renderer,
            batchTexture,
            m_vertexScratch.data(),
            static_cast<int>(quadCount * 4),
            m_indexScratch.data(),
            static_cast<int>(quadCount * 6)
        );
    }

    return {};
}

Result<PresentResult, EngineError> RenderDevice::Present(const PresentationPolicy& policy) noexcept {
    if (!m_renderer) {
        return Err(EngineError{"RenderDevice is not initialized"});
    }

    const bool wantVsync = (policy.mode == PacingMode::VSync);
    (void)SetVSync(wantVsync);

    SDL_RenderPresent(m_renderer);
    return PresentResult{};
}

Result<void, EngineError> RenderDevice::Submit(const FramePacket& packet) noexcept {
    if (auto r = Execute(packet); !r) {
        return r;
    }
    if (auto r = Present({}); !r) {
        return Err(r.Error());
    }
    return {};
}

} // namespace EngineN
