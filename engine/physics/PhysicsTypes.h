#pragma once

#include "core/Math.h"
#include "world/WorldTypes.h"

namespace en {

enum class TileType : u8 {
    Empty = 0,
    Solid = 1,
    OneWay = 2,
    SlopeUpRight = 3,
    SlopeUpLeft = 4,
    SpikeHazard = 5,
    WaterVolume = 6,
    ConveyorRight = 7,
    ConveyorLeft = 8,
};

enum class ContactFlags : u8 {
    None = 0,
    Ground = 1U << 0,
    Ceiling = 1U << 1,
    WallLeft = 1U << 2,
    WallRight = 1U << 3,
    OnSlope = 1U << 4,
    OnOneWay = 1U << 5,
    InHazard = 1U << 6,
    InWater = 1U << 7,
};
consteval bool enable_bitmask_operators(ContactFlags) { return true; }

enum class MotorFlags : u8 {
    None = 0,
    UseGravity = 1U << 0,
    SlideWalls = 1U << 1,
    SnapToSlopes = 1U << 2,
    Crushed = 1U << 3,
    Noclip = 1U << 4,
};
consteval bool enable_bitmask_operators(MotorFlags) { return true; }

struct alignas(8) CompactAabb2D {
    i16 offsetX_q8{0};
    i16 offsetY_q8{0};
    u16 halfW_q8{128};
    u16 halfH_q8{128};

    [[nodiscard]] static constexpr CompactAabb2D FromMeters(
        f32 ox, f32 oy, f32 hw, f32 hh) noexcept {
        return {
            static_cast<i16>(std::clamp(ox * 256.0f, -32767.0f, 32767.0f)),
            static_cast<i16>(std::clamp(oy * 256.0f, -32767.0f, 32767.0f)),
            static_cast<u16>(std::clamp(hw * 256.0f, 1.0f, 65535.0f)),
            static_cast<u16>(std::clamp(hh * 256.0f, 1.0f, 65535.0f)),
        };
    }

    [[nodiscard]] constexpr Vec2 Offset() const noexcept {
        return {static_cast<f32>(offsetX_q8) * (1.0f / 256.0f),
                static_cast<f32>(offsetY_q8) * (1.0f / 256.0f)};
    }
    [[nodiscard]] constexpr Vec2 HalfExtents() const noexcept {
        return {static_cast<f32>(halfW_q8) * (1.0f / 256.0f),
                static_cast<f32>(halfH_q8) * (1.0f / 256.0f)};
    }
    [[nodiscard]] constexpr Rect WorldRect(Vec2 worldPos) const noexcept {
        const Vec2 c = worldPos + Offset();
        const Vec2 h = HalfExtents();
        return {c.x - h.x, c.y - h.y, h.x * 2.0f, h.y * 2.0f};
    }
};
static_assert(sizeof(CompactAabb2D) == 8);

struct alignas(32) KinematicMotor2D {
    static constexpr u8 kTypeId = 16;
    f32 vx{0.0f};
    f32 vy{0.0f};
    CompactAabb2D bounds{CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.8f)};
    EntityId carrierId{kNullEntity};
    i16 slopeNormalX_q14{0};
    i16 slopeNormalY_q14{-16384};
    u16 solidMask{1};
    u8 bodyLayer{1};
    u8 pushboxGroup{0};
    ContactFlags contacts{ContactFlags::None};
    MotorFlags flags{MotorFlags::UseGravity | MotorFlags::SlideWalls | MotorFlags::SnapToSlopes};
    u8 oneWayIgnoreTicks{0};
    u8 stepCooldownTicks{0};

    [[nodiscard]] constexpr bool Grounded() const noexcept {
        return HasFlag(contacts, ContactFlags::Ground);
    }
    [[nodiscard]] constexpr bool WallSliding() const noexcept {
        return HasFlag(contacts, ContactFlags::WallLeft | ContactFlags::WallRight) && !Grounded();
    }
};
static_assert(sizeof(KinematicMotor2D) == 32);

