#pragma once

#include "game/core/GameTypes.h"

namespace vots {

inline constexpr u16 kResonanceMarkDurationTicks   = 420U;
inline constexpr u16 kHeavyAttackStartupTicks      = 24U;
inline constexpr u8  kUnisonStartupTicks           = 12U;
inline constexpr u8  kUnisonResolveTick            = 12U;
inline constexpr u8  kUnisonRecoveryTicks          = 18U;
inline constexpr u8  kUnisonTotalTicks             = 31U;
inline constexpr u8  kSacredChordStartupTicks      = 24U;
inline constexpr u8  kSacredChordResolveTick       = 24U;
inline constexpr u8  kSacredChordRecoveryTicks     = 36U;
inline constexpr u8  kSacredChordTotalTicks        = 61U;

inline constexpr u16 kStandardStaggerTicks         = 45U;
inline constexpr u16 kEliteBossBreakStateTicks     = 150U;
inline constexpr u16 kEliteBossBreakRecovTicks     = 240U;
inline constexpr u16 kFractureDurationTicks        = 720U;
inline constexpr u16 kPiezoQuakeStunTicks          = 42U;
inline constexpr u16 kTectonicMicroStaggerTicks    = 30U;
inline constexpr u16 kGuardBrokenLockoutTicks      = 90U;
inline constexpr u16 kEclipsePullDurationTicks     = 12U;
inline constexpr u16 kChronoOverdriveDelayTicks    = 12U;
inline constexpr u16 kUnisonInterruptTicks         = 30U;
inline constexpr u16 kCryoTempestSlowTicks         = 180U;
inline constexpr u16 kEclipseChordStunTicks        = 72U;
inline constexpr u16 kSingularityPullTicks         = 36U;

inline constexpr u8  kMaxBreakMeter                = 100U;
inline constexpr u8  kMaxHarmonyGauge              = 100U;
inline constexpr u8  kUnisonHarmonyCost            = 50U;
inline constexpr u8  kSacredChordHarmonyCost       = 100U;
inline constexpr u8  kReactionHarmonyGain          = 10U;
inline constexpr u8  kParryHarmonyGain             = 15U;
inline constexpr u8  kParryBaseBreakDamage         = 18U;
inline constexpr u8  kParryPhaseInversionBonus     = 12U;

inline constexpr f32 kFractureDamageMultiplier     = 1.15f;
inline constexpr f32 kBreakStateDamageMultiplier   = 1.25f;

// Immutable presentation and lore metadata for each of the 12 Echoes in GDD v0.2–v0.4.
struct EchoIdentityDef {
    EchoId id{EchoId::Ignis};
    EchoWaveCategory category{EchoWaveCategory::ElementalWave};
    std::string_view name{};
    std::string_view glyph{};
    std::string_view resonanceType{};
    Color primaryColor{1.0f, 1.0f, 1.0f, 1.0f};
    EchoId oppositeCounter{EchoId::None};
};

inline constexpr std::array<EchoIdentityDef, kEchoCount> kEchoTable{{
    {EchoId::Ignis,    EchoWaveCategory::ElementalWave,  "Ignis",    "IGN", "Thermal",          Color{1.00f, 0.42f, 0.17f, 1.0f}, EchoId::Glacies},
    {EchoId::Terra,    EchoWaveCategory::ElementalWave,  "Terra",    "TER", "Seismic",          Color{0.83f, 0.63f, 0.22f, 1.0f}, EchoId::Fulgur},
    {EchoId::Fulgur,   EchoWaveCategory::ElementalWave,  "Fulgur",   "FUL", "Piezoelectric",    Color{0.16f, 0.94f, 1.00f, 1.0f}, EchoId::Terra},
    {EchoId::Glacies,  EchoWaveCategory::ElementalWave,  "Glacies",  "GLA", "Cryo-crystalline", Color{0.42f, 0.88f, 1.00f, 1.0f}, EchoId::Ignis},
    {EchoId::Ventus,   EchoWaveCategory::ElementalWave,  "Ventus",   "VEN", "Aero-acoustic",    Color{0.18f, 0.90f, 0.66f, 1.0f}, EchoId::Aqua},
    {EchoId::Aqua,     EchoWaveCategory::ElementalWave,  "Aqua",     "AQU", "Hydro-cavitation", Color{0.17f, 0.41f, 1.00f, 1.0f}, EchoId::Ventus},
    {EchoId::Lux,      EchoWaveCategory::PhenomenonWave, "Lux",      "LUX", "Photonic",         Color{1.00f, 0.94f, 0.66f, 1.0f}, EchoId::Umbra},
    {EchoId::Umbra,    EchoWaveCategory::PhenomenonWave, "Umbra",    "UMB", "Null",             Color{0.52f, 0.28f, 0.78f, 1.0f}, EchoId::Lux},
    {EchoId::Gravitas, EchoWaveCategory::PhenomenonWave, "Gravitas", "GRV", "Gravitational",    Color{0.35f, 0.32f, 0.92f, 1.0f}, EchoId::Tempus},
    {EchoId::Tempus,   EchoWaveCategory::PhenomenonWave, "Tempus",   "TMP", "Temporal delay",   Color{0.72f, 0.91f, 0.96f, 1.0f}, EchoId::Gravitas},
    {EchoId::Aether,   EchoWaveCategory::PhenomenonWave, "Aether",   "AET", "Bio-sympathetic",  Color{0.88f, 0.14f, 0.28f, 1.0f}, EchoId::Stridor},
    {EchoId::Stridor,  EchoWaveCategory::PhenomenonWave, "Stridor",  "STR", "Shockwave",        Color{0.90f, 1.00f, 1.00f, 1.0f}, EchoId::Aether},
}};

[[nodiscard]] constexpr const EchoIdentityDef& GetEchoDef(EchoId id) noexcept {
    const u8 idx = IsValidEcho(id) ? static_cast<u8>(id) : 0U;
    return kEchoTable[idx];
}

// Shared Resonance Blade move statistics per GDD v0.3 Section 3.4, scaled to match the 96x96 (2.40 wu) sword swings.
struct WeaponMoveSpec {
    WeaponMoveKind kind{WeaponMoveKind::None};
    i16 hpDamage{0};
    u8 breakDamage{0U};
    u8 startupTicks{0U};
    u8 activeTicks{0U};
    u8 recoveryTicks{0U};
    f32 reachWidthMeters{1.70f};
    f32 reachHeightMeters{1.20f};
    f32 offsetX{0.92f};
    f32 offsetY{0.0f};

