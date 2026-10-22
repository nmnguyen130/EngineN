#pragma once

#include "assets/AssetTypes.h"
#include "core/Math.h"
#include "core/Types.h"
#include "input/ActionBuffer.h"
#include "physics/PhysicsTypes.h"
#include "time/TimeTypes.h"
#include "world/WorldTypes.h"

#include <type_traits>

namespace en {

class World;
class AssetBank;
class PhysicsSystem;
class GameplayPipeline;

enum class GameplayErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    CorruptedRigBlob,
    PoolRegistrationFailed,
};

enum class SimCueKind : u8 {
    AudioOneShot      = 0,
    SpawnPrefabEffect = 1,
    CameraTraumaPulse = 2,
    GamepadRumble     = 3,
    HitstopFreeze     = 4,
    CustomGameEvent   = 5,
};

enum class CancelCategory : u8 {
    None    = 0,
    Dash    = 1U << 0U,
    Jump    = 1U << 1U,
    Light   = 1U << 2U,
    Heavy   = 1U << 3U,
    Special = 1U << 4U,
    Parry   = 1U << 5U,
    Skill   = 1U << 6U,
    Any     = 0xFFU,
};
consteval bool enable_bitmask_operators(CancelCategory) noexcept { return true; }

enum class MotionFlags : u8 {
    None          = 0,
    FacingLeft    = 1U << 0U,
    LockFacing    = 1U << 1U,
    AirborneDash  = 1U << 2U,
    JumpHeld      = 1U << 3U,
    WallSliding   = 1U << 4U,
    JumpCutApplied = 1U << 5U,
};
consteval bool enable_bitmask_operators(MotionFlags) noexcept { return true; }

enum class HitboxFlags : u8 {
    None           = 0,
    HitboxActive   = 1U << 0U,
    HurtboxActive  = 1U << 1U,
    CanClash       = 1U << 2U,
    Unblockable    = 1U << 3U,
    FlipWithFacing = 1U << 4U,
};
consteval bool enable_bitmask_operators(HitboxFlags) noexcept { return true; }

enum class ReceiverFlags : u8 {
    None         = 0,
    Invulnerable = 1U << 0U,
    SuperArmor   = 1U << 1U,
    Parrying     = 1U << 2U,
    Dead         = 1U << 3U,
};
consteval bool enable_bitmask_operators(ReceiverFlags) noexcept { return true; }

enum class HitOutcome : u8 {
    None          = 0,
    Damaged       = 1,
    ArmorAbsorbed = 2,
    Parried       = 3,
    Clashed       = 4,
};

enum class CameraCtrlFlags : u8 {
    None            = 0,
    EnableLookahead = 1U << 0U,
    LatchGroundY    = 1U << 1U,
    ConfineToRoom   = 1U << 2U,
    SnapNextStep    = 1U << 3U,
};
consteval bool enable_bitmask_operators(CameraCtrlFlags) noexcept { return true; }

enum class FrameDirectiveOp : u8 {
    None             = 0,
    EmitAudioCue     = 1,
    EmitVfxCue       = 2,
    ActivateHitbox   = 3,
    DeactivateHitbox = 4,
    SetCancelMask    = 5,
    AddRootMotion    = 6,
    SetInvulnerable  = 7,
};

// 32-byte (2 per 64B cache line, zero padding) lockless simulation output cue record.
struct alignas(32) SimCue {
    AssetId assetId{};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    EntityId emitter{kNullEntity};
    u16 magnitude_q16{65535U};
    u16 durationTicks{0U};
    SimCueKind kind{SimCueKind::AudioOneShot};
    u8 channel{0U};
    u16 userTag{0U};

    [[nodiscard]] constexpr f32 Magnitude() const noexcept {
        return static_cast<f32>(magnitude_q16) * (1.0f / 65535.0f);
    }
    constexpr void SetMagnitude(f32 mag) noexcept {
        const f32 c = std::clamp(mag, 0.0f, 1.0f);
        magnitude_q16 = static_cast<u16>(c * 65535.0f + 0.5f);
    }
};
static_assert(sizeof(SimCue) == 32);

