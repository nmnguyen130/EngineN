#pragma once

#include "game/core/GameTypes.h"

namespace vots {

inline constexpr i32 kStandardEnemyBaseHp            = 100;
inline constexpr i32 kHeavyEliteEnemyBaseHp          = 300;
inline constexpr i32 kBossPhaseBaseHp                = 1200;
inline constexpr u8  kBossPhaseTransitionLockTicks   = 60U;
inline constexpr u8  kBossTotalPhases                = 2U;

// GDD v0.5 Section 1 Rule 6 & Section 3: AI Elevation Perception Leash, Turn-Around Delay & Dual Attack-Token Pools.
inline constexpr f32 kEnemyVerticalAggroRangeWu      = 1.65f; // Max |dy| to chase/engage for standard/elite enemies
inline constexpr f32 kEnemyVerticalAttackRangeWu     = 1.15f; // Max |dy| to initiate a melee swing or horizontal bolt
inline constexpr f32 kBossVerticalAggroRangeWu       = 2.60f;
inline constexpr f32 kBossVerticalAttackRangeWu      = 1.65f;
inline constexpr f32 kEnemyTacticalRetreatDistWu     = 1.25f; // Agile enemies step back during cooldown if player is too close
inline constexpr u16 kEnemyTacticalRetreatTicks      = 28U;

// Turn-around reaction delay when Kaelen crosses behind an enemy (prevents instant 180-degree snap attacks).
inline constexpr u8  kEnemyTurnDelayTicks            = 18U;   // 150 ms turn-around delay before common enemy can flip & swing
inline constexpr u8  kBossTurnDelayTicks             = 22U;   // 183 ms turn-around delay for Boss

// Dual-Pool Attack Token System (inspired by DOOM / Dead Cells / Nine Sols attention budget):
// At most 1 melee attacker and 1 ranged attacker can execute an attack simultaneously.
inline constexpr u32 kMaxSimultaneousCommonAttackers = 1U;    // Max concurrent common melee attackers
inline constexpr u32 kMaxSimultaneousRangedAttackers = 1U;    // Max concurrent ranged attackers
inline constexpr u16 kAttackTokenBusyRetryTicks      = 28U;   // Staggered retry delay when attack token is held by another enemy

// Deliberate post-recovery AI cooldown ticks (0.95s - 1.20s rest window between attacks so single enemies never spam).
inline constexpr u16 kDrifterPostAttackCooldownTicks = 114U;  // 0.95s rest after 32t recovery
inline constexpr u16 kSentryPostAttackCooldownTicks  = 132U;  // 1.10s rest after 38t recovery
inline constexpr u16 kBrutePostAttackCooldownTicks   = 138U;  // 1.15s rest after 42t recovery
inline constexpr u16 kWardenPostAttackCooldownTicks  = 102U;  // 0.85s rest after Boss attack recovery

// Enemy & Boss attack-data contract per GDD v0.3–v0.5.
struct EnemyAttackSpec {
    EnemyAttackId id{EnemyAttackId::None};
    std::string_view name{};
    u16 windupTicks{18U};
    u16 activeTicks{4U};
    u16 recoveryTicks{24U};
    u16 postCooldownTicks{114U};
    i16 hitDamage{12};
    u8 breakDamage{10U};
    AttackTelegraphKind telegraph{AttackTelegraphKind::GoldHarmonicWave};
    EchoId counterEcho{EchoId::None};
    bool interruptible{true};
    f32 reachWidthWu{1.1f};
    f32 reachHeightWu{1.0f};
    f32 projectileSpeedWu{0.0f};
    u16 projectileLifetimeTicks{0U};

    [[nodiscard]] constexpr bool IsParryable() const noexcept {
        return telegraph != AttackTelegraphKind::CrimsonOverloadWave;
    }
    [[nodiscard]] constexpr bool IsRanged() const noexcept {
        return projectileSpeedWu > 0.0f;
    }
    [[nodiscard]] constexpr u16 TotalAnimationTicks() const noexcept {
        return static_cast<u16>(windupTicks + activeTicks + recoveryTicks);
    }
};

inline constexpr std::array<EnemyAttackSpec, 9> kEnemyAttackTable{{
    {EnemyAttackId::None,                  "None",               0U,  0U,  0U,   0U,  0,  0U, AttackTelegraphKind::Standard,            EchoId::None,    true,  0.0f, 0.0f,  0.0f,   0U},
    {EnemyAttackId::DrifterCloseSwipe,     "Close Swipe",       30U,  6U, 34U, 114U, 12,  8U, AttackTelegraphKind::GoldHarmonicWave,    EchoId::Ignis,   true,  1.2f, 1.0f,  0.0f,   0U},
    {EnemyAttackId::SentrySingleBolt,      "Single Bolt",       38U,  1U, 38U, 132U, 10,  6U, AttackTelegraphKind::GoldHarmonicWave,    EchoId::Ventus,  true,  7.0f, 1.0f,  7.0f, 180U},
    {EnemyAttackId::BruteHeavySlam,        "Heavy Slam",        44U,  6U, 44U, 138U, 20, 14U, AttackTelegraphKind::CrimsonOverloadWave, EchoId::None,    false, 1.5f, 1.2f,  0.0f,   0U},
    {EnemyAttackId::BruteShoulderBreak,    "Shoulder Break",    34U,  8U, 40U, 138U, 16, 12U, AttackTelegraphKind::GoldHarmonicWave,    EchoId::Terra,   true,  2.0f, 1.2f,  0.0f,   0U},
    {EnemyAttackId::WardenResonantCleave,  "Resonant Cleave",   34U,  8U, 38U, 102U, 18, 14U, AttackTelegraphKind::GoldHarmonicWave,    EchoId::Terra,   false, 2.0f, 1.4f,  0.0f,   0U},
    {EnemyAttackId::WardenChoirBolts,      "Choir Bolts",       30U,  1U, 42U, 102U, 12, 10U, AttackTelegraphKind::GoldHarmonicWave,    EchoId::Fulgur,  false, 8.0f, 1.5f,  8.0f, 180U},
    {EnemyAttackId::WardenCrimsonCollapse, "Crimson Collapse",  44U, 60U, 44U, 114U, 24, 18U, AttackTelegraphKind::CrimsonOverloadWave, EchoId::None,    false, 4.5f, 0.35f, 9.0f,  30U},
    {EnemyAttackId::WardenRefractedSweep,  "Refracted Sweep",   34U,  8U, 36U, 102U, 16, 12U, AttackTelegraphKind::GoldHarmonicWave,    EchoId::Glacies, false, 2.4f, 1.5f,  0.0f,   0U},
}};

[[nodiscard]] constexpr const EnemyAttackSpec& GetEnemyAttackSpec(EnemyAttackId id) noexcept {
    const u8 idx = static_cast<u8>(id) < kEnemyAttackTable.size() ? static_cast<u8>(id) : 0U;
    return kEnemyAttackTable[idx];
}

} // namespace vots