    [[nodiscard]] constexpr u16 TotalTicks() const noexcept {
        return static_cast<u16>(startupTicks + activeTicks + recoveryTicks);
    }
};

inline constexpr std::array<WeaponMoveSpec, 7> kResonanceBladeMoves{{
    {WeaponMoveKind::None,   0,  0U,  0U, 0U,  0U, 0.00f, 0.00f, 0.00f, 0.0f},
    {WeaponMoveKind::Light1, 10, 4U,  6U, 6U, 14U, 1.90f, 1.55f, 1.05f, 0.0f},
    {WeaponMoveKind::Light2, 12, 5U,  7U, 6U, 15U, 2.00f, 1.65f, 1.10f, 0.0f},
    {WeaponMoveKind::Light3, 18, 9U,  9U, 8U, 19U, 2.25f, 1.80f, 1.22f, 0.0f},
    {WeaponMoveKind::Air1,   10, 4U,  6U, 6U, 12U, 1.90f, 1.60f, 1.02f, 0.0f},
    {WeaponMoveKind::Air2,   15, 7U,  8U, 6U, 16U, 2.05f, 1.70f, 1.10f, 0.0f},
    {WeaponMoveKind::Heavy,  24, 18U, 24U, 8U, 20U, 2.45f, 1.90f, 1.30f, 0.0f},
}};

[[nodiscard]] constexpr const WeaponMoveSpec& GetWeaponMoveSpec(WeaponMoveKind kind) noexcept {
    const u8 idx = static_cast<u8>(kind) < kResonanceBladeMoves.size() ? static_cast<u8>(kind) : 0U;
    return kResonanceBladeMoves[idx];
}

// Exact specification of the 10 named Signature Reactions and 12 unordered Echo pairs (GDD v0.2–v0.4).
struct WaveReactionSpec {
    WaveReactionId id{WaveReactionId::None};
    EchoId echoA{EchoId::None};
    EchoId echoB{EchoId::None};
    std::string_view name{};
    i16 bonusHpDamage{0};
    u8 bonusBreak{0U};
    u8 maxSecondaryTargets{0U};
    f32 secondaryRadiusMeters{0.0f};
    i16 secondaryHpDamage{0};
    u8 secondaryBreak{0U};
};

inline constexpr std::array<WaveReactionSpec, kReactionFormulaCount> kReactionFormulaTable{{
    {WaveReactionId::MeltShock,          EchoId::Ignis,    EchoId::Glacies, "Melt-Shock",          24, 16U, 0U, 0.00f,  0, 0U},
    {WaveReactionId::PiezoQuake,         EchoId::Terra,    EchoId::Fulgur,  "Piezo-Quake",         20, 22U, 0U, 0.00f,  0, 0U},
    {WaveReactionId::SteamCavitation,    EchoId::Ignis,    EchoId::Aqua,    "Steam Cavitation",    22, 12U, 2U, 1.75f,  8, 0U},
    {WaveReactionId::SuperconductorGale, EchoId::Fulgur,   EchoId::Ventus,  "Superconductor Gale", 18, 16U, 2U, 2.50f,  9, 4U},
    {WaveReactionId::PyroCyclone,        EchoId::Ignis,    EchoId::Ventus,  "Pyro-Cyclone",        20, 10U, 4U, 2.50f, 12, 0U},
    {WaveReactionId::TectonicShatter,    EchoId::Glacies,  EchoId::Terra,   "Tectonic Shatter",    28, 28U, 0U, 0.00f,  0, 0U},
    {WaveReactionId::TectonicShatter,    EchoId::Glacies,  EchoId::Stridor, "Tectonic Shatter",    28, 28U, 0U, 0.00f,  0, 0U},
    {WaveReactionId::EclipseSingularity, EchoId::Lux,      EchoId::Umbra,   "Eclipse Singularity", 26, 18U, 4U, 3.50f,  0, 0U},
    {WaveReactionId::GravitonQuake,      EchoId::Gravitas, EchoId::Terra,   "Graviton Quake",      22, 25U, 4U, 2.00f,  8, 0U},
    {WaveReactionId::ChronoOverdrive,    EchoId::Tempus,   EchoId::Stridor, "Chrono-Overdrive",    20, 14U, 0U, 0.00f, 10, 0U},
    {WaveReactionId::SanguineBoil,       EchoId::Aether,   EchoId::Ignis,   "Sanguine Boil",       18, 12U, 2U, 2.00f,  8, 0U},
    {WaveReactionId::SanguineBoil,       EchoId::Aether,   EchoId::Aqua,    "Sanguine Boil",       18, 12U, 2U, 2.00f,  8, 0U},
}};

[[nodiscard]] constexpr const WaveReactionSpec* FindWaveReaction(EchoId first, EchoId second) noexcept {
    if (!IsValidEcho(first) || !IsValidEcho(second) || first == second) return nullptr;
    for (const WaveReactionSpec& spec : kReactionFormulaTable) {
        if ((spec.echoA == first && spec.echoB == second) ||
            (spec.echoA == second && spec.echoB == first)) {
            return &spec;
        }
    }
    return nullptr;
}

// Exact specification of the 4 Sacred Chords (GDD v0.2–v0.4).
struct SacredChordSpec {
    SacredChordId id{SacredChordId::None};
    EchoId e0{EchoId::None};
    EchoId e1{EchoId::None};
    EchoId e2{EchoId::None};
    std::string_view name{};
    f32 radiusMeters{4.0f};
    u8 maxTargets{5U};
    i16 totalHpDamage{0};
    u8 breakDamage{0U};