// 32-byte (2 per 64B cache line, zero padding) priority velocity stack, coyote/wall-jump controller, and soft pushbox component.
// Separates accelerated walk velocity (currentWalkVx) from decaying horizontal knockback (impulseVx) and one-shot vertical launches (pendingLaunchVy).
struct alignas(32) MotionStack2D {
    static constexpr u8 kTypeId = 16;

    f32 targetWalkVx{0.0f};
    f32 currentWalkVx{0.0f};
    f32 impulseVx{0.0f};
    f32 pendingLaunchVy{0.0f};
    i16 overrideVx_q6{0};
    i16 overrideVy_q6{0};
    i16 rootMotionDx_q8{0};
    i16 rootMotionDy_q8{0};
    u8 overrideTicks{0U};
    u8 activePriority{0U};
    u8 groundCoyoteTicks{0U};
    u8 wallCoyoteTicks{0U};
    u8 oneWayDropTicks{0U};
    u8 pushboxGroup{1U};
    MotionFlags flags{MotionFlags::None};
    i8 wallContactSign{0}; // -1: wall on left, +1: wall on right, 0: none

    [[nodiscard]] static constexpr i16 QuantizeQ6(f32 v) noexcept {
        const f32 scaled = std::clamp(v, -512.0f, 511.98f) * 64.0f;
        return static_cast<i16>(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
    }

    [[nodiscard]] constexpr i8 FacingSign() const noexcept {
        return HasPhysicsFlag(flags, MotionFlags::FacingLeft) ? -1 : 1;
    }

    constexpr void SetFacingSign(i8 dirSign) noexcept {
        if (HasPhysicsFlag(flags, MotionFlags::LockFacing) || dirSign == 0) return;
        if (dirSign < 0) flags |= MotionFlags::FacingLeft;
        else             flags &= ~MotionFlags::FacingLeft;
    }

    [[nodiscard]] constexpr f32 OverrideVx() const noexcept { return static_cast<f32>(overrideVx_q6) * (1.0f / 64.0f); }
    [[nodiscard]] constexpr f32 OverrideVy() const noexcept { return static_cast<f32>(overrideVy_q6) * (1.0f / 64.0f); }

    constexpr bool RequestVelocityOverride(f32 vx, f32 vy, u8 ticks, u8 priority) noexcept {
        if (overrideTicks > 0U && priority < activePriority) return false;
        overrideVx_q6 = QuantizeQ6(vx);
        overrideVy_q6 = QuantizeQ6(vy);
        overrideTicks = ticks;
        activePriority = priority;
        return true;
    }

    constexpr void AddKnockbackImpulse(f32 kx, f32 ky) noexcept {
        impulseVx += kx;
        if (std::abs(ky) > std::abs(pendingLaunchVy)) {
            pendingLaunchVy = ky;
        }
    }

    // Executes a ground coyote jump or directional wall jump if within grace window.
    constexpr bool TryExecuteJump(f32 jumpSpeedY, f32 wallJumpSpeedX = 0.0f) noexcept {
        if (groundCoyoteTicks > 0U) {
            groundCoyoteTicks = 0U;
            pendingLaunchVy = jumpSpeedY;
            flags |= MotionFlags::JumpHeld;
            flags &= ~MotionFlags::JumpCutApplied;
            return true;
        }
        if (wallCoyoteTicks > 0U && wallContactSign != 0 && wallJumpSpeedX > 0.0f) {
            const f32 awayDir = static_cast<f32>(-wallContactSign);
            wallCoyoteTicks = 0U;
            currentWalkVx = awayDir * wallJumpSpeedX;
            pendingLaunchVy = jumpSpeedY;
            SetFacingSign(static_cast<i8>(-wallContactSign));
            flags |= MotionFlags::JumpHeld;
            flags &= ~MotionFlags::JumpCutApplied;
            return true;
        }
        return false;
    }

    constexpr void AddRootMotionMeters(f32 dx, f32 dy) noexcept {
        rootMotionDx_q8 = CompactAabb2D::QuantizeS16(RootMotionDx() + dx);
        rootMotionDy_q8 = CompactAabb2D::QuantizeS16(RootMotionDy() + dy);
    }

    [[nodiscard]] constexpr f32 RootMotionDx() const noexcept {
        return static_cast<f32>(rootMotionDx_q8) * (1.0f / 256.0f);
    }
    [[nodiscard]] constexpr f32 RootMotionDy() const noexcept {
        return static_cast<f32>(rootMotionDy_q8) * (1.0f / 256.0f);
    }
};
static_assert(sizeof(MotionStack2D) == 32);

// 16-byte (4 per 64B cache line, zero padding) data-driven finite state machine, per-entity hitstop, and action buffer component.
struct alignas(16) StateMachine2D {
    static constexpr u8 kTypeId = 17;

