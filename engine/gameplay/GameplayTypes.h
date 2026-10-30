#pragma once

#include "assets/AssetTypes.h"
#include "input/ActionBuffer.h"
#include "physics/PhysicsTypes.h"

namespace en {

class World;
class GameplaySystem;

enum class GameplayErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    PoolRegistrationFailed,
};

enum class CueKind : u8 {
    Audio       = 0,
    Effect      = 1,
    CameraShake = 2,
    Hitstop     = 3,
    Rumble      = 4,
    Custom      = 5,
};

enum class CharFlags : u8 {
    None           = 0,
    JumpHeld       = 1U << 0,
    JumpCutApplied = 1U << 1,
    WallSliding    = 1U << 2,
    Dashing        = 1U << 3,
    LockFacing     = 1U << 4,
    JumpRequested  = 1U << 5,
    DashRequested  = 1U << 6,
};
consteval bool enable_bitmask_operators(CharFlags) noexcept { return true; }

enum class CombatFlags : u8 {
    None           = 0,
    HurtboxActive  = 1U << 0,
    HitboxActive   = 1U << 1,
    CanClash       = 1U << 2,
    FlipWithFacing = 1U << 3,
};
consteval bool enable_bitmask_operators(CombatFlags) noexcept { return true; }

enum class HealthFlags : u8 {
    None         = 0,
    Invulnerable = 1U << 0,
    SuperArmor   = 1U << 1,
    Parrying     = 1U << 2,
    Dead         = 1U << 3,
};
consteval bool enable_bitmask_operators(HealthFlags) noexcept { return true; }

enum class HitOutcome : u8 {
    None          = 0,
    Damaged       = 1,
    ArmorAbsorbed = 2,
    Parried       = 3,
    Clashed       = 4,
};

enum class CameraFlags : u8 {
    None            = 0,
    EnableLookahead = 1U << 0,
    LatchGroundY    = 1U << 1,
    ConfineToRoom   = 1U << 2,
    SnapNextStep    = 1U << 3,
};
consteval bool enable_bitmask_operators(CameraFlags) noexcept { return true; }

// 32-byte simulation cue emitted by gameplay systems for audio, VFX, hitstop, or camera feedback.
struct GameplayCue {
    AssetId     asset{};
    Vec2        position{};
    EntityId    emitter{kNullEntity};
    f32         magnitude{1.0f};
    u16         durationTicks{0};
    u16         userTag{0};
    CueKind     kind{CueKind::Audio};
    TimeChannel channel{TimeChannel::World};
    u16         pad{0};
};
static_assert(sizeof(GameplayCue) == 32);

// 48-byte 2D Metroidvania character controller supporting coyote time, variable jump, wall slide/jump, and dash.
// Value type paired with PhysicsBody2D and Collider2D.
struct CharacterController2D {
    static constexpr u8 kTypeId = 10;

    f32       moveInputX{0.0f};
    f32       maxRunSpeed{8.0f};
    f32       jumpSpeed{14.0f};
    f32       dashSpeed{20.0f};
    f32       wallSlideSpeed{3.5f};
    f32       wallJumpSpeedX{10.0f};
    f32       currentWalkVx{0.0f};
    Vec2      knockbackVel{0.0f, 0.0f};
    u8        coyoteTicks{0};
    u8        wallCoyoteTicks{0};
    u8        dashTicks{0};
    u8        dashCooldownTicks{0};
    u8        airDashesLeft{1};
    u8        maxAirDashes{1};
    u8        airJumpsLeft{0};
    u8        maxAirJumps{0};
    u8        oneWayDropTicks{0};
    i8        facing{1};
    i8        wallDir{0};
    CharFlags flags{CharFlags::None};

    constexpr void SetFacing(i8 dir) noexcept {
        if (!HasFlag(flags, CharFlags::LockFacing) && dir != 0) {
            facing = (dir < 0) ? static_cast<i8>(-1) : static_cast<i8>(1);
        }
    }

    constexpr void RequestJump(bool held = true) noexcept {
        flags |= CharFlags::JumpRequested;
        if (held) flags |= CharFlags::JumpHeld;
    }

    constexpr void SetJumpHeld(bool held) noexcept {
        if (held) flags |= CharFlags::JumpHeld;
        else flags &= ~CharFlags::JumpHeld;
    }

    constexpr void RequestDash() noexcept {
        flags |= CharFlags::DashRequested;
    }

    constexpr void DropOneWay(u8 ticks = 10U) noexcept {
        oneWayDropTicks = ticks;
    }

