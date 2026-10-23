#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "world/WorldTypes.h"

#include <array>
#include <string_view>

namespace vots {

using en::Color;
using en::EntityId;
using en::f32;
using en::i16;
using en::i32;
using en::i8;
using en::kNullEntity;
using en::u16;
using en::u32;
using en::u64;
using en::u8;
using en::usize;
using en::Vec2;

inline constexpr u32 kEchoCount             = 12U;
inline constexpr u32 kTriadDeckSize         = 3U;
inline constexpr u32 kReactionFormulaCount  = 12U;
inline constexpr u32 kSacredChordCount      = 4U;
inline constexpr u32 kMaxMapPins            = 32U;
inline constexpr u32 kRoomCount             = 8U;
inline constexpr u32 kRegionCount           = 6U;
inline constexpr u32 kMemoryTuningFlagCount = 4U;

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
    None     = 0xFFU,
};

[[nodiscard]] constexpr bool IsValidEcho(EchoId id) noexcept {
    return static_cast<u8>(id) < kEchoCount;
}

enum class EchoWaveCategory : u8 {
    ElementalWave  = 0,
    PhenomenonWave = 1,
};

enum class WaveReactionId : u8 {
    None               = 0,
    MeltShock          = 1,
    PiezoQuake         = 2,
    SteamCavitation    = 3,
    SuperconductorGale = 4,
    PyroCyclone        = 5,
    TectonicShatter    = 6,
    EclipseSingularity = 7,
    GravitonQuake      = 8,
    ChronoOverdrive    = 9,
    SanguineBoil       = 10,
};

enum class SacredChordId : u8 {
    None                 = 0,
    ElementalCataclysm   = 1,
    CryoTempestProtocol  = 2,
    EclipseOfEternity    = 3,
    SingularityOverdrive = 4,
};

enum class WeaponMoveKind : u8 {
    None   = 0,
    Light1 = 1,
    Light2 = 2,
    Light3 = 3,
    Air1   = 4,
    Air2   = 5,
    Heavy  = 6,
};

enum class UnisonActionState : u8 {
    None           = 0,
    StandardUnison = 1,
    SacredChord    = 2,
};

enum class TargetCategory : u8 {
    Standard   = 0,
    HeavyElite = 1,
    Boss       = 2,
};

enum class EnemyArchetype : u8 {
    DissonantDrifter  = 0,
    ChoirSentry       = 1,
    ResonanceBrute    = 2,
    TuningHuskElite   = 3,
    AshSumpWardenBoss = 4,
};

enum class EnemyAttackId : u8 {
    None                  = 0,
    DrifterCloseSwipe     = 1,
    SentrySingleBolt      = 2,
    BruteHeavySlam        = 3,
    BruteShoulderBreak    = 4,
    WardenResonantCleave  = 5,
    WardenChoirBolts      = 6,
    WardenCrimsonCollapse = 7,
    WardenRefractedSweep  = 8,
};

enum class BreakPhase : u8 {
    Normal        = 0,
    Staggered     = 1,
    BreakState    = 2,
    BreakRecovery = 3,
};

enum class ActionLockKind : u8 {
    None         = 0,
    MicroStagger = 1,
    Stun         = 2,
    BreakLock    = 3,
};

enum class AttackTelegraphKind : u8 {
    Standard            = 0,
    GoldHarmonicWave    = 1,
    CrimsonOverloadWave = 2,
};

enum class MapPinKind : u8 {
    None            = 0,
    FrequencyGate   = 1,
    UnreachedLedge  = 2,
    LoreTablet      = 3,
    BossArena       = 4,
    TuningForkQuest = 5,
};

// GDD v0.4/v0.5 Macro-Regions across Vaelen.
enum class RegionId : u8 {
    R0_AshSumpAndMine       = 0,
    R1_GlassConservatory    = 1,
    R2_Choirworks           = 2,
    R3_DrownedArchive       = 3,
    R4_GravityFoundry       = 4,
    R5_SilentReliquarySpire = 5,
};

// GDD v0.5 Section 1 Rule 3 & Section 4: Primary Room Role.
enum class RoomRole : u8 {
    HubAndOnboarding  = 0,
    TraversalAndTest  = 1,
    DiscoveryAndLore  = 2,
    CoordinatedCombat = 3,
    VerticalShaft     = 4,
    BossArena         = 5,
    ShortcutLoop      = 6,
};

// GDD v0.4/v0.5 Four narrative Memory Tuning progression flags.
enum class MemoryTuningFlagId : u8 {
    SurfaceLie = 0,
    TheCollars = 1,
    TheVeil    = 2,
    TheTwelve  = 3,
};

enum class EndingId : u8 {
    None              = 0,
    FalseDawn         = 1,
    ShatteredSpire    = 2,
    SymphonyOfOffBeat = 3,
};

enum class ScreenShakeMode : u8 {
    Off     = 0,
    Reduced = 1,
    Full    = 2,
};

