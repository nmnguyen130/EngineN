#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "core/time/TimeTypes.h"
#include "assets/AssetTypes.h"
#include "renderer/RendererTypes.h"

#include <cmath>
#include <concepts>
#include <type_traits>

namespace en::world {

enum class WorldErrc : u8 {
    InvalidConfig = 0,
    OutOfMemory,
    EntityPoolExhausted,
    ComponentPoolExhausted,
    CommandBufferOverflow,
    InvalidEntity,
    ComponentNotRegistered,
    HierarchyCycleDetected,
    HierarchyDepthExceeded,
    CorruptedPrefabBlob,
};

struct EntityId {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return index != 0 && generation != 0;
    }
    constexpr bool operator==(const EntityId&) const noexcept = default;
};

inline constexpr EntityId kNullEntity{0, 0};
inline constexpr u16 kInvalidDenseIndex = 0xFFFF;
inline constexpr u8 kMaxComponentTypes  = 64;
inline constexpr u8 kMaxHierarchyDepth  = 32;

using ComponentMask = u64;

enum class EntityFlags : u8 {
    None          = 0,
    Alive         = 1 << 0,
    Enabled       = 1 << 1,
    Spatial       = 1 << 2,
    StaticSpatial = 1 << 3,
    Teleported    = 1 << 4,
};

