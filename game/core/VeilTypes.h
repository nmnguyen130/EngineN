#pragma once

#include "framework/Application.h"
#include "gameplay/AbilityAndModifier.h"

namespace en::veil {

enum class VeilAction : u8 {
    MoveLeft = 0,
    MoveRight = 1,
    Jump = 2,
    Dash = 3,
    Attack = 4,
    Parry = 5,
    ThrowShuriken = 6,
    Grapple = 7,
    Interact = 8,
    Pause = 9,
    UltimateChord = 10,
};

enum class VeilAxis : u8 {
    MoveX = 0,
    MoveY = 1,
};

enum class PlayerState : u8 {
    Idle = 0,
    Run = 1,
    Jump = 2,
    Fall = 3,
    WallSlide = 4,
    Dash = 5,
    Slash1 = 6,
    Slash2 = 7,
    Slash3 = 8,
    ParryStance = 9,
    ParryRiposte = 10,
    GrapplePull = 11,
    Hurt = 12,
};

enum class EnemyKind : u8 {
    RoninScout = 0,
    AshArcher = 1,
    WardenBoss = 2,
};

enum class EnemyState : u8 {
    Patrol = 0,
    Chase = 1,
    Windup = 2,
    Attack = 3,
    Recover = 4,
    Staggered = 5,
};

inline constexpr StateId kTitleStateId = 1;
inline constexpr StateId kGameplayStateId = 2;
inline constexpr StateId kPauseStateId = 3;

inline constexpr AssetId kSceneCourtyardId = "scenes/courtyard.escn"_aid;
inline constexpr AssetId kSceneBossSanctumId = "scenes/sanctum.escn"_aid;
inline constexpr AssetId kColCourtyardId = "maps/courtyard.ecol"_aid;
inline constexpr AssetId kColSanctumId = "maps/sanctum.ecol"_aid;
inline constexpr AssetId kRigPlayerId = "rigs/player.erig"_aid;
inline constexpr AssetId kRigEnemyId = "rigs/enemy.erig"_aid;
inline constexpr AssetId kCueBankMainId = "audio/main.eaud"_aid;
inline constexpr AssetId kClipSlashId = "audio/slash.pcm"_aid;
inline constexpr AssetId kClipParryId = "audio/parry.pcm"_aid;
inline constexpr AssetId kClipHitId = "audio/hit.pcm"_aid;
inline constexpr AssetId kTexMainAtlasId = "textures/veil_atlas.rgba"_aid;
inline constexpr AssetId kAtlasMainId = "atlases/veil.eatlas"_aid;

struct alignas(8) PlayerControl2D {
    static constexpr u8 kTypeId = 24;
    Vec2 grappleTarget{};
    f32 focusEnergy{50.0f};
    f32 maxFocusEnergy{100.0f};
    u16 dashCooldownTicks{0};
    u16 shurikenCooldownTicks{0};
    u8 airDashesLeft{1};
    u8 comboIndex{0};
    u8 shurikenAmmo{5};
    bool grappleActive{false};
};
static_assert(sizeof(PlayerControl2D) == 24);

struct alignas(8) EnemyBrain2D {
    static constexpr u8 kTypeId = 25;
    f32 patrolOriginX{0.0f};
    f32 patrolRange{4.0f};
    f32 aggroRange{9.0f};
    f32 attackRange{1.5f};
    u16 actionCooldownTicks{0};
    EnemyKind kind{EnemyKind::RoninScout};
    u8 bossPhase{1};
    i8 patrolDir{1};
    bool aggroActive{false};
    u16 homeRoomId{0};
};
static_assert(sizeof(EnemyBrain2D) == 24);

struct alignas(8) Projectile2D {
    static constexpr u8 kTypeId = 26;
    EntityId owner{kNullEntity};
    f32 vx{0.0f};
    f32 vy{0.0f};
    u8 teamId{0};
    u8 damage{12};
    bool reflected{false};
    u8 bouncesLeft{0};
};
static_assert(sizeof(Projectile2D) == 16);

struct alignas(4) GrappleAnchor2D {
    static constexpr u8 kTypeId = 27;
    f32 pullSpeed{22.0f};
    f32 activationRadius{9.5f};
    bool enabled{true};
    u8 anchorTag{0};
    u16 cooldownTicks{0};
};
static_assert(sizeof(GrappleAnchor2D) == 12);

struct alignas(4) ShrineCheckpoint2D {
    static constexpr u8 kTypeId = 28;
    u16 shrineId{1};
    u16 worldFlagId{10};
    bool activated{false};
    u8 healPercent{100};
    u16 bonusSouls{25};
};
static_assert(sizeof(ShrineCheckpoint2D) == 8);

struct alignas(4) BreakableProp2D {
    static constexpr u8 kTypeId = 29;
    u16 worldFlagId{100};
    u8 dropSoulCount{3};
    bool broken{false};
};
static_assert(sizeof(BreakableProp2D) == 4);

struct VeilSessionState {
    EntityId playerEntity{kNullEntity};
    EntityId cameraEntity{kNullEntity};
    EntityId bossEntity{kNullEntity};
    AtlasHandle mainAtlas{};
    BlobHandle playerRigBlob{};
    BlobHandle enemyRigBlob{};
    BehaviorTree2D enemyBt{};
    PostProcessConfig postFx{};
    u32 soulCurrency{0};
    u32 roomsCleared{0};
    u32 totalParries{0};
    u32 totalKills{0};
    f32 cvarPlayerSpeed{8.5f};
    f32 cvarJumpSpeed{14.2f};
    bool bossDefeated{false};
    bool showUiPanel{true};
};

} // namespace en::veil
