#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "core/time/TimeModulator.h"
#include "input/InputTypes.h"
#include "assets/AssetTypes.h"
#include "world/WorldTypes.h"

#include <array>
#include <span>
#include <type_traits>

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

enum class HitboxRigFlags : u8 {
    None           = 0,
    HurtboxActive  = 1u << 0,
    HitboxActive   = 1u << 1,
    ClashParryable = 1u << 2,
    RecoilOnHit    = 1u << 3,
    SweptDashHull  = 1u << 4
};

enum class CameraCtrlFlags : u8 {
    None                 = 0,
    LockVerticalAirborne = 1u << 0,
    ClampToRoom          = 1u << 1,
    LockAxisX            = 1u << 2,
    LockAxisY            = 1u << 3,
    SoftBoundaryCushion  = 1u << 4,
    VelocityLookahead    = 1u << 5
};

template <typename E>
concept GameplayBitmaskEnum =
    std::same_as<E, ContactFlags> ||
    std::same_as<E, MotorFlags> ||
    std::same_as<E, CancelCategory> ||
    std::same_as<E, TileMaterialFlags> ||
    std::same_as<E, HitboxRigFlags> ||
    std::same_as<E, CameraCtrlFlags>;

template <GameplayBitmaskEnum E>
[[nodiscard]] constexpr E operator|(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
}
template <GameplayBitmaskEnum E>
[[nodiscard]] constexpr E operator&(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
}
template <GameplayBitmaskEnum E>
[[nodiscard]] constexpr E operator~(E a) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(~static_cast<U>(a));
}
template <GameplayBitmaskEnum E>
constexpr E& operator|=(E& a, E b) noexcept { return a = (a | b); }
template <GameplayBitmaskEnum E>
constexpr E& operator&=(E& a, E b) noexcept { return a = (a & b); }

template <GameplayBitmaskEnum E>
[[nodiscard]] constexpr bool HasFlag(E flags, E test) noexcept {
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(flags) & static_cast<U>(test)) != 0;
}
[[nodiscard]] constexpr bool HasContact(ContactFlags flags, ContactFlags test) noexcept { return HasFlag(flags, test); }
[[nodiscard]] constexpr bool HasMotorFlag(MotorFlags flags, MotorFlags test) noexcept { return HasFlag(flags, test); }

enum class SimCueKind : u8 {
    AudioOneShot = 0,
    SpawnPrefabEffect,
    CameraTraumaPulse,
    GamepadRumblePulse,
    CustomGameEvent
};

// Outgoing presentation cue emitted during 120 Hz simulation ticks.
struct alignas(32) SimCue {
    assets::AssetId assetId{};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    world::EntityId emitter{world::kNullEntity};
    u16 magnitude_q16{0};
    u16 durationTicks{0};
    SimCueKind kind{SimCueKind::AudioOneShot};
    u8 channel{0};
    u16 userTag{0};
};
static_assert(sizeof(SimCue) == 32);

// Branchless minimax polynomial sine/cosine pair on [-pi, pi] for deterministic combat knockback/recoil vectors.
constexpr void FastSinCos(f32 rad, f32& outCos, f32& outSin) noexcept {
    constexpr f32 kInvTwoPi = 0.159154943f;
    constexpr f32 kTwoPi    = 6.283185307f;
    constexpr f32 kHalfPi   = 1.570796327f;
    constexpr f32 kPi       = 3.141592654f;

    const f32 turns = rad * kInvTwoPi;
    i32 k = static_cast<i32>(turns);
    k -= (turns < static_cast<f32>(k));
    f32 x = rad - static_cast<f32>(k) * kTwoPi;
    if (x > kPi) x -= kTwoPi;

    f32 signCos = 1.0f;
    if (x > kHalfPi)       { x =  kPi - x; signCos = -1.0f; }
    else if (x < -kHalfPi) { x = -kPi - x; signCos = -1.0f; }

    const f32 x2 = x * x;
    outSin = x * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * 0.00019841f)));
    outCos = signCos * (1.0f - x2 * (0.5f - x2 * (0.04166667f - x2 * 0.00138889f)));
}

// Quantized 8-byte local AABB in Q8.8 signed meters (range [-128.0m, +127.996m], ~3.9mm precision).
struct CompactAabb2D {
    i16 offsetX_q8{0};
    i16 offsetY_q8{0};
    u16 halfW_q8{128};
    u16 halfH_q8{256};

