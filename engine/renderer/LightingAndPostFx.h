#pragma once

#include "renderer/RenderList.h"

namespace en {

// 2D dynamic lighting, 1D shadow hull visibility solver, and post-processing pass emitter.
// Stateless utility class submitting light halos, shadow rays, bloom, vignette, and CRT passes.
// Invariant: ComputeShadowVisibilityPolygon outputs vertices in counter-clockwise angular order.
class LightingAndPostFx final {
public:
    [[nodiscard]] static u32 ComputeShadowVisibilityPolygon(
        Vec2 lightPos,
        f32 lightRadius,
        std::span<const ShadowOccluder2D> occluders,
        std::span<Vec2> outPolygonVertices) noexcept;

    static void EmitPointLightHalo(
        RenderList& list,
        Vec2 worldPos,
        f32 radius,
        Color color,
        f32 intensity,
        u8 passId = 0,
        u8 layer = 40,
        u8 viewSlot = 0) noexcept;

    [[nodiscard]] static constexpr Color EvaluateNormalLitColor(Color baseAlbedo, Vec3 surfaceNormal, Vec3 toLightDir, Color lightColor, f32 ambientIntensity = 0.25f) noexcept {
        const f32 ndotl = std::max(0.0f, surfaceNormal.x * toLightDir.x + surfaceNormal.y * toLightDir.y + surfaceNormal.z * toLightDir.z);
        return Color{
            std::clamp(baseAlbedo.r * (ambientIntensity + lightColor.r * ndotl), 0.0f, 1.0f),
            std::clamp(baseAlbedo.g * (ambientIntensity + lightColor.g * ndotl), 0.0f, 1.0f),
            std::clamp(baseAlbedo.b * (ambientIntensity + lightColor.b * ndotl), 0.0f, 1.0f),
            baseAlbedo.a};
    }

    static void EmitPostProcessOverlays(
        RenderList& list,
        const PostProcessConfig& cfg,
        u8 hudPassId = 0,
        u8 screenViewSlot = 15) noexcept;
};

} // namespace en
