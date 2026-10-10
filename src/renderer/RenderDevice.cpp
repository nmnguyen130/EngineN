#include "renderer/RenderDevice.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace EngineN {

RenderDevice::~RenderDevice() {
    Shutdown();
}

RenderDevice::RenderDevice(RenderDevice&& other) noexcept {
    *this = std::move(other);
}

RenderDevice& RenderDevice::operator=(RenderDevice&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_renderer = other.m_renderer;
        m_vertexScratch = std::move(other.m_vertexScratch);
        m_indexScratch = std::move(other.m_indexScratch);
        other.m_renderer = nullptr;
    }
    return *this;
}

Result<void, EngineError> RenderDevice::Init(Window& window, const RenderDeviceConfig& config) noexcept {
    if (m_renderer != nullptr) {
        Shutdown();
    }

    if (!window.IsValid()) {
        return Err(EngineError{"Window handle is invalid for RenderDevice initialization"});
    }

    m_renderer = SDL_CreateRenderer(window.Native(), nullptr);
    if (!m_renderer) {
        return Err(EngineError{SDL_GetError()});
    }

    (void)SDL_SetRenderVSync(m_renderer, config.vsync ? 1 : 0);

    // Pre-allocate scratch storage to guarantee 0 heap allocations per frame
    constexpr usize kInitialQuadCapacity = 4096;
    m_vertexScratch.reserve(kInitialQuadCapacity * 4);
    m_indexScratch.reserve(kInitialQuadCapacity * 6);

    const char* driver = SDL_GetRendererName(m_renderer);
    ENGINE_LOG(Info, Render, "RenderDevice initialized. Hardware backend: {}", driver ? driver : "Default");

    return {};
}

void RenderDevice::Shutdown() noexcept {
    if (m_renderer != nullptr) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    m_vertexScratch.clear();
    m_indexScratch.clear();
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

Result<void, EngineError> RenderDevice::Submit(const FramePacket& packet) noexcept {
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
        if (batch.instanceCount == 0) {
            continue;
        }

        // Configure blending mode
        switch (static_cast<BlendMode>(batch.blendMode)) {
            case BlendMode::Additive:
                SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_ADD);
                break;
            case BlendMode::Opaque:
                SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
                break;
            case BlendMode::Premultiplied:
            default:
                SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
                break;
        }

        const usize quadCount = batch.instanceCount;
        m_vertexScratch.resize(quadCount * 4);
        m_indexScratch.resize(quadCount * 6);

        // Expand quad instances into 4 vertices & 6 indices each
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

            // UV coordinates
            constexpr float kInv65535 = 1.0f / 65535.0f;
            const float u0 = static_cast<float>(q.uvRect[0]) * kInv65535;
            const float v0 = static_cast<float>(q.uvRect[1]) * kInv65535;
            const float u1 = static_cast<float>(q.uvRect[2]) * kInv65535;
            const float v1 = static_cast<float>(q.uvRect[3]) * kInv65535;

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
            nullptr, // Slot 0 is untextured solid geometry
            m_vertexScratch.data(),
            static_cast<int>(m_vertexScratch.size()),
            m_indexScratch.data(),
            static_cast<int>(m_indexScratch.size())
        );
    }

    // Present swapchain to window display
    SDL_RenderPresent(m_renderer);
    return {};
}

} // namespace EngineN
