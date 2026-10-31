#pragma once

#include "gameplay/GameplayTypes.h"

namespace en {

enum class CameraFlags : u8 {
    None            = 0,
    EnableLookahead = 1U << 0,
    LatchGroundY    = 1U << 1,
    ConfineToRoom   = 1U << 2,
    SnapNextStep    = 1U << 3,
};
consteval bool enable_bitmask_operators(CameraFlags) noexcept { return true; }

enum class MutationFlags : u8 {
    None             = 0,
    DestroyIfMutated = 1U << 0,
    DisableCollider  = 1U << 1,
    HideSprite       = 1U << 2,
};
consteval bool enable_bitmask_operators(MutationFlags) noexcept { return true; }

// 32-byte simulation cue emitted by gameplay systems for audio, VFX, hitstop, or camera feedback.
// Value type; drained by EnginePipeline::RouteGameplayCues during PostPhysics.
// Invariant: sizeof(GameplayCue) == 32.
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

// 16-byte hazard respawn anchor tracking the last stable solid ground position.
// Value type; updated automatically by GameplaySystem::SyncCharactersPostPhysics.
// Invariant: sizeof(SafeAnchor2D) == 16.
struct SafeAnchor2D {
    static constexpr u8 kTypeId = 14;

    Vec2 safePos{0.0f, 0.0f};
    Vec2 prevSafePos{0.0f, 0.0f};
};
static_assert(sizeof(SafeAnchor2D) == 16);

// 64-byte 2D Metroidvania camera follower with deadzone, lookahead, room confinement, and dual-target framing.
// Value type; stepped during PostPhysics before render extraction.
// Invariant: sizeof(CameraFollow2D) == 64.
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
    u16         secondaryTargetIndex{0};

    constexpr void AddTrauma(f32 amount) noexcept {
        trauma = std::clamp(trauma + amount, 0.0f, 1.0f);
    }

    constexpr void SetSecondaryTarget(EntityId secondary) noexcept {
        secondaryTargetIndex = secondary.IsValid() ? secondary.index : 0U;
    }
};
static_assert(sizeof(CameraFollow2D) == 64);

// 16-byte world mutation persistence link binding an entity to a deterministic SpatialGuid or world flag.
// Value type; automatically records permanent state changes (death, pickup, unlock) and patches rooms on load.
// Invariant: sizeof(WorldMutation2D) == 16.
struct WorldMutation2D {
    static constexpr u8 kTypeId = 17;

    u64           guid{0};
    u16           flagId{0xFFFFU};
    MutationFlags flags{MutationFlags::DestroyIfMutated};
    u8            recordOnDeath{1};
    u32           userTag{0};

    [[nodiscard]] static constexpr u64 ComputeSpatialGuid(
        u32 sectorId, i32 cellX, i32 cellY, u32 entityTypeTag) noexcept {
        const u64 a = (static_cast<u64>(sectorId) << 32ULL) | static_cast<u32>(cellX);
        const u64 b = (static_cast<u64>(static_cast<u32>(cellY)) << 32ULL) | entityTypeTag;
        const u64 h = HashCombine(a, b);
        return h != 0ULL ? h : 1ULL;
    }
};
static_assert(sizeof(WorldMutation2D) == 16);

// 40-byte room portal trigger linking a trigger collider to an adjacent room transition and spawn offset.
// Value type; checked during PostPhysics when an actor overlaps the portal trigger volume.
// Invariant: sizeof(RoomPortal2D) == 40.
struct RoomPortal2D {
    static constexpr u8 kTypeId = 18;

    AssetId targetRoom{};
    AssetId targetCollisionMap{};
    AssetId targetPrefab{};
    Vec2    spawnPos{};
    u16     requiredFlag{0xFFFFU};
    u8      wipeTicks{12};
    u8      wipeKind{1};
    u32     pad{0};
};
static_assert(sizeof(RoomPortal2D) == 40);

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

// 48-byte data-driven configuration for GameplaySystem locomotion, apex hang, and combat parameters.
// Value type; eliminates hardcoded magic numbers inside character and combat solvers.
// Invariant: sizeof(GameplayConfig) == 48.
struct GameplayConfig {
    f32 groundAccel{65.0f};
    f32 airAccel{42.0f};
    f32 knockbackDecay{12.0f};
    f32 jumpCutMultiplier{0.45f};
    f32 pogoJumpMultiplier{0.90f};
    f32 apexSpeedThreshold{2.5f};
    f32 apexGravityScale{0.50f};
    f32 apexAirAccelScale{1.35f};
    f32 parryRecoilScale{0.60f};
    u8  coyoteWindowTicks{8};
    u8  dashDurationTicks{12};
    u8  dashCooldownTicks{24};
    u8  defaultStunTicks{15};
    u16 maxCuesPerTick{256};
    u16 maxCombatHitsPerTick{128};
    u32 arenaBytes{128U * 1024U};
};
static_assert(sizeof(GameplayConfig) == 48);

} // namespace en
