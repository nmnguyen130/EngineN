#pragma once

#include "core/Types.h"
#include "renderer/RendererTypes.h"
#include "renderer/QuadInstance.h"
#include "renderer/SortKey.h"
#include "renderer/FrameArena.h"
#include "asset/AssetReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace EngineN {

enum class Blend : u8 {
    Alpha = 0,
    Additive,
    Opaque,
    Multiply
};

enum class SortPolicy : u8 {
    Submission = 0,
    Depth,
    YSort
};

struct LayerDesc {
    u8 id;
    SortPolicy policy;
};

namespace Layers {
    inline constexpr LayerDesc Background{ 10, SortPolicy::Submission };
    inline constexpr LayerDesc Tiles     { 20, SortPolicy::Submission };
    inline constexpr LayerDesc Actors    { 30, SortPolicy::YSort };
    inline constexpr LayerDesc Fx        { 40, SortPolicy::Submission };
    inline constexpr LayerDesc Ui        { 60, SortPolicy::Submission };
}

struct SpriteDesc {
    SpriteView sprite{};
    Vec2       pos{};           // Pivot in world coordinates
    Vec2       scale{1.0f, 1.0f};
    f32        rotation = 0.0f; // Radians about pivot
    Color      tint = Colors::White;
    Blend      blend = Blend::Alpha;
    bool       flipX = false;
    bool       flipY = false;
    u16        depth = 0;       // For SortPolicy::Depth
    f32        sortY = 0.0f;    // For SortPolicy::YSort
};

// Additive trick: Premultiplied with alpha forced to 0 merges with alpha blend state
[[nodiscard]] constexpr u32 PackAdditive(const Color& c) noexcept {
    const f32 a = std::clamp(c.a, 0.0f, 1.0f);
    const auto q = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(c.b * a) << 16) | (q(c.g * a) << 8) | q(c.r * a); // Alpha byte is 0
}

struct QueueCounters {
    u32 droppedQuads = 0;
};

/**
 * RenderQueue: Typed extraction facade wrapping FrameArenaSlot.
 * Gameplay code never constructs QuadInstance or bit-shifts SortKey manually.
 */
class RenderQueue {
public:
    explicit RenderQueue(FrameArenaSlot& arena) noexcept : m_arena(arena) {}

    void Begin(const ViewTransform2D& view, Color clear) noexcept {
        m_arena.Reset();
        m_counters = {};
        (void)m_arena.PushPass({.pass = RenderPass::Scene, .enabled = true, .clearColor = clear});
        (void)m_arena.PushViewSlot({.view = view});
        m_view = view;
        m_layer = Layers::Tiles;
        m_pass = RenderPass::Scene;
    }

    void SetLayer(LayerDesc l) noexcept { m_layer = l; }
    void SetPass(RenderPass p)   noexcept { m_pass = p; }

    [[nodiscard]] const ViewTransform2D& View() const noexcept { return m_view; }
    [[nodiscard]] const QueueCounters& Counters() const noexcept { return m_counters; }

    [[nodiscard]] bool Sprite(const SpriteDesc& d) noexcept {
        const f32 w = d.sprite.size[0] * d.scale.x;
        const f32 h = d.sprite.size[1] * d.scale.y;
        const f32 c = std::cos(d.rotation);
        const f32 s = std::sin(d.rotation);

        // Quad center = pivot position + R * (half size - pivot)
        const f32 ox = w * 0.5f - d.sprite.pivot[0] * d.scale.x;
        const f32 oy = h * 0.5f - d.sprite.pivot[1] * d.scale.y;

        QuadInstance q{};
        q.pos[0] = d.pos.x + ox * c - oy * s;
        q.pos[1] = d.pos.y + ox * s + oy * c;
        q.size[0] = w;
        q.size[1] = h;
        q.rotCosSin[0] = c;
        q.rotCosSin[1] = s;
        std::memcpy(q.uvRect, d.sprite.uvRect, sizeof(q.uvRect));

        q.colorPremul = (d.blend == Blend::Additive)
            ? PackAdditive(d.tint)
            : d.tint.ToPremultipliedRGBA8();

        u32 flags = 0;
        if (d.flipX) flags |= QuadFlag_FlipX;
        if (d.flipY) flags |= QuadFlag_FlipY;
        q.kindFlags = flags;

        const BlendMode bm = ToBackendBlend(d.blend);
        const u16 depth = DepthFor(d);
        const u64 key = SortKey::Pack(
            static_cast<u8>(m_pass),
            m_layer.id,
            depth,
            static_cast<u8>(bm),
            /*pipeline: Sprite*/ 0,
            d.sprite.texture.slot
        );

        return Push(q, key);
    }

private:
    static constexpr BlendMode ToBackendBlend(Blend b) noexcept {
        switch (b) {
            case Blend::Opaque:   return BlendMode::Opaque;
            case Blend::Multiply: return BlendMode::Multiply;
            default:              return BlendMode::Premultiplied; // Additive aliases Premultiplied
        }
    }

    [[nodiscard]] u16 DepthFor(const SpriteDesc& d) const noexcept {
        switch (m_layer.policy) {
            case SortPolicy::Submission: return 0;
            case SortPolicy::Depth:      return d.depth;
            case SortPolicy::YSort:      return static_cast<u16>(std::clamp(d.sortY, 0.0f, 65535.0f));
        }
        return 0;
    }

    bool Push(const QuadInstance& q, u64 key) noexcept {
        if (m_arena.PushQuad(q, SortKey{key}, 0)) {
            return true;
        }
        ++m_counters.droppedQuads;
        return false;
    }

    FrameArenaSlot& m_arena;
    ViewTransform2D m_view{};
    LayerDesc       m_layer = Layers::Tiles;
    RenderPass      m_pass  = RenderPass::Scene;
    QueueCounters   m_counters{};
};

} // namespace EngineN
