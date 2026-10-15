#pragma once

#include <array>
#include <span>
#include <string_view>

#include "assets/AssetTypes.h"
#include "audio/AudioSystem.h"
#include "core/Result.h"
#include "core/Types.h"
#include "core/time/TimeTypes.h"
#include "debug/DebugSystem.h"
#include "framework/FrameworkTypes.h"
#include "gameplay/GameplayPipeline.h"
#include "gameplay/GameplayTypes.h"
#include "platform/input/InputTypes.h"
#include "world/World.h"
#include "world/WorldTypes.h"

namespace game {

using namespace en;

// ============================================================================
// 1. Compile-Time FNV-1a Asset & Cue Hashing (Deterministic Zero-Heap IDs)
// ============================================================================
consteval u64 HashAssetString(std::string_view str) noexcept {
    u64 hash = 14695981039346656037ull;
    for (char c : str) {
        hash ^= static_cast<u64>(static_cast<u8>(c));
        hash *= 1099511628211ull;
    }
    return hash;
}

consteval assets::AssetId operator""_aid(const char* str, usize len) noexcept {
    return assets::AssetId{HashAssetString(std::string_view{str, len})};
}

consteval audio::SoundCueId operator""_cue(const char* str, usize len) noexcept {
    return audio::SoundCueId::FromAssetId(assets::AssetId{HashAssetString(std::string_view{str, len})});
}

// ============================================================================
// 2. Logical Input Actions & Axes (Satisfies platform::input::InputId<T>)
// ============================================================================
enum class GameAction : u8 {
    MoveLeft       = 0,
    MoveRight      = 1,
    LookUp         = 2,
    Crouch         = 3,
    Jump           = 4,
    AttackPrimary  = 5,
    SkillSecondary = 6,
    Dash           = 7,
    Parry          = 8,
    Interact       = 9,
    Pause          = 10,
    MenuConfirm    = 11,
    MenuCancel     = 12,
    QuickRestart   = 13
};

enum class GameAxis : u8 {
    MoveX = 0,
    MoveY = 1
};

// Action Masks per State Layer
inline constexpr u16 kMenuActionMask = platform::input::MakeActionMask(
    GameAction::LookUp, GameAction::Crouch, GameAction::MenuConfirm, GameAction::MenuCancel, GameAction::Pause);

inline constexpr u16 kGameplayActionMask = platform::input::MakeActionMask(
    GameAction::MoveLeft, GameAction::MoveRight, GameAction::LookUp, GameAction::Crouch,
    GameAction::Jump, GameAction::AttackPrimary, GameAction::SkillSecondary,
    GameAction::Dash, GameAction::Parry, GameAction::Interact, GameAction::Pause);

// ============================================================================
// 3. SceneDirector State IDs & Persistent World Flags (0..4095)
// ============================================================================
namespace StateIds {
    inline constexpr framework::StateId kTitleScreen = 1;
    inline constexpr framework::StateId kGameplay    = 2;
    inline constexpr framework::StateId kPauseModal  = 3;
    inline constexpr framework::StateId kGameOver    = 4;
}

namespace WorldFlags {
    inline constexpr u16 kUnlockedWallCling    = 1;
    inline constexpr u16 kUnlockedShadowDash   = 2;
    inline constexpr u16 kUnlockedDoubleJump   = 3;
    inline constexpr u16 kRoom01GateOpened     = 10;
    inline constexpr u16 kRoom02MiniBossSlain  = 11;
    inline constexpr u16 kShrine01Activated    = 20;
}

// ============================================================================
// 4. Actor FSM States & Teams
// ============================================================================
enum class PlayerState : u8 {
    Idle         = 0,
    Run          = 1,
    JumpRise     = 2,
    Fall         = 3,
    Crouch       = 4,
    WallSlide    = 5,
    WallJump     = 6,
    Dash         = 7,
    AttackSlash1 = 8,
    AttackSlash2 = 9,
    AttackSlam3  = 10,
    AirSlash     = 11,
    ParryStance  = 12,
    ParryRiposte = 13,
    HurtStun     = 14,
    Dead         = 15
};

enum class EnemyKind : u8 {
    CrawlerHusk   = 0, // Patrols ledges, reverses on wall or missing ground ray
    LancerGuard   = 1, // Shields frontally, telegraphs lunging thrust, vulnerable to parry
    WardenBoss    = 2  // Multi-phase chamber guardian with jump-slam and dash-sweep
};

enum class EnemyState : u8 {
    Idle       = 0,
    Patrol     = 1,
    Alert      = 2,
    Windup     = 3,
    Attack     = 4,
    Recovery   = 5,
    Parried    = 6,
    Hurt       = 7,
    Dead       = 8
};

namespace TeamIds {
    inline constexpr u8 kPlayer      = 1;
    inline constexpr u8 kEnemy       = 2;
    inline constexpr u8 kEnvironment = 3;
}

namespace CollisionLayers {
    inline constexpr u16 kStaticWorld  = 1u << 0;
    inline constexpr u16 kOneWay       = 1u << 1;
    inline constexpr u16 kPlayerBody   = 1u << 2;
    inline constexpr u16 kEnemyBody    = 1u << 3;
    inline constexpr u16 kPlayerHitbox = 1u << 4;
    inline constexpr u16 kEnemyHitbox  = 1u << 5;
    inline constexpr u16 kHazardHitbox = 1u << 6;
}

// ============================================================================
// 5. Known Asset & Cue Identifiers
// ============================================================================
namespace Assets {
    // Room 01: Forgotten Ancestral Hall
    inline constexpr assets::AssetId kRoom01Scene     = "scenes/ancestral_hall"_aid;
    inline constexpr assets::AssetId kRoom01Manifest  = "scenes/ancestral_hall.escn"_aid;
    inline constexpr assets::AssetId kRoom01Collision = "maps/ancestral_hall.ecol"_aid;
    inline constexpr assets::AssetId kRoom01Prefab    = "prefabs/ancestral_hall_root.epfb"_aid;
    inline constexpr assets::AssetId kRoom01AudioBank = "audio/ancestral_hall.eaud"_aid;

