#include "renderer/LightingAndPostFx.h"
#include <algorithm>
#include <cmath>

namespace en {

namespace {
[[nodiscard]] f32 RaySegmentIntersectDist(Vec2 orig, Vec2 dir, Vec2 a, Vec2 b, f32 maxDist) noexcept {
    const Vec2 v1 = orig - a;
    const Vec2 v2 = b - a;
    const Vec2 v3{-dir.y, dir.x};
    const f32 dot = v2.Dot(v3);
    if (std::abs(dot) < 1e-6f) return maxDist;
    const f32 t1 = v2.Cross(v1) / dot;
    const f32 t2 = v1.Dot(v3) / dot;
    if (t1 >= 0.0f && t1 < maxDist && t2 >= 0.0f && t2 <= 1.0f) {
        return t1;
    }
    return maxDist;
}
} // namespace

u32 LightingAndPostFx::ComputeShadowVisibilityPolygon(
    Vec2 lightPos, f32 lightRadius, std::span<const ShadowOccluder2D> occluders,
    std::span<Vec2> outPolygonVertices) noexcept {
    if (outPolygonVertices.empty() || lightRadius <= 0.0f) return 0;
    const u32 rayCount = std::min<u32>(static_cast<u32>(outPolygonVertices.size()), 64U);
    const f32 step = kTwoPi / static_cast<f32>(rayCount);

    for (u32 i = 0; i < rayCount; ++i) {
        f32 c = 1.0f, s = 0.0f;
        FastSinCos(static_cast<f32>(i) * step, c, s);
        const Vec2 dir{c, s};
        f32 bestDist = lightRadius;
        for (const ShadowOccluder2D& occ : occluders) {
            bestDist = RaySegmentIntersectDist(lightPos, dir, occ.a, occ.b, bestDist);
        }
        outPolygonVertices[i] = lightPos + dir * bestDist;
    }
    return rayCount;
}

void LightingAndPostFx::EmitPointLightHalo(
    RenderList& list, Vec2 worldPos, f32 radius, Color color, f32 intensity,
    u8 passId, u8 layer, u8 viewSlot) noexcept {
    if (radius <= 0.01f || intensity <= 0.01f) return;
    Color outer = color;
    outer.a = std::clamp(intensity * 0.24f, 0.0f, 0.55f);
    Color inner = color;
    inner.a = std::clamp(intensity * 0.36f, 0.0f, 0.70f);

    QuadCmd outerCmd{};
    outerCmd.center = worldPos;
    outerCmd.size = {radius * 2.0f, radius * 2.0f};
    outerCmd.color = outer;
    outerCmd.texture = kBuiltinSoftLightTexture;
    outerCmd.passId = passId;
    outerCmd.layer = layer;
    outerCmd.viewSlot = viewSlot;
    outerCmd.blend = BlendMode::Additive;
    list.PushQuad(outerCmd);

    QuadCmd innerCmd = outerCmd;
    innerCmd.size = {radius * 0.9f, radius * 0.9f};
    innerCmd.color = inner;
    innerCmd.layer = static_cast<u8>(layer + 1U);
    list.PushQuad(innerCmd);
}

void LightingAndPostFx::EmitPostProcessOverlays(
    RenderList& list, const PostProcessConfig& cfg, u8 hudPassId, u8 screenViewSlot) noexcept {
    const ViewportExtent vp = list.Viewport();
    const f32 w = static_cast<f32>(vp.w);
    const f32 h = static_cast<f32>(vp.h);

    if (cfg.vignetteStrength > 0.01f) {
        const f32 band = std::min(w, h) * 0.08f;
        const Color vig{0.0f, 0.0f, 0.0f, std::clamp(cfg.vignetteStrength, 0.0f, 0.9f)};
        list.PushRect({0.0f, 0.0f, w, band}, vig, hudPassId, 200, 32768, screenViewSlot);
        list.PushRect({0.0f, h - band, w, band}, vig, hudPassId, 200, 32768, screenViewSlot);
        list.PushRect({0.0f, band, band, h - 2.0f * band}, vig, hudPassId, 200, 32768, screenViewSlot);
        list.PushRect({w - band, band, band, h - 2.0f * band}, vig, hudPassId, 200, 32768, screenViewSlot);
    }

    if (cfg.screenFlashColor.a > 0.01f) {
        list.PushRect({0.0f, 0.0f, w, h}, cfg.screenFlashColor, hudPassId, 210, 32768, screenViewSlot);
    }

    if (cfg.crtScanlines) {
        const Color lineCol{0.0f, 0.0f, 0.0f, 0.14f};
        for (f32 y = 0.0f; y < h; y += 4.0f) {
            list.PushRect({0.0f, y, w, 1.0f}, lineCol, hudPassId, 215, 32768, screenViewSlot);
        }
    }
}

} // namespace en