    BlobHandle rigBlob{};
    u16 stateTicks{0U};
    u16 lastAnimFrame{0xFFFFU};
    CancelCategory cancelMask{CancelCategory::None};
    CancelCategory onHitCancelMask{CancelCategory::None};
    u8 stateId{0U};
    u8 prevStateId{0U};
    u8 hitstopTicks{0U};
    u8 bufferedActionId{0xFFU};
    u8 bufferedActionTicks{0U};
    u8 flags{1U}; // bit 0: stateJustEntered, bit 1: confirmedHitThisState

    constexpr void TransitionTo(u8 nextState, CancelCategory initialCancels = CancelCategory::None) noexcept {
        prevStateId = stateId;
        stateId = nextState;
        stateTicks = 0U;
        lastAnimFrame = 0xFFFFU;
        cancelMask = initialCancels;
        onHitCancelMask = CancelCategory::None;
        flags = 1U;
    }

    [[nodiscard]] constexpr bool CanCancelInto(CancelCategory cat) const noexcept {
        const CancelCategory effective =
            ((flags & 2U) != 0U) ? (cancelMask | onHitCancelMask) : cancelMask;
        return HasPhysicsFlag(effective, cat);
    }

    constexpr void PromoteOnHitCancels() noexcept {
        flags |= 2U;
        cancelMask |= onHitCancelMask;
    }

    constexpr void BufferAction(u8 actionId, u8 windowTicks = 8U) noexcept {
        bufferedActionId = actionId;
        bufferedActionTicks = windowTicks;
    }

    [[nodiscard]] constexpr bool ConsumeBufferedAction(u8 actionId) noexcept {
        if (bufferedActionTicks > 0U && bufferedActionId == actionId) {
            bufferedActionId = 0xFFU;
            bufferedActionTicks = 0U;
            return true;
        }
        return false;
    }
};
static_assert(sizeof(StateMachine2D) == 16);

// 32-byte (2 per 64B cache line, zero padding) combat hitbox/hurtbox rig with 64-bit SplitMix swing Bloom filter.
struct alignas(32) HitboxRig2D {
    static constexpr u8 kTypeId = 18;

    u64 hitBloomMask{0ULL};
    CompactAabb2D hurtbox{};
    CompactAabb2D hitbox{};
    i16 baseDamage{10};
    i8 knockbackX_q2{20};   // 5.0 m/s in Q5.2
    i8 knockbackY_q2{12};   // 3.0 m/s in Q5.2
    u8 hitLayerMask{0x02U};
    u8 poiseDamage{10U};
    // packedMeta layout: bits [0..1] = swingId (0..3), bits [2..3] = hurtLayer (0..3),
    //                    bits [4..5] = priority (0..3), bits [6..7] = hitstopTicks >> 1 (0, 2, 4, 6 ticks)
    u8 packedMeta{0x90U};
    HitboxFlags flags{HitboxFlags::HurtboxActive | HitboxFlags::FlipWithFacing};

    [[nodiscard]] constexpr u8 SwingId() const noexcept { return packedMeta & 0x03U; }
    [[nodiscard]] constexpr u8 HurtLayer() const noexcept { return (packedMeta >> 2U) & 0x03U; }
    [[nodiscard]] constexpr u8 Priority() const noexcept { return (packedMeta >> 4U) & 0x03U; }
    [[nodiscard]] constexpr u8 HitstopTicks() const noexcept { return static_cast<u8>(((packedMeta >> 6U) & 0x03U) * 2U); }

