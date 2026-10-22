#pragma once

#include "game/core/GameConfig.h"
#include "world/WorldTypes.h"

namespace vots {

// Tracks Kaelen's Triad Deck, Harmony gauge, air-dash charge, Parry window, and carried Tuning Shards.
// Registered in World at kTypeId = 24; strictly 32 bytes (2 per cache line, 0B padding).
// Invariant: harmony in [0..100], airDashCharges <= maxAirDashCharges, and capturedEchoId is fixed on attack/parry start.
struct alignas(32) PlayerStateComp {
    static constexpr u8 kTypeId = 24U;

    u32 carriedTuningShards{0U};
    i32 baseMaxHp{kPlayerBaseMaxHp};
    TriadDeck deck{};
    u16 unlockedEchoMask{0x000FU};     // Ignis, Terra, Fulgur, Glacies unlocked at baseline
    u16 parryWindowTicks{0U};          // Counts down from 10 ticks on Parry input
    u16 parryCooldownTicks{0U};
    u16 dashCooldownTicks{0U};
    u16 healFlashTicks{0U};
    u8 harmony{0U};                    // Clamped [0..100]
    u8 airDashCharges{1U};             // Restored to 1 on valid ground contact; never restored by reactions
    u8 maxAirDashCharges{1U};
    EchoId capturedParryEcho{EchoId::Ignis};
    u8 activeRoomId{0U};
    u8 lastResonatorRoomId{0U};
    u8 huskPenaltyActive{0U};          // 1 when Lost Wick Husk is unrecovered (-25% Max HP cap)
    u8 lanternRelinquished{0U};        // 1 after Ending C (Symphony of the Off-Beat)
};
static_assert(sizeof(PlayerStateComp) == 32);
static_assert(en::Component<PlayerStateComp>);

// Tracks an active weapon attack instance and the EchoId captured at the moment the swing started.
// Registered in World at kTypeId = 25; strictly 16 bytes (4 per cache line, 0B padding).
// Invariant: capturedEchoId never changes mid-swing even if the player switches Echoes during the animation.
struct alignas(16) WeaponAttackComp {
    static constexpr u8 kTypeId = 25U;

    u32 instanceId{0U};
    u16 elapsedTicks{0U};
    u16 totalTicks{0U};
    i16 baseHpDamage{10};
    u8 baseBreakDamage{4U};
    WeaponMoveKind moveKind{WeaponMoveKind::None};
    EchoId capturedEchoId{EchoId::Ignis};
    AttackTelegraphKind telegraph{AttackTelegraphKind::Standard};
    EchoId counterEchoId{EchoId::None};
    u8 hitboxTriggered{0U};
};
static_assert(sizeof(WeaponAttackComp) == 16);
static_assert(en::Component<WeaponAttackComp>);

// Stores the single active Resonance Mark on a target and any pending Chrono-Overdrive delayed strike.
// Registered in World at kTypeId = 26; strictly 16 bytes (4 per cache line, 0B padding).
// Invariant: remainingTicks <= 420; expired marks (remainingTicks == 0) are cleared before hit resolution.
struct alignas(16) ResonanceMarkComp {
    static constexpr u8 kTypeId = 26U;

    u32 lastAppliedAttackInstance{0U};
    u16 remainingTicks{0U};
    u16 chronoDelayTicks{0U};
    i16 chronoPendingDamage{0};
    EchoId activeMark{EchoId::None};
    WaveReactionId lastReactionTriggered{WaveReactionId::None};
    u16 reactionVfxTicks{0U};
    u16 reserved{0U};

    [[nodiscard]] constexpr bool HasActiveMark() const noexcept {
        return remainingTicks > 0U && IsValidEcho(activeMark);
    }

    constexpr void ClearMark() noexcept {
        remainingTicks = 0U;
        activeMark = EchoId::None;
    }

    constexpr void ApplyOrRefreshMark(EchoId echo, u32 attackInstanceId) noexcept {
        activeMark = echo;
        remainingTicks = kResonanceMarkDurationTicks;
        lastAppliedAttackInstance = attackInstanceId;
    }
};
static_assert(sizeof(ResonanceMarkComp) == 16);
static_assert(en::Component<ResonanceMarkComp>);

// Independent Break posture meter, Break State / Break Recovery machine, Fracture vulnerability, and CC state.
// Registered in World at kTypeId = 27; strictly 32 bytes (2 per cache line, 0B padding).
// Invariant: breakMeter in [0..100], Break is ignored during Staggered/BreakState/BreakRecovery, and lock priority is BreakLock > Stun > MicroStagger.
struct alignas(32) BreakAndCcComp {
    static constexpr u8 kTypeId = 27U;

    f32 breakResistance{1.0f};
    f32 pullTargetX{0.0f};
    f32 pullTargetY{0.0f};
    u16 phaseTicks{0U};
    u16 actionLockTicks{0U};
    u16 fractureTicks{0U};
    u16 slowTicks{0U};
    u16 guardBrokenTicks{0U};
    u16 pullTicks{0U};
    u8 breakMeter{0U};
    BreakPhase breakPhase{BreakPhase::Normal};
    TargetCategory category{TargetCategory::Standard};
    ActionLockKind lockKind{ActionLockKind::None};
    u8 slowPercent{0U}; // e.g., 40 means 40% slow (0.60x speed)
    u8 flags{0U};       // bit 0: DisplacementResistant, bit 1: BreakableGuard, bit 2: GuardActive, bit 3:InterruptibleWindup