struct alignas(16) KinematicCarrier2D {
    static constexpr u8 kTypeId = 17;
    f32 deltaX{0.0f};
    f32 deltaY{0.0f};
    CompactAabb2D bounds{CompactAabb2D::FromMeters(0.0f, 0.0f, 1.5f, 0.25f)};
};
static_assert(sizeof(KinematicCarrier2D) == 16);

struct alignas(32) MotionStack2D {
    static constexpr u8 kTypeId = 18;
    f32 targetWalkVx{0.0f};
    f32 overrideVx{0.0f};
    f32 overrideVy{0.0f};
    f32 impulseVx{0.0f};
    f32 impulseVy{0.0f};
    u16 gravityScale_q8{256};
    u16 overrideTicks{0};
    u8 activePriority{0};
    u8 groundCoyoteTicks{0};
    u8 wallCoyoteTicks{0};
    i8 facingSign{1};
    i8 wallSign{0};
    bool lockFacing{false};
    u16 motionFlags{0};

    constexpr void SetDashOverride(f32 vx, f32 vy, u16 ticks, u8 priority = 10) noexcept {
        if (priority >= activePriority || overrideTicks == 0) {
            overrideVx = vx;
            overrideVy = vy;
            overrideTicks = ticks;
            activePriority = priority;
        }
    }
    constexpr void AddImpulse(f32 ix, f32 iy) noexcept {
        impulseVx += ix;
        impulseVy += iy;
    }
};
static_assert(sizeof(MotionStack2D) == 32);

struct alignas(8) TriggerVolume2D {
    static constexpr u8 kTypeId = 15;
    CompactAabb2D bounds{CompactAabb2D::FromMeters(0.0f, 0.0f, 1.0f, 1.0f)};
    StringId triggerTag{};
};
static_assert(sizeof(TriggerVolume2D) == 16);

struct alignas(8) DistanceJoint2D {
    static constexpr u8 kTypeId = 14;
    EntityId targetEntity{kNullEntity};
    f32 restLength{2.0f};
    f32 stiffness{0.8f};
    f32 damping{0.1f};
};
static_assert(sizeof(DistanceJoint2D) == 16);

struct alignas(4) VerletRope2D {
    static constexpr u8 kTypeId = 13;
    static constexpr usize kMaxNodes = 8;
    std::array<Vec2, kMaxNodes> currPos{};
    std::array<Vec2, kMaxNodes> prevPos{};
    f32 segmentLength{0.35f};
    f32 damping{0.985f};
    u8 nodeCount{6};
    bool pinStartToEntity{true};
    u16 solverIterations{4};
};
static_assert(sizeof(VerletRope2D) == 140);

struct SlabImpact2D {
    f32 toi{1.0f};
    Vec2 normal{0.0f, 0.0f};
    EntityId hitEntity{kNullEntity};
    bool hit{false};
};

struct RayHit2D {
    f32 distance{0.0f};
    Vec2 point{};
    Vec2 normal{};
    EntityId entity{kNullEntity};
    TileType tile{TileType::Empty};
    bool hit{false};
};

struct KinematicStepConfig {
    f32 gravity{32.0f};
    f32 maxFallSpeed{22.0f};
    f32 wallSlideSpeed{5.5f};
    f32 groundAccel{95.0f};
    f32 airAccel{65.0f};
    f32 stepUpHeight{0.35f};
    f32 slopeSnapDist{0.25f};
    f32 waterBuoyancy{18.0f};
    f32 waterDrag{4.0f};
    f32 conveyorSpeed{3.5f};
    u8 maxCoyoteTicks{10};
};

inline constexpr u32 kCookedCollisionMagic = 0x4C4F4345U; // 'ECOL'

struct alignas(8) CookedCollisionHeader {
    u32 magic{kCookedCollisionMagic};
    u16 width{0};
    u16 height{0};
    f32 tileSize{1.0f};
    f32 originX{0.0f};
    f32 originY{0.0f};
    u32 tileCrc32{0};
};
static_assert(sizeof(CookedCollisionHeader) == 24);

} // namespace en