    constexpr void SetHurtLayer(u8 layer) noexcept {
        packedMeta = static_cast<u8>((packedMeta & ~0x0CU) | ((layer & 0x03U) << 2U));
    }
    constexpr void SetPriority(u8 prio) noexcept {
        packedMeta = static_cast<u8>((packedMeta & ~0x30U) | ((prio & 0x03U) << 4U));
    }
    constexpr void SetHitstopTicks(u8 ticks) noexcept {
        const u8 code = static_cast<u8>(std::min<u32>(3U, (static_cast<u32>(ticks) + 1U) >> 1U));
        packedMeta = static_cast<u8>((packedMeta & ~0xC0U) | (code << 6U));
    }

    [[nodiscard]] static constexpr u64 ComputeDefenderBloomBits(u8 swing, EntityId defender) noexcept {
        u64 z = (static_cast<u64>(defender.Packed()) << 8U) | static_cast<u64>(swing);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31U);
        const u32 b0 = static_cast<u32>(z & 63ULL);
        const u32 b1 = static_cast<u32>((z >> 6U) & 63ULL);
        return (1ULL << b0) | (1ULL << b1);
    }

    constexpr void BeginNewSwing(
        const CompactAabb2D& box,
        i16 damage,
        u8 poiseDmg = 10U,
        f32 kbX = 5.0f,
        f32 kbY = 3.0f,
        u8 prio = 1U,
        u8 hitstop = 4U) noexcept {
        const u8 nextSwing = static_cast<u8>((SwingId() + 1U) & 0x03U);
        const u8 hsCode = static_cast<u8>(std::min<u32>(3U, (static_cast<u32>(hitstop) + 1U) >> 1U));
        packedMeta = static_cast<u8>(
            (packedMeta & 0x0CU) | nextSwing | ((prio & 0x03U) << 4U) | (hsCode << 6U));
        hitBloomMask = 0ULL;
        hitbox = box;
        baseDamage = damage;
        poiseDamage = poiseDmg;
        knockbackX_q2 = static_cast<i8>(std::clamp(kbX * 4.0f, -128.0f, 127.0f));
        knockbackY_q2 = static_cast<i8>(std::clamp(kbY * 4.0f, -128.0f, 127.0f));
        flags |= HitboxFlags::HitboxActive;
    }

    constexpr void EndSwing() noexcept {
        flags &= ~HitboxFlags::HitboxActive;
    }

    [[nodiscard]] constexpr bool HasAlreadyHit(EntityId defender) const noexcept {
        const u64 bits = ComputeDefenderBloomBits(SwingId(), defender);
        return (hitBloomMask & bits) == bits;
    }

    constexpr void RecordDefenderHit(EntityId defender) noexcept {
        hitBloomMask |= ComputeDefenderBloomBits(SwingId(), defender);
    }

    [[nodiscard]] constexpr f32 KnockbackX() const noexcept { return static_cast<f32>(knockbackX_q2) * 0.25f; }
    [[nodiscard]] constexpr f32 KnockbackY() const noexcept { return static_cast<f32>(knockbackY_q2) * 0.25f; }
};
static_assert(sizeof(HitboxRig2D) == 32);

// 16-byte (4 per 64B cache line, zero padding) combat health, poise, directional parry, and auto-iframe component.
struct alignas(16) DamageReceiver2D {
    static constexpr u8 kTypeId = 19;

    i32 hp{100};
    i32 maxHp{100};
    u16 iframeTicks{0U};
    u16 stunTicks{0U};
    u8 poise{20U};
    u8 maxPoise{20U};
    u8 autoIframeTicks{10U};
    u8 packedTeamAndFlags{0U}; // low 4 bits: teamId (0..15), high 4 bits: ReceiverFlags

    [[nodiscard]] constexpr u8 TeamId() const noexcept { return packedTeamAndFlags & 0x0FU; }
    constexpr void SetTeamId(u8 team) noexcept {
        packedTeamAndFlags = static_cast<u8>((packedTeamAndFlags & 0xF0U) | (team & 0x0FU));
    }

