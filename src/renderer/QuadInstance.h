#pragma once

#include "core/Types.h"
#include "renderer/RendererTypes.h"
#include <algorithm>
#include <cmath>

namespace EngineN {

enum class QuadKind : u8 {
    Sprite      = 0,
    RectFilled  = 1,
    RectStroked = 2,
    Circle      = 3,
    Capsule     = 4,
    Line        = 5,
    MSDFText    = 6,
    NineSlice   = 7,
};

enum QuadFlags : u32 {
    QuadFlag_None   = 0,
    QuadFlag_FlipX  = 1u << 8,
    QuadFlag_FlipY  = 1u << 9,
    QuadFlag_Opaque = 1u << 10,
};

/**
 * QuadInstance: Canonical 48-byte layout consumed by the vertex storage buffer.
 * Vertex shader expands 6 vertices procedurally from SV_VertexID.
 */
struct alignas(16) QuadInstance {
    f32 pos[2]{0.0f, 0.0f};          //  8 B: Center/pivot position
    f32 size[2]{0.0f, 0.0f};         //  8 B: Width, height (or extents)
    f32 rotCosSin[2]{1.0f, 0.0f};    //  8 B: cos(theta), sin(theta) for zero-trig rotation in VS
    u16 uvRect[4]{0, 0, 65535, 65535};// 8 B: u0, v0, u1, v1 in unorm16
    u32 colorPremul{0xFFFFFFFF};     //  4 B: Packed RGBA8 (premultiplied alpha)
    u32 kindFlags{0};                //  4 B: [0..7] QuadKind, [8..31] QuadFlags
    f32 param[2]{0.0f, 0.0f};        //  8 B: Corner radius / stroke thickness / MSDF range

    static constexpr u16 FloatToUnorm16(f32 val) noexcept {
        const f32 clamped = std::clamp(val, 0.0f, 1.0f);
        return static_cast<u16>(clamped * 65535.0f + 0.5f);
    }

    static constexpr void PackUV(u16 out[4], f32 u0, f32 v0, f32 u1, f32 v1) noexcept {
        out[0] = FloatToUnorm16(u0);
        out[1] = FloatToUnorm16(v0);
        out[2] = FloatToUnorm16(u1);
        out[3] = FloatToUnorm16(v1);
    }

    [[nodiscard]] constexpr QuadKind GetKind() const noexcept {
        return static_cast<QuadKind>(kindFlags & 0xFF);
    }

    [[nodiscard]] constexpr bool HasFlag(QuadFlags flag) const noexcept {
        return (kindFlags & static_cast<u32>(flag)) != 0;
    }

    void SetUV(f32 u0, f32 v0, f32 u1, f32 v1) noexcept {
        PackUV(uvRect, u0, v0, u1, v1);
    }

    void SetRotation(f32 radians) noexcept {
        rotCosSin[0] = std::cos(radians);
        rotCosSin[1] = std::sin(radians);
    }

    void SetColor(const Color& color) noexcept {
        colorPremul = color.ToPremultipliedRGBA8();
    }
};

static_assert(sizeof(QuadInstance) == 48, "QuadInstance must be exactly 48 bytes");
static_assert(alignof(QuadInstance) == 16, "QuadInstance must be 16-byte aligned");

} // namespace EngineN