    [[nodiscard]] constexpr u16 RequiredMask() const noexcept {
        return static_cast<u16>(
            (1U << static_cast<u8>(e0)) |
            (1U << static_cast<u8>(e1)) |
            (1U << static_cast<u8>(e2)));
    }
};

inline constexpr std::array<SacredChordSpec, kSacredChordCount> kSacredChordTable{{
    {SacredChordId::ElementalCataclysm,   EchoId::Ignis,    EchoId::Terra,   EchoId::Fulgur,  "Elemental Cataclysm",   4.0f, 5U, 30, 24U},
    {SacredChordId::CryoTempestProtocol,  EchoId::Glacies,  EchoId::Ventus,  EchoId::Aqua,    "Cryo-Tempest Protocol", 4.0f, 5U, 22, 18U},
    {SacredChordId::EclipseOfEternity,    EchoId::Lux,      EchoId::Umbra,   EchoId::Tempus,  "Eclipse of Eternity",   4.0f, 3U, 24, 18U},
    {SacredChordId::SingularityOverdrive, EchoId::Gravitas, EchoId::Aether,  EchoId::Stridor, "Singularity Overdrive", 4.5f, 5U, 28, 25U},
}};

[[nodiscard]] constexpr const SacredChordSpec* FindMatchingSacredChord(const TriadDeck& deck) noexcept {
    const u16 mask = deck.EquippedMask();
    for (const SacredChordSpec& spec : kSacredChordTable) {
        if (spec.RequiredMask() == mask) {
            return &spec;
        }
    }
    return nullptr;
}

} // namespace vots