    [[nodiscard]] constexpr ReceiverFlags Flags() const noexcept {
        return static_cast<ReceiverFlags>((packedTeamAndFlags >> 4U) & 0x0FU);
    }
    constexpr void SetFlags(ReceiverFlags f) noexcept {
        packedTeamAndFlags = static_cast<u8>((packedTeamAndFlags & 0x0FU) | ((static_cast<u8>(f) & 0x0FU) << 4U));
    }
    constexpr void AddFlags(ReceiverFlags f) noexcept { SetFlags(Flags() | f); }
    constexpr void RemoveFlags(ReceiverFlags f) noexcept { SetFlags(Flags() & ~f); }
    [[nodiscard]] constexpr bool HasFlag(ReceiverFlags f) const noexcept { return HasPhysicsFlag(Flags(), f); }
};
static_assert(sizeof(DamageReceiver2D) == 16);

// 16-byte (4 per 64B cache line, zero padding) 3-ray verified safe ground respawn anchor component.
struct alignas(16) SafeAnchor2D {
    static constexpr u8 kTypeId = 20;

    f32 safeX{0.0f};
    f32 safeY{0.0f};
    f32 prevSafeX{0.0f};
    f32 prevSafeY{0.0f};
};
static_assert(sizeof(SafeAnchor2D) == 16);

// 64-byte (1 cache line exact, zero padding) 2nd-order critically damped spring 2D camera controller component.
struct alignas(64) CameraController2D {
    static constexpr u8 kTypeId = 21;

    EntityId primaryTarget{kNullEntity};
    EntityId secondaryTarget{kNullEntity};
    f32 anchorX{0.0f};
    f32 anchorY{0.0f};
    f32 velX{0.0f};
    f32 velY{0.0f};
    f32 latchedGroundY{0.0f};
    f32 kickOffsetX{0.0f};
    f32 kickOffsetY{0.0f};
    Rect activeRoomBounds{-1000.0f, -1000.0f, 2000.0f, 2000.0f};
    i16 kickVelX_q4{0};
    i16 kickVelY_q4{0};
    i16 lookaheadX_q8{0};
    u16 trauma_q16{0U};
    u8 secondaryWeight_q8{0U};  // [0..255] -> [0.0..1.0] blend toward secondaryTarget
    u8 deadzoneHalfW_q4{16U};   // 1.0m in Q4.4
    u8 deadzoneHalfH_q4{12U};   // 0.75m in Q4.4
    CameraCtrlFlags flags{CameraCtrlFlags::EnableLookahead | CameraCtrlFlags::ConfineToRoom | CameraCtrlFlags::SnapNextStep};

    constexpr void AddTrauma(f32 amount) noexcept {
        const f32 cur = static_cast<f32>(trauma_q16) * (1.0f / 65535.0f);
        const f32 next = std::clamp(cur + amount, 0.0f, 1.0f);
        trauma_q16 = static_cast<u16>(next * 65535.0f + 0.5f);
    }

    constexpr void ApplyDirectionalKick(f32 impulseX, f32 impulseY) noexcept {
        SetKickVel(KickVelX() + impulseX, KickVelY() + impulseY);
    }

    [[nodiscard]] constexpr f32 KickVelX() const noexcept { return static_cast<f32>(kickVelX_q4) * (1.0f / 16.0f); }
    [[nodiscard]] constexpr f32 KickVelY() const noexcept { return static_cast<f32>(kickVelY_q4) * (1.0f / 16.0f); }
    constexpr void SetKickVel(f32 vx, f32 vy) noexcept {
        kickVelX_q4 = static_cast<i16>(std::clamp(vx * 16.0f, -32768.0f, 32767.0f));
        kickVelY_q4 = static_cast<i16>(std::clamp(vy * 16.0f, -32768.0f, 32767.0f));
    }