    // Room 02: Sunken Reliquary (Mini-Boss & Vertical Shaft)
    inline constexpr assets::AssetId kRoom02Scene     = "scenes/sunken_reliquary"_aid;
    inline constexpr assets::AssetId kRoom02Manifest  = "scenes/sunken_reliquary.escn"_aid;
    inline constexpr assets::AssetId kRoom02Collision = "maps/sunken_reliquary.ecol"_aid;
    inline constexpr assets::AssetId kRoom02Prefab    = "prefabs/sunken_reliquary_root.epfb"_aid;
    inline constexpr assets::AssetId kRoom02AudioBank = "audio/sunken_reliquary.eaud"_aid;

    // Actor Rigs & Atlases
    inline constexpr assets::AssetId kPlayerAnimRig   = "rigs/knight_player.erig"_aid;
    inline constexpr assets::AssetId kEnemyAnimRig    = "rigs/abyss_enemies.erig"_aid;
    inline constexpr assets::AssetId kHeroAtlas       = "atlases/knight_player.eatl"_aid;
    inline constexpr assets::AssetId kEnemyAtlas      = "atlases/abyss_enemies.eatl"_aid;
    inline constexpr assets::AssetId kVfxAtlas        = "atlases/combat_vfx.eatl"_aid;

    // Sound Cues
    inline constexpr audio::SoundCueId kCueJump       = "sfx/player_jump"_cue;
    inline constexpr audio::SoundCueId kCueLand       = "sfx/player_land"_cue;
    inline constexpr audio::SoundCueId kCueDash       = "sfx/player_dash"_cue;
    inline constexpr audio::SoundCueId kCueSlash      = "sfx/sword_slash"_cue;
    inline constexpr audio::SoundCueId kCueHitFlesh   = "sfx/hit_impact_heavy"_cue;
    inline constexpr audio::SoundCueId kCueParryClang = "sfx/parry_clang"_cue;
    inline constexpr audio::SoundCueId kCuePlayerHurt = "sfx/player_hurt"_cue;
    inline constexpr audio::SoundCueId kCueUiMove     = "sfx/ui_cursor"_cue;
    inline constexpr audio::SoundCueId kCueUiConfirm  = "sfx/ui_confirm"_cue;
    inline constexpr audio::SoundCueId kCueRelicGet   = "sfx/relic_fanfare"_cue;

    // Music Track IDs
    inline constexpr u32 kMusicTrackTitle     = 1;
    inline constexpr u32 kMusicTrackCatacombs = 2;
    inline constexpr u32 kMusicTrackBoss      = 3;
}

// ============================================================================
// 6. Client ECS Components (kTypeId 32..37, Zero Padding, Trivially Copyable)
// ============================================================================

enum class AbilityMask : u8 {
    None       = 0,
    WallCling  = 1u << 0,
    ShadowDash = 1u << 1,
    DoubleJump = 1u << 2
};
constexpr AbilityMask operator|(AbilityMask a, AbilityMask b) noexcept {
    return static_cast<AbilityMask>(static_cast<u8>(a) | static_cast<u8>(b));
}
constexpr bool HasAbility(AbilityMask mask, AbilityMask bit) noexcept {
    return (static_cast<u8>(mask) & static_cast<u8>(bit)) != 0;
}

// Player controller component (32 bytes exact, zero padding)
struct alignas(32) PlayerController2D {
    static constexpr u8 kTypeId = 32;

