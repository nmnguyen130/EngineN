#pragma once

#include "renderer/RendererTypes.h"
#include "renderer/Texture.h"
#include "renderer/RenderQueue.h"
#include "renderer/RenderDevice.h"
#include "renderer/PrimitiveBatch.h"

#include <array>
#include <optional>

namespace EngineN {

struct RenderState {
    ViewTransform2D view{};
    Mat2D           viewMatrix = Mat2D::Identity();
    RenderPass      pass = RenderPass::World;
    u8              layer = 0;
    f32             depth = 0.0f;
    BlendMode       blend = BlendMode::Premultiplied;
    Color           tint = Colors::White;
    u8              material = 0;
    u16             scissorId = 0;
    bool            pixelSnap = false;
};

struct SpriteDrawDesc {
    TextureRegion      region{};
    Transform2D        transform{};
    Color              tint = Colors::White;
    SpriteFlip         flip = SpriteFlip::None;
    std::optional<f32> depthOverride = std::nullopt;
};

/// Per-frame graphics state machine and command recorder.
/// Transforms geometry at record time into virtual-screen coordinates.
class RenderContext {
public:
    RenderContext() : m_primitives(*this) {}
    ~RenderContext() = default;

    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    [[nodiscard]] Result<void> Init(RenderDevice& device, const RenderQueueConfig& qcfg = {}) {
        m_device = &device;
        return m_queue.Init(qcfg);
    }

    void BeginFrame() noexcept {
        m_queue.BeginFrame();
        m_state = RenderState{};
        m_stateStackDepth = 0;
    }

    [[nodiscard]] Result<void> EndFrame() {
        if (!m_device) {
            return Unexpected(MakeRenderError(
                RenderErrc::ContextInitFailed,
                "RenderContext has not been initialized with a RenderDevice"));
        }
        const BatchView batch = m_queue.Finalize();
        return m_device->Submit(batch);
    }

    // State Management & Stack
    void PushState() noexcept {
        if (m_stateStackDepth < m_stateStack.size()) {
            m_stateStack[m_stateStackDepth++] = m_state;
        }
    }

    void PopState() noexcept {
        if (m_stateStackDepth > 0) {
            m_state = m_stateStack[--m_stateStackDepth];
        }
    }

    class ScopedState {
    public:
        explicit ScopedState(RenderContext& ctx) noexcept : m_ctx(ctx) { m_ctx.PushState(); }
        ~ScopedState() noexcept { m_ctx.PopState(); }
        ScopedState(const ScopedState&) = delete;
        ScopedState& operator=(const ScopedState&) = delete;
    private:
        RenderContext& m_ctx;
    };

    void SetView(const ViewTransform2D& view) noexcept {
        m_state.view = view;
        m_state.viewMatrix = view.ToMatrix();
    }

    void SetScreenSpace() noexcept {
        m_state.view.screenSpace = true;
        m_state.viewMatrix = Mat2D::Identity();
    }

    void SetPass(RenderPass pass) noexcept { m_state.pass = pass; }
    void SetLayer(u8 layer) noexcept { m_state.layer = layer; }
    void SetDepth(f32 depth) noexcept { m_state.depth = depth; }
    void SetBlendMode(BlendMode blend) noexcept { m_state.blend = blend; }
    void SetTint(Color tint) noexcept { m_state.tint = tint; }
    void SetMaterial(u8 material) noexcept { m_state.material = material; }
    void SetPixelSnap(bool snap) noexcept { m_state.pixelSnap = snap; }

    void SetScissor(std::optional<Rect> scissor) noexcept {
        if (scissor && !scissor->IsEmpty()) {
            m_state.scissorId = m_queue.InternScissor(*scissor);
        } else {
            m_state.scissorId = 0;
        }
    }

    [[nodiscard]] const RenderState& GetState() const noexcept { return m_state; }
    [[nodiscard]] PrimitiveBatch& Primitives() noexcept { return m_primitives; }
    [[nodiscard]] RenderQueue& Queue() noexcept { return m_queue; }

    // Sprite Drawing
    void DrawSprite(const SpriteDrawDesc& desc) noexcept {
        if (!desc.region.handle.IsValid() || desc.region.pixelWidth == 0 || desc.region.pixelHeight == 0) {
            return;
        }

        const f32 w = static_cast<f32>(desc.region.pixelWidth);
        const f32 h = static_cast<f32>(desc.region.pixelHeight);

        // Quad corners relative to origin
        const Vec2 p0{0.0f, 0.0f};
        const Vec2 p1{w, 0.0f};
        const Vec2 p2{w, h};
        const Vec2 p3{0.0f, h};

        const Mat2D modelMatrix = desc.transform.ToMatrix();
        const Mat2D mvMatrix = m_state.viewMatrix * modelMatrix;

        Vec2 v0 = mvMatrix.TransformPoint(p0);
        Vec2 v1 = mvMatrix.TransformPoint(p1);
        Vec2 v2 = mvMatrix.TransformPoint(p2);
        Vec2 v3 = mvMatrix.TransformPoint(p3);

        // Frustum culling: calculate AABB of transformed corners
        const f32 minX = std::min({v0.x, v1.x, v2.x, v3.x});
        const f32 maxX = std::max({v0.x, v1.x, v2.x, v3.x});
        const f32 minY = std::min({v0.y, v1.y, v2.y, v3.y});
        const f32 maxY = std::max({v0.y, v1.y, v2.y, v3.y});

        const FRect bounds{minX, minY, maxX - minX, maxY - minY};
        if (!bounds.Overlaps(m_state.view.viewport)) {
            // Culled
            return;
        }

        if (m_state.pixelSnap) {
            v0.x = std::round(v0.x); v0.y = std::round(v0.y);
            v1.x = std::round(v1.x); v1.y = std::round(v1.y);
            v2.x = std::round(v2.x); v2.y = std::round(v2.y);
            v3.x = std::round(v3.x); v3.y = std::round(v3.y);
        }

        const auto flippedRegion = desc.region.Flipped(desc.flip);
        const FRect uv = flippedRegion.uvRect;

        const u32 colorPacked = (m_state.tint * desc.tint).Premultiplied().ToPackedRGBA8();

        auto res = m_queue.Reserve(4, 6);
        if (!res.isValid) return;

        res.vertices[0] = {v0.x, v0.y, uv.x, uv.y, colorPacked};
        res.vertices[1] = {v1.x, v1.y, uv.x + uv.w, uv.y, colorPacked};
        res.vertices[2] = {v2.x, v2.y, uv.x + uv.w, uv.y + uv.h, colorPacked};
        res.vertices[3] = {v3.x, v3.y, uv.x, uv.y + uv.h, colorPacked};

        res.indices[0] = 0; res.indices[1] = 1; res.indices[2] = 2;
        res.indices[3] = 0; res.indices[4] = 2; res.indices[5] = 3;

        const f32 drawDepth = desc.depthOverride.value_or(m_state.depth);
        const u64 key = SortKey::Pack(
            m_state.pass,
            m_state.layer,
            drawDepth,
            m_state.blend,
            m_state.material,
            desc.region.handle.index
        );

        m_queue.Commit(res, key, m_state.scissorId, 4, 6);
    }

private:
    RenderDevice*                  m_device = nullptr;
    RenderQueue                    m_queue;
    PrimitiveBatch                 m_primitives;
    RenderState                    m_state{};
    std::array<RenderState, 16>    m_stateStack{};
    usize                          m_stateStackDepth = 0;
};

} // namespace EngineN