    constexpr void ApplyKnockback(Vec2 impulse) noexcept {
        knockbackVel += impulse;
    }

    [[nodiscard]] constexpr bool IsDashing() const noexcept { return dashTicks > 0U; }
    [[nodiscard]] constexpr bool IsWallSliding() const noexcept {
        return HasFlag(flags, CharFlags::WallSliding);
    }
};
static_assert(sizeof(CharacterController2D) == 48);

// 16-byte entity finite state machine with timed auto-transitions, cancel masks, and hitstop freeze.
struct StateMachine2D {
    static constexpr u8 kTypeId = 11;

    u16 stateTicks{0};
    u16 durationTicks{0};
    u8  state{0};
    u8  prevState{0};
    u8  nextState{0};
    u8  hitstopTicks{0};
    u8  cancelMask{0xFF};
    u8  bufferedAction{0xFF};
    u8  bufferTicks{0};
    u8  entered{1};
    u32 userFlags{0};

    constexpr void TransitionTo(
        u8 targetState,
        u16 duration = 0U,
        u8 autoNextState = 0U,
        u8 cancels = 0xFFU) noexcept {
        prevState     = state;
        state         = targetState;
        nextState     = autoNextState;
        stateTicks    = 0U;
        durationTicks = duration;
        cancelMask    = cancels;
        entered       = 1U;
    }

    [[nodiscard]] constexpr bool CanCancel(u8 categoryMask) const noexcept {
        return (cancelMask & categoryMask) != 0U;
    }

    constexpr void BufferAction(u8 action, u8 windowTicks = 8U) noexcept {
        bufferedAction = action;
        bufferTicks    = windowTicks;
    }

    [[nodiscard]] constexpr bool ConsumeBuffered(u8 action) noexcept {
        if (bufferTicks > 0U && bufferedAction == action) {
            bufferedAction = 0xFFU;
            bufferTicks    = 0U;
            return true;
        }
        return false;
    }
};
static_assert(sizeof(StateMachine2D) == 16);

// 16-byte health, poise, invulnerability frame, and team affiliation component.
struct Health2D {
    static constexpr u8 kTypeId = 12;

    i16         hp{100};
    i16         maxHp{100};
    u16         iframeTicks{0};
    u16         stunTicks{0};
    u16         autoIframeTicks{24};
    u16         lastSwingTag{0};
    u8          poise{20};
    u8          maxPoise{20};
    u8          team{0};
    HealthFlags flags{HealthFlags::None};

    [[nodiscard]] constexpr bool IsAlive() const noexcept {
        return !HasFlag(flags, HealthFlags::Dead) && hp > 0;
    }
    [[nodiscard]] constexpr bool IsInvulnerable() const noexcept {
        return HasFlag(flags, HealthFlags::Invulnerable) || iframeTicks > 0U;
    }

    [[nodiscard]] constexpr bool ApplyDamage(i16 amount) noexcept {
        if (amount <= 0) return false;
        hp = static_cast<i16>(hp - amount);
        if (hp <= 0) {
            hp = 0;
            flags |= HealthFlags::Dead;
            return true;
        }
        return false;
    }

    constexpr void Heal(i16 amount) noexcept {
        if (amount > 0 && IsAlive()) {
            hp = static_cast<i16>(std::min<i32>(maxHp, hp + amount));
        }
    }
};
static_assert(sizeof(Health2D) == 16);

// 48-byte combat hurtbox and active attack hitbox component with automatic facing flip and swing deduplication.
struct CombatBox2D {
    static constexpr u8 kTypeId = 13;

    Rect        hurtbox{-0.4f, -0.5f, 0.8f, 1.0f};
    Rect        hitbox{0.4f, -0.4f, 0.8f, 0.8f};
    Vec2        knockback{6.0f, 4.0f};
    i16         damage{10};
    u16         swingId{1};
    u8          poiseDamage{10};
    u8          hitstopTicks{4};
    u8          targetTeamMask{0xFF};
    CombatFlags flags{CombatFlags::HurtboxActive | CombatFlags::FlipWithFacing};

    constexpr void BeginSwing(
        const Rect& box,
        i16 dmg,
        Vec2 kb = {6.0f, 4.0f},
        u8 hitstop = 4U,
        u8 poiseDmg = 10U) noexcept {
        ++swingId;
        if (swingId == 0U) swingId = 1U;
        hitbox       = box;
        damage       = dmg;
        knockback    = kb;
        hitstopTicks = hitstop;
        poiseDamage  = poiseDmg;
        flags       |= CombatFlags::HitboxActive;
    }