[[nodiscard]] constexpr EntityFlags operator|(EntityFlags a, EntityFlags b) noexcept {
    return static_cast<EntityFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr EntityFlags operator&(EntityFlags a, EntityFlags b) noexcept {
    return static_cast<EntityFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}
[[nodiscard]] constexpr EntityFlags operator~(EntityFlags a) noexcept {
    return static_cast<EntityFlags>(~static_cast<u8>(a));
}
[[nodiscard]] constexpr bool HasFlag(EntityFlags flags, EntityFlags test) noexcept {
    return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0;
}

template <typename T>
concept Component =
    std::is_trivially_copyable_v<T> &&
    std::is_trivially_destructible_v<T> &&
    std::is_standard_layout_v<T> &&
    (alignof(T) <= 64) &&
    requires {
        { T::kTypeId } -> std::convertible_to<u8>;
    } && (T::kTypeId < kMaxComponentTypes);

template <Component... Ts>
struct Exclude {};

template <Component... Ts>
[[nodiscard]] consteval ComponentMask MakeComponentMask() noexcept {
    return ((ComponentMask{1} << Ts::kTypeId) | ... | ComponentMask{0});
}

// 24B zero-padding local spatial state.
struct LocalTransform2D {
    static constexpr u8 kTypeId = 0;

    f32 x{0.0f};
    f32 y{0.0f};
    f32 scaleX{1.0f};
    f32 scaleY{1.0f};
    f32 rotationRad{0.0f};
    f32 depthOffset{0.0f};
};
static_assert(sizeof(LocalTransform2D) == 24);

// 32B cache-aligned world spatial state (2 per 64B cache line).
struct alignas(32) WorldTransform2D {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 scaleX{1.0f};
    f32 scaleY{1.0f};
    f32 cosRot{1.0f};
    f32 sinRot{0.0f};
    f32 worldRotRad{0.0f};
    f32 worldDepth{0.0f};
};
static_assert(sizeof(WorldTransform2D) == 32);

// 24B previous-tick world snapshot for per-channel render interpolation.
struct PrevWorldSnapshot2D {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 scaleX{1.0f};
    f32 scaleY{1.0f};
    f32 worldRotRad{0.0f};
    f32 worldDepth{0.0f};
};
static_assert(sizeof(PrevWorldSnapshot2D) == 24);

// 16B linear & angular velocity integrated at 120 Hz for non-colliding ambient entities.
struct alignas(16) Velocity2D {
    static constexpr u8 kTypeId = 1;

    f32 vx{0.0f};
    f32 vy{0.0f};
    f32 angularVelRad{0.0f};
    f32 linearDamping{0.0f};
};
static_assert(sizeof(Velocity2D) == 16);

enum class SpriteFlags : u8 {
    None   = 0,
    FlipX  = 1 << 0,
    FlipY  = 1 << 1,
    Hidden = 1 << 2,
};

// 32B cache-aligned visual quad descriptor (2 per 64B cache line).
struct alignas(32) Sprite2D {
    static constexpr u8 kTypeId = 2;

    renderer::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    assets::SpriteHandle sprite{};
    u16 baseDepth{32768};
    i16 ySortScale{0};
    i16 localOffsetX{0};
    i16 localOffsetY{0};
    u8 passId{0};
    u8 layer{0};
    u8 viewSlot{0};
    u8 blendAndFlags{0};
};
static_assert(sizeof(Sprite2D) == 32);

// 16B Q8.8 fixed-point 120 Hz atlas frame animator (4 per 64B cache line).
// Stepped exclusively by gameplay::AnimationSubsystem in lockstep with Sprite2D.
struct alignas(16) SpriteAnimator2D {
    static constexpr u8 kTypeId = 3;
    static constexpr u16 kSubTicksPerTick = 256;

    assets::AtlasHandle atlas{};
    u16 firstFrame{0};
    u16 frameCount{1};
    u16 currentFrame{0};
    u16 ticksPerFrame{12};
    u16 subTickAccum{0};
    u8 loop{1};
    u8 playing{1};
};
static_assert(sizeof(SpriteAnimator2D) == 16);

// 40B 2D camera rig component extracted into RenderList::SetView.
struct alignas(8) CameraRig2D {
    static constexpr u8 kTypeId = 4;

    renderer::FRect viewport{0.0f, 0.0f, 1920.0f, 1080.0f};
    renderer::FRect scissor{0.0f, 0.0f, 1920.0f, 1080.0f};
    f32 zoom{1.0f};
    u8 viewSlot{0};
    bool screenSpace{false};
    bool active{true};
    u8 pad{0};
};
static_assert(sizeof(CameraRig2D) == 40);

struct EntitySpawnDesc {
    LocalTransform2D localTransform{};
    time::TimeChannel timeChannel{time::TimeChannel::World};
    EntityId parent{kNullEntity};
    bool hasSpatial{true};
    bool isStaticSpatial{false};
};

struct WorldConfig {
    u16 maxEntities{16384};
    u32 commandBufferBytes{262144};
    u16 defaultComponentPoolCapacity{4096};
};

[[nodiscard]] constexpr f32 NormalizeAngleRad(f32 rad) noexcept {
    constexpr f32 kTwoPi    = 6.2831853071795864769f;
    constexpr f32 kInvTwoPi = 0.15915494309189533577f;
    const f32 turns = rad * kInvTwoPi;
    const i32 rounded = static_cast<i32>(turns + (turns >= 0.0f ? 0.5f : -0.5f));
    return rad - kTwoPi * static_cast<f32>(rounded);
}

[[nodiscard]] constexpr f32 ShortestAngleDelta(f32 fromRad, f32 toRad) noexcept {
    return NormalizeAngleRad(toRad - fromRad);
}

constexpr void FastSinCos(f32 rad, f32& outCos, f32& outSin) noexcept {
    constexpr f32 kPi     = 3.14159265358979323846f;
    constexpr f32 kHalfPi = 1.57079632679489661923f;

    f32 x = NormalizeAngleRad(rad);
    const f32 signCos = (std::abs(x) > kHalfPi) ? -1.0f : 1.0f;
    if (x > kHalfPi) {
        x = kPi - x;
    } else if (x < -kHalfPi) {
        x = -kPi - x;
    }

    const f32 x2 = x * x;
    outSin = x * (1.0f + x2 * (-0.166666567f + x2 * (0.008332075f - x2 * 0.000195152f)));
    outCos = (1.0f + x2 * (-0.49999925f + x2 * (0.04166358f - x2 * 0.00136950f))) * signCos;
}

} // namespace en::world