    f32 spawnX{4.0f};
    f32 spawnY{4.0f};
    u16 dashCooldownTicks{0};
    u16 dashActiveTicks{0};
    u16 wallJumpLockTicks{0};
    u16 comboResetTicks{0};
    u16 parryWindowTicks{0};
    u16 healChannelTicks{0};
    u16 portalCooldownTicks{0};
    u16 soulEnergy{50};           // [0..100], spent on skills/healing, gained on melee hits
    u8  airJumpsRemaining{1};
    u8  maxAirJumps{1};
    u8  airDashesRemaining{1};
    AbilityMask unlockedAbilities{AbilityMask::WallCling | AbilityMask::ShadowDash | AbilityMask::DoubleJump};
    u8  comboStep{0};             // 0 = Slash1, 1 = Slash2, 2 = Slam3
    u8   bufferedAttack{0};
    u8  facingSign{1};            // 0 = Left (-1), 2 = Right (+1)
    u8  wasGrounded{0};
};
static_assert(sizeof(PlayerController2D) == 32);
static_assert(world::Component<PlayerController2D>);

// Enemy AI & combat brain component (32 bytes exact, zero padding)
struct alignas(32) EnemyBrain2D {
    static constexpr u8 kTypeId = 33;

    f32 homeX{0.0f};
    f32 homeY{0.0f};
    f32 patrolRadius{5.0f};
    f32 aggroRange{7.5f};
    f32 attackRange{1.8f};
    u16 aiTimerTicks{0};
    u16 attackCooldownTicks{0};
    EnemyKind kind{EnemyKind::CrawlerHusk};
    EnemyState aiState{EnemyState::Patrol};
    i8  moveDir{1};               // -1 or +1
    u8  phase{0};
    u16 worldFlagOnDeath{0};      // Sets persistent flag when slain (e.g., kRoom02MiniBossSlain)
    u16 reserved{0};
};
static_assert(sizeof(EnemyBrain2D) == 32);
static_assert(world::Component<EnemyBrain2D>);

// Room Transition Portal / Interactable Shrine / Ability Relic (32 bytes exact)
enum class TriggerKind : u8 {
    RoomPortal    = 0,
    SaveShrine    = 1,
    AbilityRelic  = 2,
    SpikeHazard   = 3,
    LockedGate    = 4
};

struct alignas(32) RoomTriggerVolume2D {
    static constexpr u8 kTypeId = 34;

    assets::AssetId targetSceneId{};
    f32 halfW{1.0f};
    f32 halfH{2.0f};
    f32 destSpawnX{3.0f};
    f32 destSpawnY{3.0f};
    u16 worldFlagId{0};
    TriggerKind kind{TriggerKind::RoomPortal};
    u8  destSpawnFacing{1};
};
static_assert(sizeof(RoomTriggerVolume2D) == 32);
static_assert(world::Component<RoomTriggerVolume2D>);

// Transient combat VFX / floating damage indicator (16 bytes exact)
struct alignas(16) TransientVfx2D {
    static constexpr u8 kTypeId = 35;

    f32 velX{0.0f};
    f32 velY{1.5f};
    u16 remainingTicks{24};
    u16 initialTicks{24};
    u8  fadeAlpha{1};
    u8  pad0{0};
    u16 pad1{0};
};
static_assert(sizeof(TransientVfx2D) == 16);
static_assert(world::Component<TransientVfx2D>);

// Tag marking entities owned by the current room that must be destroyed on RoomSwap (4 bytes)
struct alignas(4) RoomTransientTag {
    static constexpr u8 kTypeId = 36;
    u32 roomSalt{0};
};
static_assert(sizeof(RoomTransientTag) == 4);
static_assert(world::Component<RoomTransientTag>);

// Moving platform waypoint oscillator driving KinematicCarrier2D (32 bytes exact)
struct alignas(32) WaypointCarrierDriver2D {
    static constexpr u8 kTypeId = 37;