    constexpr void EndSwing() noexcept {
        flags &= ~CombatFlags::HitboxActive;
    }

    [[nodiscard]] static constexpr Rect TransformBox(const Rect& local, Vec2 worldPos, i8 facing) noexcept {
        if (facing < 0) {
            return {worldPos.x - (local.x + local.w), worldPos.y + local.y, local.w, local.h};
        }
        return {worldPos.x + local.x, worldPos.y + local.y, local.w, local.h};
    }

    [[nodiscard]] constexpr Rect WorldHurtbox(Vec2 worldPos, i8 facing = 1) const noexcept {
        const i8 dir = HasFlag(flags, CombatFlags::FlipWithFacing) ? facing : static_cast<i8>(1);
        return TransformBox(hurtbox, worldPos, dir);
    }

    [[nodiscard]] constexpr Rect WorldHitbox(Vec2 worldPos, i8 facing = 1) const noexcept {
        const i8 dir = HasFlag(flags, CombatFlags::FlipWithFacing) ? facing : static_cast<i8>(1);
        return TransformBox(hitbox, worldPos, dir);
    }
};
static_assert(sizeof(CombatBox2D) == 48);

// 16-byte hazard respawn anchor tracking the last stable solid ground position.
struct SafeAnchor2D {
    static constexpr u8 kTypeId = 14;

    Vec2 safePos{0.0f, 0.0f};
    Vec2 prevSafePos{0.0f, 0.0f};
};
static_assert(sizeof(SafeAnchor2D) == 16);

// 64-byte 2D Metroidvania camera follower with deadzone, lookahead, room confinement, and trauma shake.
struct CameraFollow2D {
    static constexpr u8 kTypeId = 15;

    Rect        roomBounds{-1000.0f, -1000.0f, 2000.0f, 2000.0f};
    Vec2        anchor{0.0f, 0.0f};
    Vec2        velocity{0.0f, 0.0f};
    Vec2        deadzone{1.0f, 0.75f};
    f32         lookaheadDist{2.0f};
    f32         lookaheadX{0.0f};
    f32         groundLatchY{0.0f};
    f32         trauma{0.0f};
    EntityId    target{kNullEntity};
    CameraFlags flags{CameraFlags::EnableLookahead | CameraFlags::ConfineToRoom | CameraFlags::SnapNextStep};
    u8          followOmega{10};
    u16         pad{0};

    constexpr void AddTrauma(f32 amount) noexcept {
        trauma = std::clamp(trauma + amount, 0.0f, 1.0f);
    }
};
static_assert(sizeof(CameraFollow2D) == 64);

// 32-byte resolved combat interaction record emitted during GameplaySystem::Step.
struct CombatHitEvent {
    EntityId   attacker{kNullEntity};
    EntityId   defender{kNullEntity};
    Vec2       point{};
    Vec2       knockback{};
    i16        damage{0};
    u16        swingTag{0};
    HitOutcome outcome{HitOutcome::None};
    u8         hitstopTicks{0};
    u8         killed{0};
    u8         poiseBroken{0};
};
static_assert(sizeof(CombatHitEvent) == 32);

struct GameplayCallbacks {
    void (*onStateStep)(
        EntityId id,
        StateMachine2D& fsm,
        World& world,
        const TickInput& input,
        ActionBuffer& actions,
        GameplaySystem& gameplay) noexcept{nullptr};
    void (*onStateTransition)(
        EntityId id,
        u8 fromState,
        u8 toState,
        World& world,
        GameplaySystem& gameplay) noexcept{nullptr};
    void (*onCombatHit)(
        const CombatHitEvent& hit,
        World& world,
        GameplaySystem& gameplay) noexcept{nullptr};
};
static_assert(sizeof(GameplayCallbacks) == 24);

struct GameplayConfig {
    f32 groundAccel{65.0f};
    f32 airAccel{42.0f};
    f32 knockbackDecay{12.0f};
    f32 jumpCutMultiplier{0.45f};
    u8  coyoteWindowTicks{8};
    u8  dashDurationTicks{12};
    u8  dashCooldownTicks{24};
    u8  defaultStunTicks{15};
    u16 maxCuesPerTick{256};
    u16 maxCombatHitsPerTick{128};
    u32 arenaBytes{128U * 1024U};
};
static_assert(sizeof(GameplayConfig) == 28);

} // namespace en
