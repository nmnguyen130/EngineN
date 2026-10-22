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

inline constexpr u32 kEchoCount = 12U;
inline constexpr u32 kTriadDeckSize = 3U;
inline constexpr u32 kReactionFormulaCount = 12U;
inline constexpr u32 kSacredChordCount = 4U;
inline constexpr u32 kMaxMapPins = 32U;
inline constexpr u32 kRoomCount = 4U;

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

enum class TargetCategory : u8 {
    Standard   = 0,
    HeavyElite = 1,
    Boss       = 2,
};

enum class BreakPhase : u8 {
    Normal        = 0,
    Staggered     = 1, // Standard enemy 45 ticks; Break ignored while active, resets to 0 on exit
    BreakState    = 2, // Heavy/Elite & Boss 150 ticks; Break resets to 0 on entry, 1.25x HP Damage
    BreakRecovery = 3, // Heavy/Elite & Boss 240 ticks after BreakState; Break ignored
};

// Action-locking CC priority order per GDD v0.2 Section 4.3: BreakState > Stun > MicroStagger.
enum class ActionLockKind : u8 {
    None         = 0,
    MicroStagger = 1,
    Stun         = 2,
    BreakLock    = 3,
};

enum class AttackTelegraphKind : u8 {
    Standard            = 0,
    GoldHarmonicWave    = 1, // Parryable within 10 ticks
    CrimsonOverloadWave = 2, // Unblockable / unparryable; must evade
};

enum class MapPinKind : u8 {
    None            = 0,
    FrequencyGate   = 1,
    UnreachedLedge  = 2,
    LoreTablet      = 3,
    BossArena       = 4,
    TuningForkQuest = 5,
};

enum class EndingId : u8 {
    None              = 0,
    FalseDawn         = 1, // Ending A: Escape to surface without dismantling network
    ShatteredSpire    = 2, // Ending B: Destroy Spire core, severing Tuning Collars
    SymphonyOfOffBeat = 3, // Ending C: All 12 Echoes + Memory Tuning complete
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
    MemoryTuningQuest01Done  = 14,
    MemoryTuningQuest02Done  = 15,
    MemoryTuningQuest03Done  = 16,
    MemoryTuningComplete     = 17,
    BossPhase2Reached        = 18,
    BossPhase3Reached        = 19,
    HighConductorDefeated    = 20,
    SpireCoreShatteredChoice = 21,
};

enum class GameAction : u8 {
    Jump          = 0,
    AttackLight   = 1,
    AttackHeavy   = 2,
    Dash          = 3,
    Parry         = 4,
    CycleEchoNext = 5,
    CycleEchoPrev = 6,
    UnisonBurst   = 7,
    SacredChord   = 8,
    Interact      = 9,
    PlaceMapPin   = 10,
    ToggleMap     = 11,
    Pause         = 12,
    MenuConfirm   = 13,
    MenuBack      = 14,
};

// Stores the 3-slot Echo configuration managed at a Tuning Resonator and cycled freely in combat.
// Value type; all operations are pure and allocation-free.
// Invariant: activeSlot < kTriadDeckSize and every populated slot holds a valid EchoId in [0..11].
struct alignas(4) TriadDeck {
    std::array<EchoId, kTriadDeckSize> slots{EchoId::Ignis, EchoId::Glacies, EchoId::Fulgur};
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

// Tracks the single active Lost Wick Husk spawned upon Kaelen's death per GDD v0.2 Section 5.2.
// Value type persisted inside SaveData and MetroidvaniaSystem.
// Invariant: at most one Husk exists at any time; subsequent deaths before recovery add shards without moving the Husk.
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

} // namespace vots