    f32 startX{0.0f};
    f32 startY{0.0f};
    f32 endX{0.0f};
    f32 endY{6.0f};
    f32 speedMetersPerSec{2.5f};
    u32 phaseTicks{0};
    u32 cycleTicks{480};          // 4 seconds full cycle at 120 Hz
    u16 requiredWorldFlag{0};     // 0 = always active, >0 = requires flag set
    u16 pad{0};
};
static_assert(sizeof(WaypointCarrierDriver2D) == 32);
static_assert(world::Component<WaypointCarrierDriver2D>);

// ============================================================================
// 7. Tunable Movement & Combat Parameters (Bound to DebugConsole CVars)
// ============================================================================
struct MovementTuning {
    f32 runSpeed{8.5f};
    f32 jumpImpulse{13.8f};
    f32 doubleJumpImpulse{12.2f};
    f32 jumpReleaseCutFactor{0.45f};
    f32 wallSlideMaxFallSpeed{-3.2f};
    f32 wallJumpVelX{9.5f};
    f32 wallJumpVelY{13.0f};
    f32 dashSpeed{19.0f};
    i32 dashDurationTicks{16};    // 133ms at 120 Hz
    i32 dashCooldownTicks{42};    // 350ms at 120 Hz
    i32 coyoteTicks{10};          // 83ms ledge forgiveness
    i32 jumpBufferTicks{12};      // 100ms input buffer window
    i32 hitstopLightTicks{5};     // 41ms hitstop on slash 1/2
    i32 hitstopHeavyTicks{10};    // 83ms hitstop on slam / parry
    bool infiniteSoul{false};
    bool godMode{false};
};

// ============================================================================
// 8. Persistent Client Session State (Zero-Heap Pre-Allocated Singleton)
// ============================================================================
struct RoomDescriptor {
    assets::AssetId sceneId{};
    assets::AssetId manifestId{};
    assets::AssetId collisionId{};
    assets::AssetId prefabId{};
    assets::AssetId audioBankId{};
    gameplay::CompactAabb2D cameraBounds{};
    u32 musicTrackId{Assets::kMusicTrackCatacombs};
};

struct alignas(64) GameSessionState {
    // Subsystem back-pointers for console commands & transitions
    framework::SceneDirector* director{nullptr};
    debug::DebugSystem* debugSys{nullptr};

    // Cached persistent entities across RoomSwap transitions
    world::EntityId playerEntity{world::kNullEntity};
    world::EntityId cameraEntity{world::kNullEntity};

    // Pending spawn state applied inside SceneTransitionDesc::onActivated
    assets::AssetId activeRoomId{Assets::kRoom01Scene};
    f32 pendingSpawnX{4.0f};
    f32 pendingSpawnY{4.0f};
    i8  pendingSpawnFacing{1};
    bool hasPendingRoomTeleport{false};
    bool quitRequested{false};

    // Checkpoint state (updated at SaveShrines)
    assets::AssetId checkpointRoomId{Assets::kRoom01Scene};
    f32 checkpointX{4.0f};
    f32 checkpointY{4.0f};

    // UI & Modal state counters
    u8  titleCursor{0};
    u8  pauseCursor{0};
    u8  gameOverCursor{0};
    u16 bannerTimerTicks{0};
    std::array<char, 32> bannerText{};

    // Live CVar-tunable parameters
    MovementTuning tuning{};

    // Pre-baked room directory (zero heap lookup)
    std::array<RoomDescriptor, 2> rooms{{
        RoomDescriptor{
            .sceneId      = Assets::kRoom01Scene,
            .manifestId   = Assets::kRoom01Manifest,
            .collisionId  = Assets::kRoom01Collision,
            .prefabId     = Assets::kRoom01Prefab,
            .audioBankId  = Assets::kRoom01AudioBank,
            .cameraBounds = {.offsetX_q8 = 25 * 256, .offsetY_q8 = 10 * 256, .halfW_q8 = 25 * 256, .halfH_q8 = 10 * 256},
            .musicTrackId = Assets::kMusicTrackCatacombs
        },
        RoomDescriptor{
            .sceneId      = Assets::kRoom02Scene,
            .manifestId   = Assets::kRoom02Manifest,
            .collisionId  = Assets::kRoom02Collision,
            .prefabId     = Assets::kRoom02Prefab,
            .audioBankId  = Assets::kRoom02AudioBank,
            .cameraBounds = {.offsetX_q8 = 30 * 256, .offsetY_q8 = 18 * 256, .halfW_q8 = 30 * 256, .halfH_q8 = 18 * 256},
            .musicTrackId = Assets::kMusicTrackBoss
        }
    }};

    [[nodiscard]] constexpr const RoomDescriptor* FindRoom(assets::AssetId id) const noexcept {
        for (const auto& r : rooms) {
            if (r.sceneId.value == id.value) return &r;
        }
        return &rooms[0];
    }

    constexpr void ShowBanner(std::string_view msg, u16 durationTicks = 240) noexcept {
        bannerTimerTicks = durationTicks;
        bannerText.fill('\0');
        const usize n = msg.size() < 31 ? msg.size() : 31;
        for (usize i = 0; i < n; ++i) bannerText[i] = msg[i];
    }
};

} // namespace game