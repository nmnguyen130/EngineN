#pragma once

#include "assets/AssetTypes.h"
#include "time/TimePipeline.h"

namespace en {

// Generational entity identifier for the ECS slot table.
// Value type; generation == 0 represents kNullEntity.
// Invariant: IsValid() holds iff index != 0 and generation != 0.
struct alignas(4) EntityId {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return index != 0U && generation != 0U; }

    [[nodiscard]] constexpr u32 Packed() const noexcept {
        return (static_cast<u32>(generation) << 16U) | static_cast<u32>(index);
    }

    [[nodiscard]] static constexpr EntityId FromPacked(u32 packed) noexcept {
        return {static_cast<u16>(packed & 0xFFFFU), static_cast<u16>(packed >> 16U)};
    }

    [[nodiscard]] constexpr auto operator<=>(const EntityId&) const noexcept = default;
};
static_assert(sizeof(EntityId) == 4);

inline constexpr EntityId kNullEntity{0U, 0U};
inline constexpr u16 kInvalidDenseIndex = 0xFFFFU;
inline constexpr usize kMaxComponentTypes = 64U;
inline constexpr usize kMaxHierarchyDepth = 32U;

template <typename T>
concept Component = std::is_trivially_copyable_v<T> &&
                    std::is_trivially_destructible_v<T> &&
                    std::is_standard_layout_v<T> &&
                    alignof(T) <= 64U &&
                    requires { { T::kTypeId } -> std::convertible_to<u8>; } &&
                    (static_cast<u8>(T::kTypeId) < kMaxComponentTypes);

template <Component... Ts>
[[nodiscard]] consteval u64 MakeComponentMask() noexcept {
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

struct alignas(4) WorldConfig {
    u16 maxEntities{4096};
    u16 defaultPoolCapacity{4096};
    u32 commandBufferBytes{131072};
    u32 arenaBytes{4U * 1024U * 1024U};
};
static_assert(sizeof(WorldConfig) == 12);

struct alignas(4) LocalTransform2D {
    static constexpr u8 kTypeId = 0;
    f32 x{0.0f};
    f32 y{0.0f};
    f32 scaleX{1.0f};
    f32 scaleY{1.0f};
    f32 rotationRad{0.0f};
    f32 depthOffset{0.0f};
};
static_assert(sizeof(LocalTransform2D) == 24);

struct alignas(4) PrevWorldSnapshot2D {
    f32 x{0.0f};
    f32 y{0.0f};
    f32 scaleX{1.0f};
    f32 scaleY{1.0f};
    f32 worldRotRad{0.0f};
    f32 worldDepth{0.0f};
};
static_assert(sizeof(PrevWorldSnapshot2D) == 24);

// Resolved world-space 2D transform with precomputed rotation sine and cosine.
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

    [[nodiscard]] constexpr LocalTransform2D ToLocal() const noexcept {
        return {x, y, scaleX, scaleY, worldRotRad, worldDepth};
    }

    [[nodiscard]] constexpr Vec2 TransformPoint(Vec2 localPt) const noexcept {
        const f32 sx = localPt.x * scaleX;
        const f32 sy = localPt.y * scaleY;
        return {x + (sx * cosRot - sy * sinRot), y + (sx * sinRot + sy * cosRot)};
    }
};
static_assert(sizeof(WorldTransform2D) == 32);

// Spawn parameters for World::Spawn; when keepWorldTransform is true, local is interpreted in world space.
struct alignas(4) EntitySpawnDesc {
    LocalTransform2D local{};
    EntityId parent{kNullEntity};
    TimeChannel timeChannel{TimeChannel::World};
    bool keepWorldTransform{false};
    u16 userFlags{0};
};
static_assert(sizeof(EntitySpawnDesc) == 32);

struct alignas(16) Velocity2D {
    static constexpr u8 kTypeId = 1;
    f32 vx{0.0f};
    f32 vy{0.0f};
    f32 angularVelRad{0.0f};
    f32 linearDamping{0.0f};
};
static_assert(sizeof(Velocity2D) == 16);

// 2D sprite component; flags pack [0]=visible, [1]=flipX, [2]=flipY, [3]=ySort, [4..5]=BlendMode, [6..7]=viewSlot.
struct alignas(32) Sprite2D {
    static constexpr u8 kTypeId = 2;
    Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    f32 width{1.0f};
    f32 height{1.0f};
    SpriteHandle sprite{0U, 1U};
    u16 baseDepth{32768U};
    u8 layer{10U};
    u8 flags{1U};

    [[nodiscard]] constexpr bool Visible() const noexcept { return (flags & 0x01U) != 0U; }
    [[nodiscard]] constexpr bool FlipX() const noexcept { return (flags & 0x02U) != 0U; }
    [[nodiscard]] constexpr bool FlipY() const noexcept { return (flags & 0x04U) != 0U; }
    [[nodiscard]] constexpr bool YSort() const noexcept { return (flags & 0x08U) != 0U; }
    [[nodiscard]] constexpr BlendMode Blend() const noexcept {
        return static_cast<BlendMode>((flags >> 4U) & 0x03U);
    }
    [[nodiscard]] constexpr u8 ViewSlot() const noexcept { return static_cast<u8>((flags >> 6U) & 0x03U); }

    constexpr void SetVisible(bool v) noexcept { flags = static_cast<u8>((flags & ~0x01U) | u8(v)); }
    constexpr void SetFlipX(bool f) noexcept { flags = static_cast<u8>((flags & ~0x02U) | (u8(f) << 1U)); }
    constexpr void SetFlipY(bool f) noexcept { flags = static_cast<u8>((flags & ~0x04U) | (u8(f) << 2U)); }
    constexpr void SetYSort(bool y) noexcept { flags = static_cast<u8>((flags & ~0x08U) | (u8(y) << 3U)); }
    constexpr void SetBlend(BlendMode b) noexcept {
        flags = static_cast<u8>((flags & ~0x30U) | ((static_cast<u8>(b) & 0x03U) << 4U));
    }
    constexpr void SetViewSlot(u8 slot) noexcept {
        flags = static_cast<u8>((flags & ~0xC0U) | ((slot & 0x03U) << 6U));
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

struct alignas(32) CameraRig2D {
    static constexpr u8 kTypeId = 4;
    Rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
    f32 zoom{1.0f};
    f32 pixelsPerMeter{48.0f};
    f32 trauma{0.0f};
    u8 viewSlot{0};
    u8 passId{0};
    bool screenSpace{false};
    bool active{true};
};
static_assert(sizeof(CameraRig2D) == 32);

struct alignas(16) Lifetime2D {
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

struct alignas(16) Tag2D {
    static constexpr u8 kTypeId = 7;
    StringId name{};
    u32 categoryMask{0};
    u32 userData{0};
};
static_assert(sizeof(Tag2D) == 16);

inline constexpr u32 kCookedPrefabMagic = 0x42465045U; // 'EPFB'
inline constexpr u32 kWorldSnapshotMagic = 0x504E5357U; // 'WSNP'

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

struct alignas(16) WorldSnapshotHeader {
    u32 magic{kWorldSnapshotMagic};
    u16 version{1};
    u16 entityCount{0};
    u32 tickCount{0};
    u32 payloadCrc32{0};
};
static_assert(sizeof(WorldSnapshotHeader) == 16);

} // namespace en
