#pragma once

#include "assets/AssetTypes.h"
#include "time/TimePipeline.h"

namespace en {

struct EntityTag {};
using EntityId = Handle<EntityTag>;
inline constexpr EntityId kNullEntity{0, 0};
inline constexpr u16 kInvalidDenseIndex = 0xFFFFU;
inline constexpr usize kMaxComponentTypes = 64;
inline constexpr usize kMaxHierarchyDepth = 32;

using ComponentMask = u64;

template <typename T>
concept Component = std::is_trivially_copyable_v<T> &&
                    std::is_trivially_destructible_v<T> &&
                    alignof(T) <= 64 &&
                    requires {
                        { T::kTypeId } -> std::convertible_to<u8>;
                    } && (static_cast<u8>(T::kTypeId) < kMaxComponentTypes);

template <Component... Ts>
[[nodiscard]] consteval ComponentMask MakeComponentMask() noexcept {
    return ((1ULL << static_cast<u8>(Ts::kTypeId)) | ... | 0ULL);
}

template <Component... Ts>
struct Exclude {};

enum class WorldErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    EntityCapacityReached,
    DeadEntity,
    PoolNotRegistered,
    PoolCapacityReached,
    HierarchyCycle,
    HierarchyDepthExceeded,
    CommandBufferOverflow,
    CorruptedPrefab,
};

struct WorldConfig {
    u16 maxEntities{4096};
    u16 defaultPoolCapacity{2048};
    u32 commandBufferBytes{131072};
    u32 arenaBytes{8 * 1024 * 1024};
};

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

struct PrevWorldSnapshot2D {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 scaleX{1.0f};
    f32 scaleY{1.0f};
    f32 worldRotRad{0.0f};
    f32 worldDepth{0.0f};
};
static_assert(sizeof(PrevWorldSnapshot2D) == 24);

struct alignas(32) WorldTransform2D {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 scaleX{1.0f};
    f32 scaleY{1.0f};
    f32 worldRotRad{0.0f};
    f32 worldDepth{0.0f};
    f32 cosRot{1.0f};
    f32 sinRot{0.0f};

    [[nodiscard]] constexpr PrevWorldSnapshot2D ToSnapshot() const noexcept {
        return {x, y, scaleX, scaleY, worldRotRad, worldDepth};
    }
};
static_assert(sizeof(WorldTransform2D) == 32);

struct EntitySpawnDesc {
    LocalTransform2D local{};
    EntityId parent{kNullEntity};
    TimeChannel timeChannel{TimeChannel::World};
};

struct alignas(16) Velocity2D {
    static constexpr u8 kTypeId = 1;
    f32 vx{0.0f};
    f32 vy{0.0f};
    f32 angularVelRad{0.0f};
    f32 linearDamping{0.0f};
};
static_assert(sizeof(Velocity2D) == 16);

struct alignas(32) Sprite2D {
    static constexpr u8 kTypeId = 2;
    Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    f32 width{1.0f};
    f32 height{1.0f};
    SpriteHandle sprite{0, 1};
    u16 baseDepth{32768};
    u8 layer{10};
    u8 flags{1}; // bit 0: visible, bit 1: flipX, bit 2: flipY

    [[nodiscard]] constexpr bool Visible() const noexcept { return (flags & 1U) != 0; }
    [[nodiscard]] constexpr bool FlipX() const noexcept { return (flags & 2U) != 0; }
    [[nodiscard]] constexpr bool FlipY() const noexcept { return (flags & 4U) != 0; }
    constexpr void SetVisible(bool v) noexcept {
        if (v) flags |= 1U;
        else flags &= static_cast<u8>(~1U);
    }
    constexpr void SetFlipX(bool f) noexcept {
        if (f) flags |= 2U;
        else flags &= static_cast<u8>(~2U);
    }
};
static_assert(sizeof(Sprite2D) == 32);

struct alignas(16) SpriteAnimator2D {
    static constexpr u8 kTypeId = 3;
    AtlasHandle atlas{};
    u16 firstFrame{0};
    u16 frameCount{1};
    u16 currentFrame{0};
    u16 ticksPerFrame{6};
    u16 tickAccum{0};
    bool loop{true};
    bool playing{true};
};
static_assert(sizeof(SpriteAnimator2D) == 16);

struct alignas(8) CameraRig2D {
    static constexpr u8 kTypeId = 4;
    Rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
    f32 zoom{1.0f};
    u8 viewSlot{0};
    bool screenSpace{false};
    bool active{true};
    u8 passId{0};
};
static_assert(sizeof(CameraRig2D) == 32);

struct alignas(8) Lifetime2D {
    static constexpr u8 kTypeId = 5;
    u32 remainingTicks{60};
    u32 initialTicks{60};
    f32 alphaScale{1.0f};
    bool fadeSpriteAlpha{false};
    bool active{true};
    u16 userTag{0};
};
static_assert(sizeof(Lifetime2D) == 16);

struct alignas(32) PointLight2D {
    static constexpr u8 kTypeId = 6;
    Color color{1.0f, 0.9f, 0.7f, 1.0f};
    f32 radius{4.0f};
    f32 intensity{1.0f};
    f32 pulseSpeed{0.0f};
    f32 pulsePhase{0.0f};
};
static_assert(sizeof(PointLight2D) == 32);

struct alignas(8) Tag2D {
    static constexpr u8 kTypeId = 7;
    StringId name{};
    u32 categoryMask{0};
    u32 userData{0};
};
static_assert(sizeof(Tag2D) == 16);

inline constexpr u32 kCookedPrefabMagic = 0x42465045U; // 'EPFB'

struct alignas(16) CookedPrefabHeader {
    u32 magic{kCookedPrefabMagic};
    u16 version{1};
    u16 entityCount{0};
    u32 componentRecordCount{0};
    u32 schemaCrc32{0};
};
static_assert(sizeof(CookedPrefabHeader) == 16);

struct alignas(8) CookedPrefabEntityRecord {
    LocalTransform2D local{};
    i16 parentEntityIndex{-1};
    u8 timeChannel{0};
    u8 componentCount{0};
    u32 entityFlags{0};
};
static_assert(sizeof(CookedPrefabEntityRecord) == 32);

} // namespace en
