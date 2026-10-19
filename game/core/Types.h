#pragma once

#include "core/Types.h"

namespace en::veil {

inline constexpr f32 kFixedTickRate = 120.0f;
inline constexpr f32 kFixedDeltaSeconds = 1.0f / kFixedTickRate;
inline constexpr u8 kActiveEchoCount = 12;
inline constexpr u8 kTriadDeckSize = 3;

enum class GameAction : u8 {
    MoveLeft        = 0,
    MoveRight       = 1,
    LookUp          = 2,
    CrouchDown      = 3,
    Jump            = 4,
    PrimaryAttack   = 5,
    WeaveShift      = 6,
    EvadeDash       = 7,
    DefensiveParry  = 8,
    WingLeftAssist  = 9,
    WingRightAssist = 10,
    DualUnison      = 11,
    TriadUnison     = 12,
    InteractRest    = 13,
    PlaceMapPin     = 14
};

enum class EchoId : u8 {
    Ignis    = 0,
    Terra    = 1,
    Fulgur   = 2,
    Glacies  = 3,
    Ventus   = 4,
    Aqua     = 5,
    Lux      = 6,
    Umbra    = 7,
    Gravitas = 8,
    Tempus   = 9,
    Aether   = 10,
    Stridor  = 11,
    Count    = 12
};

enum class WaveCategory : u8 {
    ElementalWave  = 0,
    PhenomenonWave = 1
};

enum class WavePolarity : u8 {
    Compression = 0,
    Oscillation = 1
};

enum class WeaponArchetype : u8 {
    SearingKatana      = 0,
    ExecutionerWarAxe  = 1,
    VoltTwinDaggers    = 2,
    CryoHalberd        = 3,
    GaleTwinblade      = 4,
    TidalWhipSword     = 5,
    RadiantClaymore    = 6,
    AbyssalGreatScythe = 7,
    SingularityMaul    = 8,
    ChronosOdachi      = 9,
    CrimsonRapier      = 10,
    SonicPileGauntlets = 11
};

enum class TraversalType : u8 {
    BlazeJetDash         = 0,
    TectonicQuakePlunge  = 1,
    PiezoBlinkStep       = 2,
    CymaticIceVault      = 3,
    SlipstreamUpdraft    = 4,
    CavitationWireSwing  = 5,
    PhotonRayGrapple     = 6,
    NullPhaseStep        = 7,
    GravitonSlingshot    = 8,
    TemporalAnchorRecall = 9,
    PulseTetherSwap      = 10,
    MachRecoilPogo       = 11
};

enum class AmplitudeTier : u8 {
    Fundamental = 0,
    Harmonic    = 1,
    Crescendo   = 2
};

enum class WaveReactionId : u8 {
    None                = 0,
    ConstructivePulse   = 1,
    MeltShock           = 2,
    PiezoQuake          = 3,
    SteamCavitation     = 4,
    SuperconductorGale  = 5,
    PyroCyclone         = 6,
    TectonicShatter     = 7,
    EclipseSingularity  = 8,
    GravitonQuake       = 9,
    ChronoOverdrive     = 10,
    SanguineBoil        = 11
};

enum class SacredChordId : u8 {
    None                 = 0,
    ElementalCataclysm   = 1,
    CryoTempestProtocol  = 2,
    EclipseOfEternity    = 3,
    SingularityOverdrive = 4
};

enum class BossTelegraphKind : u8 {
    None            = 0,
    GoldHarmonic    = 1,
    CrimsonOverload = 2
};

enum class BossPosture : u8 {
    Neutral    = 0,
    Dominant   = 1,
    Submissive = 2
};

enum class ActionId : u8 {
    Idle              = 0,
    LightChain1       = 1,
    LightChain2       = 2,
    LightChain3       = 3,
    Launcher          = 4,
    AirChain1         = 5,
    AirChain2         = 6,
    PlungeFinish      = 7,
    WeaveShiftStrike  = 8,
    CrescendoBurst    = 9,
    DefensiveParry    = 10,
    PhaseCounter      = 11,
    DualUnisonCast    = 12,
    TriadUnisonCast   = 13,
    SacredChordCast   = 14,
    Count             = 15
};

enum class MapPinKind : u8 {
    None            = 0,
    FrequencyGate   = 1,
    UnreachedLedge  = 2,
    LoreTablet      = 3,
    BossArena       = 4,
    TuningForkQuest = 5
};

[[nodiscard]] constexpr u16 EchoBit(EchoId id) noexcept {
    return static_cast<u16>(1u << static_cast<u8>(id));
}

// Summary of combat calculations produced during a single simulation tick.
// Value object passed between combat resolution and presentation layers.
struct CombatTickSummary {
    f32 damageDealt{0.0f};
    f32 breakDealt{0.0f};
    WaveReactionId triggeredReaction{WaveReactionId::None};
    SacredChordId triggeredSacredChord{SacredChordId::None};
    bool phaseInversionSucceeded{false};
    bool parryDeflectionSucceeded{false};
    bool crescendoBurstTriggered{false};
};

} // namespace en::veil