    [[nodiscard]] static constexpr CompactAabb2D FromMeters(f32 ox, f32 oy, f32 hw, f32 hh) noexcept {
        return {static_cast<i16>(ox * 256.0f), static_cast<i16>(oy * 256.0f),
                static_cast<u16>(hw * 256.0f), static_cast<u16>(hh * 256.0f)};
    }
    [[nodiscard]] constexpr f32 OffsetX() const noexcept { return static_cast<f32>(offsetX_q8) * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 OffsetY() const noexcept { return static_cast<f32>(offsetY_q8) * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 HalfW()   const noexcept { return static_cast<f32>(halfW_q8)   * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 HalfH()   const noexcept { return static_cast<f32>(halfH_q8)   * (1.0f / 256.0f); }
};
static_assert(sizeof(CompactAabb2D) == 8);

// Authoritative swept-collision body, soft pushbox separator, and ledge/slope sensor.
// Invariant: bounds.halfW_q8 > 0 && bounds.halfH_q8 > 0; never attach world::Velocity2D to the same entity.
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

    [[nodiscard]] constexpr f32 SlopeNormalX() const noexcept { return static_cast<f32>(slopeNormalX_q14) * (1.0f / 16384.0f); }
    [[nodiscard]] constexpr f32 SlopeNormalY() const noexcept { return static_cast<f32>(slopeNormalY_q14) * (1.0f / 16384.0f); }
    constexpr void SetSlopeNormal(f32 nx, f32 ny) noexcept {
        slopeNormalX_q14 = static_cast<i16>(nx * 16384.0f);
        slopeNormalY_q14 = static_cast<i16>(ny * 16384.0f);
    }
};
static_assert(sizeof(KinematicMotor2D) == 32);

// Moving solid carrier (platforms, elevators, pistons) evaluated in Stage 1 before actor movement.
struct alignas(16) KinematicCarrier2D {
    static constexpr u8 kTypeId = 17;

    f32 deltaX{0.0f};
    f32 deltaY{0.0f};
    CompactAabb2D bounds{};
};
static_assert(sizeof(KinematicCarrier2D) == 16);

// Priority motion arbitration stack, animation root-motion accumulator, and forgiveness timers.
// packedSigns layout: Bit 0 = FacingSign, Bits 1..2 = WallSign, Bit 3 = CarrierLaunch, Bit 4 = FacingLocked.
// Invariant: overrideTicks == 0 implies activePriority == 0.
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

    [[nodiscard]] constexpr i8 FacingSign() const noexcept { return (packedSigns & 0x01) != 0 ? i8{1} : i8{-1}; }
    constexpr void SetFacingSign(i8 sign) noexcept {
        packedSigns = static_cast<i8>((packedSigns & ~0x01) | (sign >= 0 ? 0x01 : 0x00));
    }
    [[nodiscard]] constexpr i8 WallSign() const noexcept {
        const u8 raw = static_cast<u8>((packedSigns >> 1) & 0x03);
        return raw == 1u ? i8{1} : (raw == 2u ? i8{-1} : i8{0});
    }
    constexpr void SetWallSign(i8 sign) noexcept {
        packedSigns = static_cast<i8>((packedSigns & ~0x06) | (sign > 0 ? 0x02 : (sign < 0 ? 0x04 : 0x00)));
    }
    [[nodiscard]] constexpr bool IsCarrierLaunchActive() const noexcept { return (packedSigns & 0x08) != 0; }
    constexpr void SetCarrierLaunchActive(bool active) noexcept {
        packedSigns = static_cast<i8>((packedSigns & ~0x08) | (active ? 0x08 : 0x00));
    }
    [[nodiscard]] constexpr bool IsFacingLocked() const noexcept { return (packedSigns & 0x10) != 0; }
    constexpr void SetFacingLocked(bool locked) noexcept {
        packedSigns = static_cast<i8>((packedSigns & ~0x10) | (locked ? 0x10 : 0x00));
    }
    constexpr bool PushOverride(u8 priority, f32 vx, f32 vy, u16 durationTicks, u16 gravScaleQ8 = 256) noexcept {
        if (priority < activePriority && overrideTicks > 0) return false;
        activePriority  = priority;
        overrideVx      = vx;
        overrideVy      = vy;
        overrideTicks   = durationTicks;
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
    constexpr void PromoteCancelWindowOnHit() noexcept { cancelMask |= onHitCancelMask; }
    constexpr void TransitionTo(u8 nextState, u8 nextClipId, u16 initialCancelMask = 0) noexcept {
        prevStateId     = stateId;
        stateId         = nextState;
        stateTicks      = 0;
        cancelMask      = initialCancelMask;
        onHitCancelMask = 0;
        if (animClipId != nextClipId) {
            animClipId    = nextClipId;
            lastAnimFrame = 0xFFFFu;
        }
    }
};
static_assert(sizeof(StateMachine2D) == 16);

// Multi-socket hitbox/hurtbox rig with 64-bit SplitMix Bloom Filter swing deduplication.
struct alignas(32) HitboxRig2D {
    static constexpr u8 kTypeId = 20;

    u64 hitBloomMask{0};
    CompactAabb2D hurtbox{};
    CompactAabb2D primaryHitbox{};
    u16 hitLayerMask{0};
    u16 hurtLayerMask{0};
    u16 swingId{0};
    u8 activeSliceIndex{0xFFu};
    u8 rigFlags{static_cast<u8>(HitboxRigFlags::HurtboxActive)};

    constexpr void ActivateSlice(u8 sliceIdx, CompactAabb2D unionBox, bool newSwing) noexcept {
        if (newSwing) { ++swingId; hitBloomMask = 0; }
        activeSliceIndex = sliceIdx;
        primaryHitbox    = unionBox;
        rigFlags        |= static_cast<u8>(HitboxRigFlags::HitboxActive);
    }
    constexpr void DeactivateHitbox() noexcept {
        activeSliceIndex = 0xFFu;
        rigFlags        &= ~static_cast<u8>(HitboxRigFlags::HitboxActive);
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
    constexpr void MarkHit(world::EntityId target) noexcept { hitBloomMask |= ComputeEntityBloomBits(target); }
};
static_assert(sizeof(HitboxRig2D) == 32);

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

struct alignas(16) SafeGroundBreadcrumb2D {
    static constexpr u8 kTypeId = 22;

    f32 safeX{0.0f};
    f32 safeY{0.0f};
    f32 prevSafeX{0.0f};
    f32 prevSafeY{0.0f};
};
static_assert(sizeof(SafeGroundBreadcrumb2D) == 16);

// 64B 1-cache-line 2D Camera Director (Dual-Zone Focus Box, Turn-Confirmed Lookahead, Soft Room Cushion,
// and unified 2nd-Order Spring Kick/Trauma Displacement).
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
    u8 roomBlendDuration{36};
    u8 sustainedMoveTicks{0};
    u8 secondaryWeight_q8{0};
    u8 deadzoneHalfW_q4{18};
    u8 deadzoneHalfH_q4{28};
    u8 springFreqX_q4{88};
    u8 springFreqY_q4{104};
    u8 cameraFlags{static_cast<u8>(
        CameraCtrlFlags::LockVerticalAirborne | CameraCtrlFlags::ClampToRoom |
        CameraCtrlFlags::SoftBoundaryCushion | CameraCtrlFlags::VelocityLookahead)};

    constexpr void TransitionToRoom(CompactAabb2D newBounds, u8 blendTicks = 36) noexcept {
        prevRoomBounds    = activeRoomBounds;
        activeRoomBounds  = newBounds;
        roomBlendDuration = blendTicks > 0 ? blendTicks : u8{1};
        roomBlendTicks    = blendTicks;
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

struct alignas(8) EnvironmentalZone2D {
    f32 minX{0.0f}, minY{0.0f}, maxX{0.0f}, maxY{0.0f};
    i16 forceVx_q8{0}, forceVy_q8{0};
    u16 dragScale_q8{256};
    u8 zoneFlags{0}, priority{0};
};
static_assert(sizeof(EnvironmentalZone2D) == 24);

struct alignas(16) CookedCollisionMapHeader {
    static constexpr u32 kMagic   = 0x4C4F4345u;
    static constexpr u16 kVersion = 1;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 chunkCountX{0}, chunkCountY{0};
    u16 tileSize_q8{256};
    u32 fineTileTableOffset{0};
    f32 originX{0.0f}, originY{0.0f};
    u32 zoneCount{0}, zoneTableOffset{0};
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
    u8 clipId{0}, frameIndex{0};
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
    u8 knockbackSpeed_q2{48}, hitstopTicks{6}, priority{0}, socketFlags{0};
};
static_assert(sizeof(CookedHitboxSocket) == 16);

struct alignas(8) CookedHitboxSlice {
    CompactAabb2D boundingUnion{};
    u16 firstSocketIndex{0};
    u8 socketCount{0}, sliceFlags{0};
    u32 reserved{0};
};
static_assert(sizeof(CookedHitboxSlice) == 16);

struct alignas(16) CookedAnimRigHeader {
    static constexpr u32 kMagic   = 0x47495245u;
    static constexpr u16 kVersion = 1;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 directiveCount{0}, hurtboxProfileCount{0}, hitboxSliceCount{0}, hitboxSocketCount{0}, reserved{0};
    u32 directiveOffset{0}, hurtboxProfileOffset{0}, hitboxSliceOffset{0}, hitboxSocketOffset{0};
};
static_assert(sizeof(CookedAnimRigHeader) == 32);

struct alignas(8) ResolvedHitEvent {
    world::EntityId attacker{world::kNullEntity};
    world::EntityId defender{world::kNullEntity};
    f32 contactX{0.0f}, contactY{0.0f};
    u16 damageMultiplier_q8{256};
    i16 knockbackAngleDeg{0};
    u8 knockbackSpeed_q2{0}, hitstopTicks{0}, socketFlags{0}, isClashParry{0};
};
static_assert(sizeof(ResolvedHitEvent) == 24);

class GameplayPipeline;

struct ActorPolicyContext {
    world::World& world;
    const assets::AssetBank& assetBank;
    time::SimStepView step;
    const input::TickInput& tickInput;
    input::ActionBuffer& actionBuffer;
    GameplayPipeline& pipeline;
    world::EntityId entity;
};

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