    static constexpr u8 kFlagDisplacementResistant = 1U << 0U;
    static constexpr u8 kFlagBreakableGuard        = 1U << 1U;
    static constexpr u8 kFlagGuardActive           = 1U << 2U;
    static constexpr u8 kFlagInterruptibleWindup   = 1U << 3U;
    static constexpr u8 kFlagAnchored              = 1U << 4U;

    [[nodiscard]] constexpr bool HasRuleFlag(u8 bit) const noexcept { return (flags & bit) != 0U; }
    [[nodiscard]] constexpr bool IsActionLocked() const noexcept {
        return breakPhase == BreakPhase::Staggered ||
               breakPhase == BreakPhase::BreakState ||
               actionLockTicks > 0U;
    }
    [[nodiscard]] constexpr f32 MoveSpeedMultiplier() const noexcept {
        if (IsActionLocked()) return 0.0f;
        if (slowTicks > 0U && slowPercent < 100U) {
            return static_cast<f32>(100U - slowPercent) * 0.01f;
        }
        return 1.0f;
    }
};
static_assert(sizeof(BreakAndCcComp) == 32);
static_assert(en::Component<BreakAndCcComp>);

// AI behavior controller for Standard Husks, Heavy Enforcers, and the 3-phase High Conductor Boss.
// Registered in World at kTypeId = 28; strictly 32 bytes (2 per cache line, 0B padding).
struct alignas(32) EnemyAiComp {
    static constexpr u8 kTypeId = 28U;

    f32 patrolOriginX{0.0f};
    f32 patrolRange{4.0f};
    f32 aggroRadius{7.0f};
    f32 attackRange{1.8f};
    u16 actionTimerTicks{0U};
    u16 cooldownTicks{60U};
    u16 shardReward{25U};
    u8 archetypeId{0U};      // 0: DissonantHusk, 1: ChoirEnforcer, 2: HighConductorBoss
    u8 bossPhase{1U};        // 1..3 for High Conductor
    u8 patternIndex{0U};
    AttackTelegraphKind activeTelegraph{AttackTelegraphKind::Standard};
    EchoId counterEcho{EchoId::None};
    u8 windupInterrupted{0U};
    u16 statePhaseTicks{0U};
};
static_assert(sizeof(EnemyAiComp) == 32);
static_assert(en::Component<EnemyAiComp>);

// Projectile component distinguishing clearable normal projectiles from protected Boss projectiles/traps.
// Registered in World at kTypeId = 29; strictly 16 bytes (4 per cache line, 0B padding).
struct alignas(16) ProjectileComp {
    static constexpr u8 kTypeId = 29U;

    EntityId owner{kNullEntity};
    i16 damage{12};
    u8 breakDamage{6U};
    u8 teamId{2U};
    u8 protectedFromClear{0U}; // 1 = Boss signature / environmental trap (cannot be cleared by Pyro-Cyclone)
    AttackTelegraphKind telegraph{AttackTelegraphKind::GoldHarmonicWave};
    EchoId counterEcho{EchoId::None};
    u8 active{1U};
    u32 reserved{0U};
};
static_assert(sizeof(ProjectileComp) == 16);
static_assert(en::Component<ProjectileComp>);

enum class InteractableKind : u8 {
    TuningResonator   = 0,
    LostWickHusk      = 1,
    EchoAltar         = 2,
    FrequencyGate     = 3,
    LoreTablet        = 4,
    MemoryTuningFork  = 5,
    SpireDecisionCore = 6,
    RoomExitPortal    = 7,
};

// Interactive world prop component (Checkpoints, Husks, Echo Altars, Frequency Gates, Lore Tablets, Portals).
// Registered in World at kTypeId = 30; strictly 16 bytes (4 per cache line, 0B padding).
struct alignas(16) InteractableComp {
    static constexpr u8 kTypeId = 30U;

    f32 interactRadius{1.5f};
    u16 requiredWorldFlag{0U};
    u16 grantedWorldFlag{0U};
    InteractableKind kind{InteractableKind::TuningResonator};
    u8 paramByte{0U};         // EchoId for EchoAltar, targetRoomId for RoomExitPortal, etc.
    u8 isMainRouteGate{1U};   // Communicates main route vs optional gate per GDD v0.2 Section 5.4
    u8 activated{0U};
    u32 shardPayload{0U};
};
static_assert(sizeof(InteractableComp) == 16);
static_assert(en::Component<InteractableComp>);

// Flagged destructible terrain wall/pillar damaged by Elemental Cataclysm Sacred Chord (GDD v0.2 Section 4.7).
// Registered in World at kTypeId = 31; strictly 16 bytes (4 per cache line, 0B padding).
struct alignas(16) DestructibleTerrainComp {
    static constexpr u8 kTypeId = 31U;

    i32 durability{30};
    u16 unlockedWorldFlagOnBreak{0U};
    u8 requiresSacredChord{1U};
    u8 broken{0U};
    u64 reserved{0ULL};
};
static_assert(sizeof(DestructibleTerrainComp) == 16);
static_assert(en::Component<DestructibleTerrainComp>);

} // namespace vots
