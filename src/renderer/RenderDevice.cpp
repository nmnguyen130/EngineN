#include "renderer/RenderDevice.h"
#include "renderer/SdlRendererBackend.h"
#include "platform/Window.h"

#include <algorithm>

namespace EngineN {

namespace {

// NullBackend: lightweight headless mock for CI, automated testing, and headless benchmarks
class NullBackend final : public IRenderBackend {
public:
    Result<void> Init(Window*, const RenderDeviceConfig& cfg) override {
        m_cfg = cfg;
        return Ok;
    }
    void Shutdown() override {}
    Result<void> CreateTexture(TextureHandle, const TextureConfig&, const void*) override { return Ok; }
    Result<void> UpdateTexture(TextureHandle, const Rect&, const void*) override { return Ok; }
    void DestroyTexture(TextureHandle) override {}
    Result<void> SetVSync(VSyncMode) override { return Ok; }
    Result<void> SetLogicalPresentation(i32 w, i32 h, PresentationMode m) override {
        m_cfg.virtualWidth = w;
        m_cfg.virtualHeight = h;
        m_cfg.presentation = m;
        return Ok;
    }
    Result<void> BeginFrame(Color) override { return Ok; }
    Result<void> Submit(const BatchView&) override { return Ok; }
    Result<void> Present() override { return Ok; }
    const BackendCaps& GetCaps() const noexcept override {
        static const BackendCaps caps{
            .backendName = "NullBackend (Headless)",
            .maxTextureDimension = 8192,
            .vsyncAdaptive = true,
            .supportsCustomShaders = false
        };
        return caps;
    }
    Vec2 WindowToVirtual(Vec2 windowPx) const noexcept override { return windowPx; }
    Vec2 VirtualToWindow(Vec2 virtualPx) const noexcept override { return virtualPx; }
    FRect GetPresentationRect() const noexcept override {
        return FRect{0.0f, 0.0f, static_cast<f32>(m_cfg.virtualWidth), static_cast<f32>(m_cfg.virtualHeight)};
    }
private:
    RenderDeviceConfig m_cfg{};
};

} // namespace

RenderDevice::RenderDevice() = default;

RenderDevice::~RenderDevice() {
    Destroy();
}

RenderDevice::RenderDevice(RenderDevice&&) noexcept = default;
RenderDevice& RenderDevice::operator=(RenderDevice&&) noexcept = default;

Result<void> RenderDevice::Create(Window& window, const RenderDeviceConfig& cfg) {
    Destroy();
    m_cfg = cfg;
    m_textureSlots.resize(std::max(16u, m_cfg.maxTextures));

    // Initialize hardware backend powered by SDL3
    m_backend = std::make_unique<SdlRendererBackend>();
    auto initRes = m_backend->Init(&window, m_cfg);
    if (!initRes) {
        m_backend.reset();
        return initRes;
    }

    return InitBuiltInWhiteTexture();
}

Result<void> RenderDevice::CreateHeadless(const RenderDeviceConfig& cfg) {
    Destroy();
    m_cfg = cfg;
    m_textureSlots.resize(std::max(16u, m_cfg.maxTextures));

    // Initialize mock backend for headless execution without OS window
    m_backend = std::make_unique<NullBackend>();
    auto initRes = m_backend->Init(nullptr, m_cfg);
    if (!initRes) {
        return initRes;
    }

    return InitBuiltInWhiteTexture();
}

void RenderDevice::Destroy() {
    if (m_backend) {
        // Invalidate and destroy all in-use textures
        for (usize i = 0; i < m_textureSlots.size(); ++i) {
            if (m_textureSlots[i].inUse) {
                m_backend->DestroyTexture({static_cast<u16>(i), m_textureSlots[i].generation});
                m_textureSlots[i].inUse = false;
                m_textureSlots[i].generation++; // Invalidate stale handles
            }
        }
        m_backend->Shutdown();
        m_backend.reset();
    }
    m_textureSlots.clear();
    m_isInsideFrame = false;
}

Result<void> RenderDevice::InitBuiltInWhiteTexture() {
    // Slot 1 is permanently reserved for built-in 1x1 white texture (used by primitive batching)
    const u32 whitePixel = 0xFFFFFFFF;
    TextureConfig whiteCfg{
        .width = 1,
        .height = 1,
        .filter = TextureFilter::Nearest,
        .premultiplyAlpha = false
    };

    m_textureSlots[1].config = whiteCfg;
    m_textureSlots[1].generation = 1;
    m_textureSlots[1].inUse = true;

    return m_backend->CreateTexture(TextureHandle::White(), whiteCfg, &whitePixel);
}

// Frame Lifecycle

Result<void> RenderDevice::BeginFrame() {
    if (m_isInsideFrame) {
        return Unexpected(MakeRenderError(
            RenderErrc::FrameOrderViolation,
            "RenderDevice::BeginFrame() called while already inside a frame"));
    }
    m_isInsideFrame = true;
    return m_backend->BeginFrame(m_cfg.clearColor);
}

Result<void> RenderDevice::Submit(const BatchView& batch) {
    if (!m_isInsideFrame) {
        return Unexpected(MakeRenderError(
            RenderErrc::FrameOrderViolation,
            "RenderDevice::Submit() called outside of BeginFrame()/Present()"));
    }
    return m_backend->Submit(batch);
}

Result<void> RenderDevice::Present() {
    if (!m_isInsideFrame) {
        return Unexpected(MakeRenderError(
            RenderErrc::FrameOrderViolation,
            "RenderDevice::Present() called without a matching BeginFrame()"));
    }
    m_isInsideFrame = false;
    return m_backend->Present();
}

// Texture Lifecycle (Generational Handles)

Result<TextureHandle> RenderDevice::CreateTexture(
    const TextureConfig& cfg,
    const void* initialPixels)
{
    if (cfg.width == 0 || cfg.height == 0) {
        return Unexpected(MakeRenderError(
            RenderErrc::InvalidConfiguration,
            "Invalid texture dimensions (must be > 0)"));
    }

    // Find free slot starting at index 2 (0 is Null, 1 is White)
    u16 freeSlot = 0;
    for (usize i = 2; i < m_textureSlots.size(); ++i) {
        if (!m_textureSlots[i].inUse) {
            freeSlot = static_cast<u16>(i);
            break;
        }
    }

    if (freeSlot == 0) {
        return Unexpected(MakeRenderError(
            RenderErrc::TextureLimitReached,
            "Texture slot table capacity exhausted"));
    }

    TextureSlot& slot = m_textureSlots[freeSlot];
    slot.config = cfg;
    slot.inUse = true;
    if (slot.generation == 0) slot.generation = 1;

    const TextureHandle handle{freeSlot, slot.generation};

    auto res = m_backend->CreateTexture(handle, cfg, initialPixels);
    if (!res) {
        slot.inUse = false;
        return Unexpected(res.Error());
    }

    return handle;
}

Result<void> RenderDevice::UpdateTexture(
    TextureHandle handle,
    const Rect& region,
    const void* pixels)
{
    if (!IsTextureValid(handle)) {
        return Unexpected(MakeRenderError(
            RenderErrc::TextureNotFound,
            "Attempted to update invalid or stale TextureHandle"));
    }
    return m_backend->UpdateTexture(handle, region, pixels);
}

void RenderDevice::DestroyTexture(TextureHandle handle) {
    if (!IsTextureValid(handle) || handle.index <= 1) {
        return; // Slot 0 is Null, Slot 1 is permanent White
    }

    m_backend->DestroyTexture(handle);
    TextureSlot& slot = m_textureSlots[handle.index];
    slot.inUse = false;
    slot.generation++; // Increment generation to catch stale handle dereferences
    if (slot.generation == 0) slot.generation = 1;
}

bool RenderDevice::IsTextureValid(TextureHandle handle) const noexcept {
    if (handle.index == 0 || handle.index >= m_textureSlots.size()) return false;
    const TextureSlot& slot = m_textureSlots[handle.index];
    return slot.inUse && slot.generation == handle.generation;
}

const TextureConfig* RenderDevice::GetTextureConfig(TextureHandle handle) const noexcept {
    if (!IsTextureValid(handle)) return nullptr;
    return &m_textureSlots[handle.index].config;
}

// Display Settings & Coordinate Mapping

Result<void> RenderDevice::SetVSync(VSyncMode mode) {
    m_cfg.vsync = mode;
    return m_backend->SetVSync(mode);
}

Result<void> RenderDevice::SetPresentation(i32 w, i32 h, PresentationMode mode) {
    m_cfg.virtualWidth = w;
    m_cfg.virtualHeight = h;
    m_cfg.presentation = mode;
    return m_backend->SetLogicalPresentation(w, h, mode);
}

Vec2 RenderDevice::WindowToVirtual(Vec2 windowPx) const noexcept {
    return m_backend->WindowToVirtual(windowPx);
}

Vec2 RenderDevice::VirtualToWindow(Vec2 virtualPx) const noexcept {
    return m_backend->VirtualToWindow(virtualPx);
}

FRect RenderDevice::GetPresentationRect() const noexcept {
    return m_backend->GetPresentationRect();
}

const BackendCaps& RenderDevice::GetCaps() const noexcept {
    return m_backend->GetCaps();
}

} // namespace EngineN