enum class WorldFlagId : u16 {
    AwakenedIgnisLantern     = 1,
    UnlockedGrappleHook      = 2,
    UnlockedPhaseGateKey     = 3,
    ResonatorAshSumpActive   = 4,
    ResonatorSanctumActive   = 5,
    ResonatorVaultActive     = 6,
    ResonatorSpireActive     = 7,
    GateSanctumOpened        = 8,
    GateVaultOpened          = 9,
    GateSpireOpened          = 10,
    LoreTablet01Read         = 11,
    LoreTablet02Read         = 12,
    LoreTablet03Read         = 13,
    MemoryTuningQuest01Done  = 14, // Flag 0: Surface Lie
    MemoryTuningQuest02Done  = 15, // Flag 1: The Collars
    MemoryTuningQuest03Done  = 16, // Flag 2: The Veil
    MemoryTuningComplete     = 17, // All 4 Memory Tuning Flags verified
    BossPhase2Reached        = 18,
    BossPhase3Reached        = 19,
    HighConductorDefeated    = 20,
    AshSumpWardenDefeated    = 20,
    SpireCoreShatteredChoice = 21,
    ShortcutToAshSumpOpened  = 22,
    MemoryTuningQuest04Done  = 23, // Flag 3: The Twelve
    PrismKeeperDefeated      = 24,
    ConductorSealChoirworks  = 25,
    ConductorSealArchive     = 26,
    ConductorSealFoundry     = 27,
    LoreTablet04Read         = 28,
};

enum class GameAction : u8 {
    Jump          = 0,
    AttackLight   = 1,
    AttackHeavy   = 2,
    Dash          = 3,
    Parry         = 4,
    SelectEcho1   = 5,
    SelectEcho2   = 6,
    SelectEcho3   = 7,
    CycleEchoNext = 8,
    CycleEchoPrev = 9,
    UnisonBurst   = 10,
    SacredChord   = 11,
    Interact      = 12,
    PlaceMapPin   = 13,
    ToggleMap     = 14,
    Pause         = 15,
    MenuConfirm   = 12,
    MenuBack      = 15,
};

// Stores the 3-slot Echo configuration managed at a Tuning Resonator and switched freely in combat.
// Value type; all operations are pure and allocation-free.
// Invariant: activeSlot < kTriadDeckSize and every populated slot holds a valid EchoId in [0..11].
struct alignas(4) TriadDeck {
    std::array<EchoId, kTriadDeckSize> slots{EchoId::Ignis, EchoId::Glacies, EchoId::Terra};
    u8 activeSlot{0U};

    [[nodiscard]] constexpr EchoId ActiveEcho() const noexcept {
        return slots[activeSlot < kTriadDeckSize ? activeSlot : 0U];
    }

    constexpr void CycleNext() noexcept {
        activeSlot = static_cast<u8>((activeSlot + 1U) % kTriadDeckSize);
    }

    constexpr void CyclePrev() noexcept {
        activeSlot = static_cast<u8>((activeSlot + kTriadDeckSize - 1U) % kTriadDeckSize);
    }

    [[nodiscard]] constexpr bool ConfigureAtResonator(
        EchoId s0,
        EchoId s1,
        EchoId s2,
        u16 unlockedEchoMask) noexcept {
        if (!IsValidEcho(s0) || !IsValidEcho(s1) || !IsValidEcho(s2)) return false;
        if (s0 == s1 || s0 == s2 || s1 == s2) return false;
        const u16 reqMask = static_cast<u16>(
            (1U << static_cast<u8>(s0)) |
            (1U << static_cast<u8>(s1)) |
            (1U << static_cast<u8>(s2)));
        if ((unlockedEchoMask & reqMask) != reqMask) return false;
        slots[0] = s0;
        slots[1] = s1;
        slots[2] = s2;
        if (activeSlot >= kTriadDeckSize) activeSlot = 0U;
        return true;
    }

    [[nodiscard]] constexpr u16 EquippedMask() const noexcept {
        u16 mask = 0U;
        for (const EchoId e : slots) {
            if (IsValidEcho(e)) {
                mask |= static_cast<u16>(1U << static_cast<u8>(e));
            }
        }
        return mask;
    }

    [[nodiscard]] constexpr bool operator==(const TriadDeck&) const noexcept = default;
};
static_assert(sizeof(TriadDeck) == 4);

// Tracks the single active Lost Wick Husk spawned upon Kaelen's death per GDD v0.2–v0.5.
struct alignas(8) LostWickHuskRecord {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    u32 bankedTuningShards{0U};
    u8 roomId{0U};
    u8 active{0U};
    u16 reserved{0U};

    [[nodiscard]] constexpr bool IsActive() const noexcept { return active != 0U; }
    [[nodiscard]] constexpr bool operator==(const LostWickHuskRecord&) const noexcept = default;
};
static_assert(sizeof(LostWickHuskRecord) == 16);

// Player-authored map annotation pin stored per discovered room coordinate.
struct alignas(4) MapPinRecord {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    u8 roomId{0U};
    MapPinKind kind{MapPinKind::None};
    u16 noteTag{0U};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return kind != MapPinKind::None; }
    [[nodiscard]] constexpr bool operator==(const MapPinRecord&) const noexcept = default;
};
static_assert(sizeof(MapPinRecord) == 12);

// Audio & Accessibility runtime configuration per GDD v0.3 Section 5.
struct alignas(16) AccessibilityConfig {
    f32 masterVolume{1.0f};
    f32 musicVolume{0.85f};
    f32 sfxVolume{1.0f};
    f32 combatCueVolume{1.0f};
    f32 analogDeadzone{0.18f};
    f32 triggerThreshold{0.55f};
    ScreenShakeMode screenShake{ScreenShakeMode::Full};
    u8 reducedFlash{0U};
    u8 reducedVfx{0U};
    u8 subtitlesEnabled{1U};
    u32 reserved{0U};
};
static_assert(sizeof(AccessibilityConfig) == 32);

} // namespace vots
