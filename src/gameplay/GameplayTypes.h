#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "core/time/TimeTypes.h"
#include "core/time/TimeViews.h"
#include "platform/input/InputTypes.h"
#include "assets/AssetTypes.h"
#include "world/WorldTypes.h"

#include <array>
#include <span>

namespace en::assets { class AssetBank; }
namespace en::input { class ActionBuffer; }
namespace en::world { class World; }

namespace en::gameplay {

enum class GameplayErrc : u8 {
    InvalidConfig = 0,
    OutOfMemory,
    CorruptedBlobHeader,
    CorruptedBlobPayload,
    ArenaExhausted,
    WorldRegistrationFailed
};

enum class ContactFlags : u8 {
    None           = 0,
    Ground         = 1u << 0,
    Ceiling        = 1u << 1,
    WallLeft       = 1u << 2,
    WallRight      = 1u << 3,
    OnSlope        = 1u << 4,
    OnOneWay       = 1u << 5,
    LedgeGrabLeft  = 1u << 6,
    LedgeGrabRight = 1u << 7
};

[[nodiscard]] constexpr ContactFlags operator|(ContactFlags a, ContactFlags b) noexcept {
    return static_cast<ContactFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr ContactFlags operator&(ContactFlags a, ContactFlags b) noexcept {
    return static_cast<ContactFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}
[[nodiscard]] constexpr ContactFlags operator~(ContactFlags a) noexcept {
    return static_cast<ContactFlags>(~static_cast<u8>(a));
}
constexpr ContactFlags& operator|=(ContactFlags& a, ContactFlags b) noexcept {
    a = a | b;
    return a;
}
constexpr ContactFlags& operator&=(ContactFlags& a, ContactFlags b) noexcept {
    a = a & b;
    return a;
}
[[nodiscard]] constexpr bool HasContact(ContactFlags flags, ContactFlags test) noexcept {
    return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0;
}

enum class MotorFlags : u8 {
    None             = 0,
    SnapToGround     = 1u << 0,
    CornerCorrection = 1u << 1,
    ProjectSlopes    = 1u << 2,
    DetectLedges     = 1u << 3,
    SoftPushbox      = 1u << 4,
    TrackSafeAnchor  = 1u << 5,
    Crushed          = 1u << 6,
    Submerged        = 1u << 7
};

[[nodiscard]] constexpr MotorFlags operator|(MotorFlags a, MotorFlags b) noexcept {
    return static_cast<MotorFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr MotorFlags operator&(MotorFlags a, MotorFlags b) noexcept {
    return static_cast<MotorFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}
[[nodiscard]] constexpr MotorFlags operator~(MotorFlags a) noexcept {
    return static_cast<MotorFlags>(~static_cast<u8>(a));
}
constexpr MotorFlags& operator|=(MotorFlags& a, MotorFlags b) noexcept {
    a = a | b;
    return a;
}
constexpr MotorFlags& operator&=(MotorFlags& a, MotorFlags b) noexcept {
    a = a & b;
    return a;
}
[[nodiscard]] constexpr bool HasMotorFlag(MotorFlags flags, MotorFlags test) noexcept {
    return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0;
}

enum class CancelCategory : u16 {
    None            = 0,
    MovementTurn    = 1u << 0,
    Jump            = 1u << 1,
    WallJump        = 1u << 2,
    EvadeDash       = 1u << 3,
    PrimaryAttack   = 1u << 4,
    SecondarySkill  = 1u << 5,
    DefensiveParry  = 1u << 6,
    UtilityInteract = 1u << 7,
    CrouchSlide     = 1u << 8,
    LedgeMantle     = 1u << 9,
    CustomCategory0 = 1u << 10,
    CustomCategory1 = 1u << 11,
    AnyAction       = 0xFFFFu
};

enum class TileShape : u8 {
    Empty = 0,
    FullSolid,
    OneWayTop,
    HazardSolid,
    Slope45Asc,
    Slope45Desc,
    Slope26LoAsc,
    Slope26HiAsc,
    Slope26LoDesc,
    Slope26HiDesc
};

enum class TileMaterialFlags : u8 {
    None       = 0,
    SafeAnchor = 1u << 0,
    Slippery   = 1u << 1,
    ConveyorL  = 1u << 2,
    ConveyorR  = 1u << 3,
    Hazard     = 1u << 4
};

// Quantized 8-byte local AABB in Q8.8 signed meters (range [-128.0m, +127.996m], ~3.9mm precision).
struct CompactAabb2D {
    i16 offsetX_q8{0};
    i16 offsetY_q8{0};
    u16 halfW_q8{128};
    u16 halfH_q8{256};

    [[nodiscard]] static constexpr CompactAabb2D FromMeters(
        f32 offsetX, f32 offsetY, f32 halfW, f32 halfH) noexcept {
        return CompactAabb2D{
            static_cast<i16>(offsetX * 256.0f),
            static_cast<i16>(offsetY * 256.0f),
            static_cast<u16>(halfW * 256.0f),
            static_cast<u16>(halfH * 256.0f)
        };
    }

    [[nodiscard]] constexpr f32 OffsetX() const noexcept { return static_cast<f32>(offsetX_q8) * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 OffsetY() const noexcept { return static_cast<f32>(offsetY_q8) * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 HalfW()   const noexcept { return static_cast<f32>(halfW_q8)   * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 HalfH()   const noexcept { return static_cast<f32>(halfH_q8)   * (1.0f / 256.0f); }
};
static_assert(sizeof(CompactAabb2D) == 8);

// Authoritative swept-collision body, soft pushbox separator, and ledge/slope sensor.
// Entities owning KinematicMotor2D must not attach world::Velocity2D to avoid double integration.
// Invariant: bounds.halfW_q8 > 0 && bounds.halfH_q8 > 0.
struct alignas(32) KinematicMotor2D {
    static constexpr u8 kTypeId = 16;

    f32 vx{0.0f};
    f32 vy{0.0f};
    CompactAabb2D bounds{};
    world::EntityId carrierId{world::kNullEntity};
    i16 slopeNormalX_q14{0};
    i16 slopeNormalY_q14{16384};
    u16 solidMask{0x000Fu};
    u8 bodyLayer{4};
    u8 pushboxGroup{1};
    ContactFlags contacts{ContactFlags::None};
    MotorFlags flags{MotorFlags::SnapToGround | MotorFlags::CornerCorrection |
                     MotorFlags::ProjectSlopes | MotorFlags::DetectLedges | MotorFlags::SoftPushbox};
    time::TimeChannel timeChannel{time::TimeChannel::Player};
    u8 oneWayIgnoreTicks{0};

    [[nodiscard]] constexpr f32 SlopeNormalX() const noexcept {
        return static_cast<f32>(slopeNormalX_q14) * (1.0f / 16384.0f);
    }
    [[nodiscard]] constexpr f32 SlopeNormalY() const noexcept {
        return static_cast<f32>(slopeNormalY_q14) * (1.0f / 16384.0f);
    }
    constexpr void SetSlopeNormal(f32 nx, f32 ny) noexcept {
        slopeNormalX_q14 = static_cast<i16>(nx * 16384.0f);
        slopeNormalY_q14 = static_cast<i16>(ny * 16384.0f);
    }
};
static_assert(sizeof(KinematicMotor2D) == 32);

// Moving solid carrier (platforms, elevators, pistons) evaluated in Stage 1 before actor movement.
// Invariant: deltaX and deltaY store the exact displacement executed during the current 120 Hz tick.
struct alignas(16) KinematicCarrier2D {
    static constexpr u8 kTypeId = 17;

    f32 deltaX{0.0f};
    f32 deltaY{0.0f};
    CompactAabb2D bounds{};
};
static_assert(sizeof(KinematicCarrier2D) == 16);

// Priority motion arbitration stack, animation root-motion accumulator, and forgiveness timers.
// Pure POD mechanism mutated during Stage 2, Stage 3, and Stage 4.
// Invariant: overrideTicks == 0 implies activePriority == 0 (locomotion tier).
struct alignas(32) MotionStack2D {
    static constexpr u8 kTypeId = 18;

    f32 targetWalkVx{0.0f};
    f32 overrideVx{0.0f};
    f32 overrideVy{0.0f};
    f32 impulseVx{0.0f};
    f32 impulseVy{0.0f};
    i16 rootMotionDx_q8{0};
    i16 rootMotionDy_q8{0};
    u16 gravityScale_q8{256};
    u16 overrideTicks{0};
    u8 activePriority{0};
    u8 groundCoyoteTicks{0};
    u8 wallCoyoteTicks{0};
    i8 packedSigns{1};

    [[nodiscard]] constexpr i8 FacingSign() const noexcept {
        return (packedSigns & 0x01) != 0 ? i8{1} : i8{-1};
    }
    constexpr void SetFacingSign(i8 sign) noexcept {
        if (sign >= 0) {
            packedSigns |= 0x01;
        } else {
            packedSigns &= ~0x01;
        }
    }

    [[nodiscard]] constexpr i8 WallSign() const noexcept {
        const u8 raw = static_cast<u8>((packedSigns >> 1) & 0x03);
        if (raw == 1u) return 1;
        if (raw == 2u) return -1;
        return 0;
    }
    constexpr void SetWallSign(i8 sign) noexcept {
        packedSigns &= ~0x06;
        if (sign > 0) packedSigns |= 0x02;
        else if (sign < 0) packedSigns |= 0x04;
    }

    [[nodiscard]] constexpr bool IsCarrierLaunchActive() const noexcept {
        return (packedSigns & 0x08) != 0;
    }
    constexpr void SetCarrierLaunchActive(bool active) noexcept {
        if (active) packedSigns |= 0x08;
        else packedSigns &= ~0x08;
    }

    constexpr bool PushOverride(u8 priority, f32 vx, f32 vy, u16 durationTicks, u16 gravScaleQ8 = 256) noexcept {
        if (priority < activePriority && overrideTicks > 0) {
            return false;
        }
        activePriority = priority;
        overrideVx = vx;
        overrideVy = vy;
        overrideTicks = durationTicks;
        gravityScale_q8 = gravScaleQ8;
        return true;
    }
};
static_assert(sizeof(MotionStack2D) == 32);

// Data-driven Finite State Machine, bitmask cancel-window evaluator, and on-hit cancel promoter.
// Invariant: lastAnimFrame == 0xFFFF on clip change so frame 0 directives always fire.
struct alignas(16) StateMachine2D {
    static constexpr u8 kTypeId = 19;

    assets::BlobHandle rigBlob{};
    u16 stateTicks{0};
    u16 cancelMask{0xFFFFu};
    u16 onHitCancelMask{0};
    u16 lastAnimFrame{0xFFFFu};
    u8 stateId{0};
    u8 prevStateId{0};
    u8 animClipId{0};
    u8 hurtboxProfileId{0};

    [[nodiscard]] constexpr bool CanCancelInto(CancelCategory cat) const noexcept {
        return (cancelMask & static_cast<u16>(cat)) != 0;
    }

    constexpr void PromoteCancelWindowOnHit() noexcept {
        cancelMask |= onHitCancelMask;
    }

    constexpr void TransitionTo(u8 nextState, u8 nextClipId, u16 initialCancelMask = 0) noexcept {
        prevStateId = stateId;
        stateId = nextState;
        stateTicks = 0;
        cancelMask = initialCancelMask;
        onHitCancelMask = 0;
        if (animClipId != nextClipId) {
            animClipId = nextClipId;
            lastAnimFrame = 0xFFFFu;
        }
    }
};
static_assert(sizeof(StateMachine2D) == 16);

enum class HitboxRigFlags : u8 {
    None          = 0,
    HurtboxActive = 1u << 0,
    HitboxActive  = 1u << 1,
    ClashParryable= 1u << 2,
    PogoSurface   = 1u << 3,
    SweptDashHull = 1u << 4,
    LatchedAnimPlay = 1u << 7
};

// Multi-socket hitbox/hurtbox rig with 64-bit SplitMix Bloom Filter swing deduplication.
// Invariant: swingId increments on every new attack activation, zeroing hitBloomMask.
struct alignas(32) HitboxRig2D {
    static constexpr u8 kTypeId = 20;

    u64 hitBloomMask{0};
    CompactAabb2D hurtbox{};
    CompactAabb2D primaryHitbox{};
    u16 hitLayerMask{0};
    u16 hurtLayerMask{0};
    u16 swingId{0};
    u8 activeSliceIndex{0xFFu};
    u8 rigFlags{static_cast<u8>(HitboxRigFlags::HurtboxActive) |
                static_cast<u8>(HitboxRigFlags::LatchedAnimPlay)};

    constexpr void ActivateSlice(u8 sliceIdx, CompactAabb2D unionBox, bool newSwing) noexcept {
        if (newSwing) {
            ++swingId;
            hitBloomMask = 0;
        }
        activeSliceIndex = sliceIdx;
        primaryHitbox = unionBox;
        rigFlags |= static_cast<u8>(HitboxRigFlags::HitboxActive);
    }

    constexpr void DeactivateHitbox() noexcept {
        activeSliceIndex = 0xFFu;
        rigFlags &= ~static_cast<u8>(HitboxRigFlags::HitboxActive);
    }

    [[nodiscard]] static constexpr u64 ComputeEntityBloomBits(world::EntityId id) noexcept {
        u32 h = (static_cast<u32>(id.generation) << 16u) | static_cast<u32>(id.index);
        h = ((h >> 16u) ^ h) * 0x45d9f3bu;
        h = ((h >> 16u) ^ h) * 0x45d9f3bu;
        h = (h >> 16u) ^ h;
        return (1ULL << (h & 63u)) | (1ULL << ((h >> 6u) & 63u));
    }

    [[nodiscard]] constexpr bool HasAlreadyHit(world::EntityId target) const noexcept {
        const u64 mask = ComputeEntityBloomBits(target);
        return (hitBloomMask & mask) == mask;
    }

    constexpr void MarkHit(world::EntityId target) noexcept {
        hitBloomMask |= ComputeEntityBloomBits(target);
    }
};
static_assert(sizeof(HitboxRig2D) == 32);

// Policy-free health, poise, invulnerability frame, and stun envelope.
// Invariant: hp <= maxHp.
struct alignas(16) DamageReceiver2D {
    static constexpr u8 kTypeId = 21;

    i32 hp{100};
    i32 maxHp{100};
    u16 iframeTicks{0};
    u16 stunTicks{0};
    u16 poise{100};
    u8 teamId{0};
    u8 receiverFlags{0};
};
static_assert(sizeof(DamageReceiver2D) == 16);

// Tracks the last two verified 3-ray static non-hazard ground anchors for hazard/pit respawn.
struct alignas(16) SafeGroundBreadcrumb2D {
    static constexpr u8 kTypeId = 22;

    f32 safeX{0.0f};
    f32 safeY{0.0f};
    f32 prevSafeX{0.0f};
    f32 prevSafeY{0.0f};
};
static_assert(sizeof(SafeGroundBreadcrumb2D) == 16);

enum class CameraCtrlFlags : u8 {
    None                 = 0,
    LockVerticalAirborne = 1u << 0,
    ClampToRoom          = 1u << 1,
    LockAxisX            = 1u << 2,
    LockAxisY            = 1u << 3
};

// Multi-target weighted camera director with directional hit-kick spring, trauma noise, and Hermite room blender.
// Invariant: roomBlendTicks <= roomBlendDuration.
struct alignas(64) CameraController2D {
    static constexpr u8 kTypeId = 23;

    world::EntityId primaryTarget{world::kNullEntity};
    world::EntityId secondaryTarget{world::kNullEntity};
    f32 velX{0.0f};
    f32 velY{0.0f};
    f32 latchedGroundY{0.0f};
    f32 kickOffsetX{0.0f};
    f32 kickOffsetY{0.0f};
    CompactAabb2D activeRoomBounds{};
    CompactAabb2D prevRoomBounds{};
    i16 lookaheadX_q8{0};
    i16 peekOffsetY_q8{0};
    i16 kickVelX_q8{0};
    i16 kickVelY_q8{0};
    u16 trauma_q16{0};
    u16 roomBlendTicks{0};
    u16 roomBlendDuration{36};
    u8 secondaryWeight_q8{0};
    u8 deadzoneHalfW_q4{20};
    u8 deadzoneHalfH_q4{28};
    u8 springFreqX_q4{96};
    u8 springFreqY_q4{112};
    u8 cameraFlags{static_cast<u8>(CameraCtrlFlags::LockVerticalAirborne) |
                   static_cast<u8>(CameraCtrlFlags::ClampToRoom)};

    constexpr void TransitionToRoom(CompactAabb2D newBounds, u16 blendTicks = 36) noexcept {
        prevRoomBounds = activeRoomBounds;
        activeRoomBounds = newBounds;
        roomBlendDuration = blendTicks > 0 ? blendTicks : u16{1};
        roomBlendTicks = blendTicks;
    }

    constexpr void AddDirectionalKick(f32 impulseVx, f32 impulseVy) noexcept {
        const i32 vx = static_cast<i32>(kickVelX_q8) + static_cast<i32>(impulseVx * 256.0f);
        const i32 vy = static_cast<i32>(kickVelY_q8) + static_cast<i32>(impulseVy * 256.0f);
        kickVelX_q8 = static_cast<i16>(vx < -32767 ? -32767 : (vx > 32767 ? 32767 : vx));
        kickVelY_q8 = static_cast<i16>(vy < -32767 ? -32767 : (vy > 32767 ? 32767 : vy));
    }

    constexpr void AddTrauma(f32 amount01) noexcept {
        const u32 add = static_cast<u32>(amount01 > 0.0f ? amount01 * 65535.0f : 0.0f);
        const u32 sum = static_cast<u32>(trauma_q16) + add;
        trauma_q16 = static_cast<u16>(sum > 65535u ? 65535u : sum);
    }
};
static_assert(sizeof(CameraController2D) == 64);

// Environmental force volume (wind, currents, conveyors, high-viscosity fluid).
struct alignas(8) EnvironmentalZone2D {
    f32 minX{0.0f};
    f32 minY{0.0f};
    f32 maxX{0.0f};
    f32 maxY{0.0f};
    i16 forceVx_q8{0};
    i16 forceVy_q8{0};
    u16 dragScale_q8{256};
    u8 zoneFlags{0};
    u8 priority{0};
};
static_assert(sizeof(EnvironmentalZone2D) == 24);

// Cooked Room Static Collision Blob Header (magic 'ECOL' = 0x4C4F4345).
struct alignas(16) CookedCollisionMapHeader {
    static constexpr u32 kMagic = 0x4C4F4345u;
    static constexpr u16 kVersion = 1;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 chunkCountX{0};
    u16 chunkCountY{0};
    u16 tileSize_q8{256};
    u32 fineTileTableOffset{0};
    f32 originX{0.0f};
    f32 originY{0.0f};
    u32 zoneCount{0};
    u32 zoneTableOffset{0};
};
static_assert(sizeof(CookedCollisionMapHeader) == 32);

struct alignas(8) CookedCollisionChunk8x8 {
    u64 solidBitmask{0};
    u64 pureFullBlockBitmask{0};
    u32 fineTileBaseIndex{0};
    u32 reserved{0};
};
static_assert(sizeof(CookedCollisionChunk8x8) == 24);

struct CookedTileCell {
    TileShape shape{TileShape::Empty};
    TileMaterialFlags material{TileMaterialFlags::None};
};
static_assert(sizeof(CookedTileCell) == 2);

enum class FrameDirectiveKind : u8 {
    SetCancelMask = 0,
    SetOnHitCancelMask,
    SetHurtboxProfile,
    ActivateHitboxSlice,
    DeactivateHitbox,
    ApplyRootMotionDelta,
    ApplyVelocityImpulse,
    EmitSimCue
};

struct alignas(8) CookedFrameDirective {
    u8 clipId{0};
    u8 frameIndex{0};
    FrameDirectiveKind kind{FrameDirectiveKind::SetCancelMask};
    u8 paramU8{0};
    u16 paramU16{0};
    i16 paramI16{0};
};
static_assert(sizeof(CookedFrameDirective) == 8);

struct alignas(16) CookedHitboxSocket {
    CompactAabb2D localBox{};
    u16 damageMultiplier_q8{256};
    i16 knockbackAngleDeg{35};
    u8 knockbackSpeed_q2{48};
    u8 hitstopTicks{6};
    u8 priority{0};
    u8 socketFlags{0};
};
static_assert(sizeof(CookedHitboxSocket) == 16);

struct alignas(8) CookedHitboxSlice {
    CompactAabb2D boundingUnion{};
    u16 firstSocketIndex{0};
    u8 socketCount{0};
    u8 sliceFlags{0};
    u32 reserved{0};
};
static_assert(sizeof(CookedHitboxSlice) == 16);

// Cooked Animation Rig & Frame Directive Blob Header (magic 'ERIG' = 0x47495245).
struct alignas(16) CookedAnimRigHeader {
    static constexpr u32 kMagic = 0x47495245u;
    static constexpr u16 kVersion = 1;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 directiveCount{0};
    u16 hurtboxProfileCount{0};
    u16 hitboxSliceCount{0};
    u16 hitboxSocketCount{0};
    u16 reserved{0};
    u32 directiveOffset{0};
    u32 hurtboxProfileOffset{0};
    u32 hitboxSliceOffset{0};
    u32 hitboxSocketOffset{0};
};
static_assert(sizeof(CookedAnimRigHeader) == 32);

struct alignas(8) ResolvedHitEvent {
    world::EntityId attacker{world::kNullEntity};
    world::EntityId defender{world::kNullEntity};
    f32 contactX{0.0f};
    f32 contactY{0.0f};
    u16 damageMultiplier_q8{256};
    i16 knockbackAngleDeg{0};
    u8 knockbackSpeed_q2{0};
    u8 hitstopTicks{0};
    u8 socketFlags{0};
    u8 isClashParry{0};
};
static_assert(sizeof(ResolvedHitEvent) == 24);

class GameplayPipeline;

struct ActorPolicyContext {
    world::World& world;
    const assets::AssetBank& assetBank;
    time::SimStepView step;
    const platform::input::TickInput& tickInput;
    platform::input::ActionBuffer& actionBuffer;
    GameplayPipeline& pipeline;
    world::EntityId entity;
};

// Zero-virtual callback table separating Engine Mechanisms from Game Policies.
struct GameplayPolicyTable {
    using EvaluateActorFn = void (*)(
        const ActorPolicyContext& ctx,
        KinematicMotor2D& motor,
        MotionStack2D& motion,
        StateMachine2D& fsm,
        HitboxRig2D* optRig) noexcept;

    using ResolveCombatHitsFn = void (*)(
        world::World& world,
        time::SimStepView step,
        std::span<const ResolvedHitEvent> hits,
        GameplayPipeline& pipeline) noexcept;

    EvaluateActorFn evaluateActor{nullptr};
    ResolveCombatHitsFn resolveCombatHits{nullptr};
};

} // namespace en::gameplay
