#pragma once

#include "game/core/Types.h"

#include <array>
#include <string_view>

namespace en::veil {

// Immutable specification for each of the 12 Lantern Echoes.
// Static const lookup table; zero runtime memory allocation.
struct EchoSpec {
    EchoId id;
    std::string_view name;
    std::string_view lanternTitle;
    std::string_view wavePhysicsName;
    WaveCategory category;
    WavePolarity polarity;
    EchoId counterEcho;
    WeaponArchetype weapon;
    TraversalType traversal;
    f32 baseDamage;
    f32 baseBreak;
    f32 baseReachMeters;
    f32 attackSpeedMult;
    f32 traversalSpeed;
    u16 traversalDurationTicks;
    u16 traversalCooldownTicks;
    f32 wingAssistDamage;
    f32 wingAssistBreak;
    u16 wingAssistCooldownTicks;
};

inline constexpr std::array<EchoSpec, kActiveEchoCount> kEchoPantheon{{
    {EchoId::Ignis, "Ignis", "Wick of Searing Friction", "Thermal Acoustics",
     WaveCategory::ElementalWave, WavePolarity::Oscillation, EchoId::Glacies,
     WeaponArchetype::SearingKatana, TraversalType::BlazeJetDash,
     28.0f, 22.0f, 2.25f, 1.25f, 30.0f, 16, 24, 36.0f, 28.0f, 90},

    {EchoId::Terra, "Terra", "Wick of Tectonic Mass", "Seismic Infrasound",
     WaveCategory::ElementalWave, WavePolarity::Compression, EchoId::Fulgur,
     WeaponArchetype::ExecutionerWarAxe, TraversalType::TectonicQuakePlunge,
     48.0f, 56.0f, 2.65f, 0.85f, 26.0f, 14, 45, 44.0f, 65.0f, 110},

    {EchoId::Fulgur, "Fulgur", "Wick of Piezo Spark", "Piezoelectric Discharge",
     WaveCategory::ElementalWave, WavePolarity::Oscillation, EchoId::Terra,
     WeaponArchetype::VoltTwinDaggers, TraversalType::PiezoBlinkStep,
     21.0f, 18.0f, 1.65f, 1.50f, 38.0f, 4, 28, 32.0f, 26.0f, 75},

    {EchoId::Glacies, "Glacies", "Wick of Frozen Geometry", "Cryo-Cymatics",
     WaveCategory::ElementalWave, WavePolarity::Compression, EchoId::Ignis,
     WeaponArchetype::CryoHalberd, TraversalType::CymaticIceVault,
     34.0f, 32.0f, 3.10f, 1.00f, 15.5f, 12, 38, 38.0f, 40.0f, 95},

    {EchoId::Ventus, "Ventus", "Wick of Shear Cyclones", "Aero-Acoustic Vortex",
     WaveCategory::ElementalWave, WavePolarity::Oscillation, EchoId::Aqua,
     WeaponArchetype::GaleTwinblade, TraversalType::SlipstreamUpdraft,
     24.0f, 20.0f, 2.15f, 1.35f, 16.5f, 16, 36, 30.0f, 25.0f, 80},

    {EchoId::Aqua, "Aqua", "Wick of Imploding Tides", "Hydro-Cavitation",
     WaveCategory::ElementalWave, WavePolarity::Compression, EchoId::Ventus,
     WeaponArchetype::TidalWhipSword, TraversalType::CavitationWireSwing,
     30.0f, 28.0f, 3.85f, 1.10f, 27.0f, 15, 34, 42.0f, 35.0f, 100},

    {EchoId::Lux, "Lux", "Wick of Star-In-A-Wave", "Sonoluminescence",
     WaveCategory::PhenomenonWave, WavePolarity::Compression, EchoId::Umbra,
     WeaponArchetype::RadiantClaymore, TraversalType::PhotonRayGrapple,
     38.0f, 34.0f, 2.85f, 1.05f, 32.0f, 8, 30, 46.0f, 38.0f, 95},

    {EchoId::Umbra, "Umbra", "Wick of Dead Air", "Anechoic Null-Wave",
     WaveCategory::PhenomenonWave, WavePolarity::Oscillation, EchoId::Lux,
     WeaponArchetype::AbyssalGreatScythe, TraversalType::NullPhaseStep,
     33.0f, 26.0f, 2.95f, 1.15f, 22.0f, 12, 32, 35.0f, 45.0f, 105},

    {EchoId::Gravitas, "Gravitas", "Wick of Crushing Nodes", "Acoustic Levitation",
     WaveCategory::PhenomenonWave, WavePolarity::Compression, EchoId::Tempus,
     WeaponArchetype::SingularityMaul, TraversalType::GravitonSlingshot,
     52.0f, 60.0f, 2.50f, 0.80f, 21.0f, 14, 42, 48.0f, 70.0f, 115},

    {EchoId::Tempus, "Tempus", "Wick of Delayed Echoes", "Temporal Delay-Line",
     WaveCategory::PhenomenonWave, WavePolarity::Oscillation, EchoId::Gravitas,
     WeaponArchetype::ChronosOdachi, TraversalType::TemporalAnchorRecall,
     31.0f, 27.0f, 2.75f, 1.18f, 28.0f, 10, 40, 34.0f, 34.0f, 110},

    {EchoId::Aether, "Aether", "Wick of Shared Heartbeats", "Bio-Sympathetic Pulse",
     WaveCategory::PhenomenonWave, WavePolarity::Oscillation, EchoId::Stridor,
     WeaponArchetype::CrimsonRapier, TraversalType::PulseTetherSwap,
     27.0f, 24.0f, 2.40f, 1.30f, 35.0f, 6, 30, 33.0f, 30.0f, 85},

    {EchoId::Stridor, "Stridor", "Wick of Broken Barriers", "Mach Cone Shockwave",
     WaveCategory::PhenomenonWave, WavePolarity::Compression, EchoId::Aether,
     WeaponArchetype::SonicPileGauntlets, TraversalType::MachRecoilPogo,
     42.0f, 52.0f, 1.75f, 1.20f, 18.0f, 10, 24, 50.0f, 68.0f, 90}
}};

[[nodiscard]] constexpr const EchoSpec& GetEchoSpec(EchoId id) noexcept {
    const auto idx = static_cast<u8>(id);
    return kEchoPantheon[idx < kActiveEchoCount ? idx : 0];
}

// Immutable specification for Standing-Wave interference reactions.
// Static const lookup table; zero runtime memory allocation.
struct WaveReactionSpec {
    WaveReactionId id;
    std::string_view name;
    f32 bonusDamage;
    f32 bonusBreak;
    f32 armorShredRatio;
    u16 stunTicks;
    f32 harmonyRefund;
    bool refundsAirDash;
};

inline constexpr std::array<WaveReactionSpec, 12> kWaveReactionTable{{
    {WaveReactionId::None,               "None",                0.0f,   0.0f,  0.00f,  0,  0.0f, false},
    {WaveReactionId::ConstructivePulse,  "Constructive Pulse",  95.0f,  85.0f, 0.00f, 15, 18.0f, true},
    {WaveReactionId::MeltShock,          "Melt-Shock",         165.0f, 110.0f, 0.40f, 20, 22.0f, true},
    {WaveReactionId::PiezoQuake,         "Piezo-Quake",        135.0f, 145.0f, 0.15f, 90, 20.0f, true},
    {WaveReactionId::SteamCavitation,    "Steam Cavitation",   150.0f, 105.0f, 0.20f, 25, 22.0f, true},
    {WaveReactionId::SuperconductorGale, "Superconductor Gale",130.0f, 115.0f, 0.15f, 45, 24.0f, true},
    {WaveReactionId::PyroCyclone,        "Pyro-Cyclone",       155.0f,  95.0f, 0.25f, 20, 20.0f, true},
    {WaveReactionId::TectonicShatter,    "Tectonic Shatter",   175.0f, 180.0f, 0.35f, 60, 25.0f, true},
    {WaveReactionId::EclipseSingularity, "Eclipse Singularity",190.0f, 150.0f, 0.30f, 50, 28.0f, true},
    {WaveReactionId::GravitonQuake,      "Graviton Quake",     160.0f, 170.0f, 0.25f, 75, 24.0f, true},
    {WaveReactionId::ChronoOverdrive,    "Chrono-Overdrive",   185.0f, 140.0f, 0.20f, 40, 26.0f, true},
    {WaveReactionId::SanguineBoil,       "Sanguine Boil",      160.0f, 110.0f, 0.20f, 30, 25.0f, true}
}};

[[nodiscard]] constexpr const WaveReactionSpec& GetWaveReactionSpec(WaveReactionId id) noexcept {
    const auto idx = static_cast<u8>(id);
    return kWaveReactionTable[idx < kWaveReactionTable.size() ? idx : 0];
}

[[nodiscard]] constexpr WaveReactionId ResolveWaveReaction(EchoId first, EchoId second) noexcept {
    if (first == second) {
        return WaveReactionId::None;
    }
    const u16 mask = static_cast<u16>(EchoBit(first) | EchoBit(second));

    if (mask == (EchoBit(EchoId::Ignis) | EchoBit(EchoId::Glacies))) {
        return WaveReactionId::MeltShock;
    }
    if (mask == (EchoBit(EchoId::Terra) | EchoBit(EchoId::Fulgur))) {
        return WaveReactionId::PiezoQuake;
    }
    if (mask == (EchoBit(EchoId::Ignis) | EchoBit(EchoId::Aqua))) {
        return WaveReactionId::SteamCavitation;
    }
    if (mask == (EchoBit(EchoId::Fulgur) | EchoBit(EchoId::Ventus))) {
        return WaveReactionId::SuperconductorGale;
    }
    if (mask == (EchoBit(EchoId::Ignis) | EchoBit(EchoId::Ventus))) {
        return WaveReactionId::PyroCyclone;
    }
    if (mask == (EchoBit(EchoId::Glacies) | EchoBit(EchoId::Terra)) ||
        mask == (EchoBit(EchoId::Glacies) | EchoBit(EchoId::Stridor))) {
        return WaveReactionId::TectonicShatter;
    }
    if (mask == (EchoBit(EchoId::Lux) | EchoBit(EchoId::Umbra))) {
        return WaveReactionId::EclipseSingularity;
    }
    if (mask == (EchoBit(EchoId::Gravitas) | EchoBit(EchoId::Terra))) {
        return WaveReactionId::GravitonQuake;
    }
    if (mask == (EchoBit(EchoId::Tempus) | EchoBit(EchoId::Stridor))) {
        return WaveReactionId::ChronoOverdrive;
    }
    if (mask == (EchoBit(EchoId::Aether) | EchoBit(EchoId::Ignis)) ||
        mask == (EchoBit(EchoId::Aether) | EchoBit(EchoId::Aqua))) {
        return WaveReactionId::SanguineBoil;
    }

    return WaveReactionId::ConstructivePulse;
}

[[nodiscard]] constexpr SacredChordId ResolveSacredChord(
    EchoId lead, EchoId wingL, EchoId wingR) noexcept {
    const u16 mask = static_cast<u16>(EchoBit(lead) | EchoBit(wingL) | EchoBit(wingR));

    constexpr u16 kElementalCataclysm =
        EchoBit(EchoId::Ignis) | EchoBit(EchoId::Terra) | EchoBit(EchoId::Fulgur);
    constexpr u16 kCryoTempest =
        EchoBit(EchoId::Glacies) | EchoBit(EchoId::Ventus) | EchoBit(EchoId::Aqua);
    constexpr u16 kEclipseEternity =
        EchoBit(EchoId::Lux) | EchoBit(EchoId::Umbra) | EchoBit(EchoId::Tempus);
    constexpr u16 kSingularityOverdrive =
        EchoBit(EchoId::Gravitas) | EchoBit(EchoId::Aether) | EchoBit(EchoId::Stridor);

    if (mask == kElementalCataclysm) return SacredChordId::ElementalCataclysm;
    if (mask == kCryoTempest) return SacredChordId::CryoTempestProtocol;
    if (mask == kEclipseEternity) return SacredChordId::EclipseOfEternity;
    if (mask == kSingularityOverdrive) return SacredChordId::SingularityOverdrive;
    return SacredChordId::None;
}

} // namespace en::veil