    [[nodiscard]] constexpr f32 Trauma() const noexcept {
        return static_cast<f32>(trauma_q16) * (1.0f / 65535.0f);
    }
    [[nodiscard]] constexpr f32 DeadzoneHalfW() const noexcept {
        return static_cast<f32>(deadzoneHalfW_q4) * (1.0f / 16.0f);
    }
    [[nodiscard]] constexpr f32 DeadzoneHalfH() const noexcept {
        return static_cast<f32>(deadzoneHalfH_q4) * (1.0f / 16.0f);
    }
};
static_assert(sizeof(CameraController2D) == 64);

// 32-byte (2 per 64B cache line, zero padding) resolved combat impact event record.
struct alignas(32) CombatHitEvent {
    EntityId attacker{kNullEntity};
    EntityId defender{kNullEntity};
    f32 hitPointX{0.0f};
    f32 hitPointY{0.0f};
    f32 knockbackVx{0.0f};
    f32 knockbackVy{0.0f};
    i16 damageApplied{0};
    u16 attackTag{0U};
    u8 hitstopTicks{0U};
    HitOutcome outcome{HitOutcome::None};
    u8 defenderDied{0U};
    u8 poiseBroken{0U};
};
static_assert(sizeof(CombatHitEvent) == 32);

// 16-byte cooked gameplay rig binary header ("ERIG" v1).
struct alignas(16) CookedGameplayRigHeader {
    static constexpr u32 kMagic = 0x47495245U; // "ERIG"
    static constexpr u16 kVersion = 1U;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 stateCount{0U};
    u16 directiveCount{0U};
    u16 flags{0U};
    u32 checksum{0U};
};
static_assert(sizeof(CookedGameplayRigHeader) == 16);

// 16-byte state descriptor inside a cooked "ERIG" table with automatic SpriteAnimator2D clip synchronization.
struct alignas(8) CookedStateEntry {
    u16 durationTicks{0U};      // 0 = infinite/looping until explicit transition
    u16 firstDirective{0U};
    u16 animFirstFrame{0U};
    u16 animFrameCount{0U};     // > 0 automatically configures SpriteAnimator2D on state entry
    u8 animTicksPerFrame{6U};
    u8 animLoop{1U};
    u8 directiveCount{0U};
    u8 nextStateId{0U};
    CancelCategory defaultCancels{CancelCategory::None};
    CancelCategory onHitCancels{CancelCategory::None};
    u16 stateTag{0U};
};
static_assert(sizeof(CookedStateEntry) == 16);

// 16-byte frame-accurate animation timeline directive inside a cooked "ERIG" table.
struct alignas(8) CookedFrameDirective {
    CompactAabb2D boxOrMotion{};
    u32 assetHashOrParam{0U};
    u16 triggerFrame{0U};
    FrameDirectiveOp op{FrameDirectiveOp::None};
    u8 auxByte{0U};
};
static_assert(sizeof(CookedFrameDirective) == 16);

// 24-byte (zero padding) function-pointer callback table for client gameplay logic.
struct alignas(8) GameplayPolicyTable {
    void (*onStepEntityState)(
        EntityId entity,
        StateMachine2D& fsm,
        World& world,
        const TickInput& input,
        ActionBuffer& actions,
        GameplayPipeline& pipeline) noexcept{nullptr};

    void (*onCombatHit)(
        const CombatHitEvent& hit,
        World& world,
        GameplayPipeline& pipeline) noexcept{nullptr};

    void (*onStateTransition)(
        EntityId entity,
        u8 fromState,
        u8 toState,
        World& world,
        GameplayPipeline& pipeline) noexcept{nullptr};
};
static_assert(sizeof(GameplayPolicyTable) == 24);

// 32-byte (2 per 64B cache line, zero padding) gameplay pipeline configuration.
struct alignas(16) GameplayConfig {
    f32 groundAccel{65.0f};
    f32 airAccel{40.0f};
    f32 knockbackDecayRate{12.0f};
    f32 pushboxSeparationSpeed{4.0f};
    f32 lookaheadDistance{2.5f};
    f32 jumpCutMultiplier{0.45f};   // Velocity multiplier when jump is released early while ascending
    u16 maxCuesPerTick{256U};
    u16 maxHitEventsPerTick{128U};
    u32 arenaBytes{512U * 1024U};
};
static_assert(sizeof(GameplayConfig) == 32);

} // namespace en
