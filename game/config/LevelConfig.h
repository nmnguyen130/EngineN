#pragma once

#include "game/core/GameTypes.h"

namespace vots {

// Default off-screen edge transition thresholds for baseline 36-tile-wide rooms (Room 0: [-12..24]).
inline constexpr f32 kRoomWestExitTriggerX   = -11.65f;
inline constexpr f32 kRoomEastExitTriggerX   = 23.65f;
inline constexpr f32 kRoomWestEntranceSpawnX = -9.6f;
inline constexpr f32 kRoomEastEntranceSpawnX = 21.6f;

// GDD v0.4 & v0.5 Macro-Region Sheet Specification.
struct RegionSheetDef {
    RegionId id{RegionId::R0_AshSumpAndMine};
    std::string_view codeName{};
    std::string_view displayName{};
    EchoId plannedEchoA{EchoId::Ignis};
    EchoId plannedEchoB{EchoId::Terra};
    std::string_view bossName{};
    std::string_view progressionUnlock{};
    std::string_view narrativeEvidence{};
    Color skyTopColor{0.10f, 0.12f, 0.18f, 1.0f};
    Color skyBottomColor{0.05f, 0.06f, 0.10f, 1.0f};
};

inline constexpr std::array<RegionSheetDef, kRegionCount> kRegionSheetTable{{
    {RegionId::R0_AshSumpAndMine,
     "R0", "Ash-Sump Slums & Collapsed Mine",
     EchoId::Ignis, EchoId::Terra,
     "Ash-Sump Warden", "Opens Lower Junction",
     "Tuning Collars classify citizens by synchronization capacity rather than protecting them.",
     Color{0.12f, 0.13f, 0.20f, 1.0f}, Color{0.05f, 0.06f, 0.10f, 1.0f}},
    {RegionId::R1_GlassConservatory,
     "R1", "Glass Conservatory",
     EchoId::Glacies, EchoId::Lux,
     "Prism Keeper", "Opens 3 Midgame Branches (Choirworks / Archive / Foundry)",
     "Physical botanical & solar lenses prove the surface was never destroyed as claimed.",
     Color{0.10f, 0.20f, 0.28f, 1.0f}, Color{0.05f, 0.09f, 0.14f, 1.0f}},
    {RegionId::R2_Choirworks,
     "R2", "Choirworks",
     EchoId::Fulgur, EchoId::Stridor,
     "Relay Cantor", "Grants Conductor Seal I (1/3)",
     "Acoustic conduits route citizen voices and extracted memories directly through Tuning Collars.",
     Color{0.16f, 0.14f, 0.24f, 1.0f}, Color{0.06f, 0.06f, 0.12f, 1.0f}},
    {RegionId::R3_DrownedArchive,
     "R3", "Drowned Archive",
     EchoId::Aqua, EchoId::Aether,
     "Drowned Curator", "Grants Conductor Seal II (2/3)",
     "Submerged wax cylinders reveal edited citizen memories that contradict official Vaelen history.",
     Color{0.07f, 0.16f, 0.26f, 1.0f}, Color{0.04f, 0.07f, 0.13f, 1.0f}},
    {RegionId::R4_GravityFoundry,
     "R4", "Gravity Foundry",
     EchoId::Ventus, EchoId::Gravitas,
     "Weightless Bailiff", "Grants Conductor Seal III (3/3)",
     "Foundry logs chronicle the Twelve Resonators' rebellion and the Veil's catastrophic overload.",
     Color{0.16f, 0.11f, 0.18f, 1.0f}, Color{0.06f, 0.05f, 0.09f, 1.0f}},
    {RegionId::R5_SilentReliquarySpire,
     "R5", "Silent Reliquary & Spire of Unison",
     EchoId::Umbra, EchoId::Tempus,
     "High Conductor", "Unlocks Final Spire Core & Ending Choice (A / B / C)",
     "The Twelve living anchors sustain the Veil; the Pharos Lantern was forged to sever or retune it.",
     Color{0.20f, 0.09f, 0.24f, 1.0f}, Color{0.06f, 0.04f, 0.10f, 1.0f}},
}};

[[nodiscard]] constexpr const RegionSheetDef& GetRegionSheet(RegionId id) noexcept {
    const u8 idx = static_cast<u8>(id) < kRegionSheetTable.size() ? static_cast<u8>(id) : 0U;
    return kRegionSheetTable[idx];
}

// GDD v0.4 Section 4: Echo Unlock Cadence Milestones.
struct EchoCadenceMilestone {
    EchoId echo{EchoId::Ignis};
    RegionId region{RegionId::R0_AshSumpAndMine};
    u8 roomId{0U};
    std::string_view milestoneName{};
};

inline constexpr std::array<EchoCadenceMilestone, kEchoCount> kEchoUnlockCadenceTable{{
    {EchoId::Ignis,    RegionId::R0_AshSumpAndMine,       0U, "R0-05 Lantern Chamber Awakening"},
    {EchoId::Terra,    RegionId::R0_AshSumpAndMine,       1U, "R0-08 After Ash-Sump Warden / Lower Junction"},
    {EchoId::Glacies,  RegionId::R1_GlassConservatory,    2U, "R1-03 Refraction Gallery"},
    {EchoId::Lux,      RegionId::R1_GlassConservatory,    2U, "R1-05 Conservator's Court"},
    {EchoId::Fulgur,   RegionId::R2_Choirworks,           3U, "R2-02 Resonance Conveyor"},
    {EchoId::Stridor,  RegionId::R2_Choirworks,           3U, "R2-04 Signal Tower"},
    {EchoId::Aqua,     RegionId::R3_DrownedArchive,       4U, "R3-02 Stacks Below Waterline"},
    {EchoId::Aether,   RegionId::R3_DrownedArchive,       4U, "R3-04 Memory Vault"},
    {EchoId::Ventus,   RegionId::R4_GravityFoundry,       5U, "R4-01 Intake Shaft"},
    {EchoId::Gravitas, RegionId::R4_GravityFoundry,       5U, "R4-04 Calibration Hall"},
    {EchoId::Umbra,    RegionId::R5_SilentReliquarySpire, 6U, "R5-02 Twelve Niches"},
    {EchoId::Tempus,   RegionId::R5_SilentReliquarySpire, 7U, "R5-04 Spire Ascent"},
}};

// GDD v0.4/v0.5 Four Narrative Memory Tuning Flags (each backed by dual independent clues per GDD v0.5 Rule 5).
struct MemoryTuningSpec {
    MemoryTuningFlagId id{MemoryTuningFlagId::SurfaceLie};
    WorldFlagId completionWorldFlag{WorldFlagId::MemoryTuningQuest01Done};
    WorldFlagId secondaryEvidenceFlag{WorldFlagId::LoreTablet01Read};
    std::string_view title{};
    std::string_view journalSummary{};
};

inline constexpr std::array<MemoryTuningSpec, kMemoryTuningFlagCount> kMemoryTuningSpecTable{{
    {MemoryTuningFlagId::SurfaceLie,
     WorldFlagId::MemoryTuningQuest01Done, WorldFlagId::LoreTablet01Read,
     "1. Surface Lie",
     "Surface sunlight & living flora in the Glass Conservatory prove access to the surface was concealed."},
    {MemoryTuningFlagId::TheCollars,
     WorldFlagId::MemoryTuningQuest02Done, WorldFlagId::LoreTablet02Read,
     "2. The Collars",
     "Choirworks schematics confirm Tuning Collars harvest citizen identity and vocal resonance."},
    {MemoryTuningFlagId::TheVeil,
     WorldFlagId::MemoryTuningQuest03Done, WorldFlagId::LoreTablet03Read,
     "3. The Veil",
     "Drowned Archive cylinders show the Veil was a collective suppression protocol whose failure turned catastrophic."},
    {MemoryTuningFlagId::TheTwelve,
     WorldFlagId::MemoryTuningQuest04Done, WorldFlagId::LoreTablet04Read,
     "4. The Twelve",
     "Gravity Foundry & Reliquary imprints distinguish the Twelve imprisoned consciousnesses from their Echoes."},
}};

// GDD v0.5 Section 2–4: Room Specification Template with Variable Metroidvania Room Dimensions & Screen-Edge Exits.
struct RoomTopologySpec {
    u8 roomId{0U};
    RegionId region{RegionId::R0_AshSumpAndMine};
    RoomRole role{RoomRole::HubAndOnboarding};
    u16 tileWidth{36U};
    u16 tileHeight{12U};
    f32 originX{-12.0f};
    f32 originY{-1.0f};
    u8 westNeighborRoomId{0xFFU};
    u8 eastNeighborRoomId{0xFFU};
    u8 maxSimultaneousEnemies{2U};
    std::string_view gddRoomCode{};
    std::string_view hudTitle{};
    std::string_view landmarkName{};
    std::string_view inferredObjective{};

    [[nodiscard]] constexpr f32 MinWorldX() const noexcept { return originX; }
    [[nodiscard]] constexpr f32 MaxWorldX() const noexcept { return originX + static_cast<f32>(tileWidth); }
    [[nodiscard]] constexpr f32 MinWorldY() const noexcept { return originY; }
    [[nodiscard]] constexpr f32 MaxWorldY() const noexcept { return originY + static_cast<f32>(tileHeight); }

    // True Hollow Knight screen-edge transition thresholds:
    // Only trigger when Kaelen walks all the way through the outer boundary passage to the very edge of the camera/screen!
    [[nodiscard]] constexpr f32 WestExitThresholdX() const noexcept {
        return MinWorldX() + 0.35f;
    }
    [[nodiscard]] constexpr f32 EastExitThresholdX() const noexcept {
        return MaxWorldX() - 0.35f;
    }
    [[nodiscard]] constexpr f32 WestEntranceSpawnX() const noexcept {
        return MinWorldX() + 2.4f;
    }
    [[nodiscard]] constexpr f32 EastEntranceSpawnX() const noexcept {
        return MaxWorldX() - 2.4f;
    }
};

inline constexpr std::array<RoomTopologySpec, kRoomCount> kRoomTopologyTable{{
    {0U, RegionId::R0_AshSumpAndMine, RoomRole::HubAndOnboarding,
     36U, 12U, -12.0f, -1.0f, 0xFFU, 1U, 2U,
     "R0-01..05", "R0: ASH-SUMP & LANTERN HUB", "Pharos Resonator & Sluice Gate",
     "Rest at the Resonator [E] or walk East out the screen edge into the Collapsed Mine"},
    {1U, RegionId::R0_AshSumpAndMine, RoomRole::TraversalAndTest,
     44U, 15U, -12.0f, -1.0f, 0U, 2U, 2U,
     "R0-03..06", "R0: BROKEN LIFT & MINE CROSSING", "Collar Row & Resonance Test",
     "Test Resonance Marks on the Drifter/Sentry pair and continue East to Lower Junction"},
    {2U, RegionId::R1_GlassConservatory, RoomRole::DiscoveryAndLore,
     50U, 13U, -12.0f, -1.0f, 1U, 3U, 2U,
     "R1-01..07", "R1: GLASS CONSERVATORY", "Refraction Gallery & Solar Dome",
     "Inspect the Surface Lie botanical lens and traverse the long crystal gallery East"},
    {3U, RegionId::R2_Choirworks, RoomRole::CoordinatedCombat,
     42U, 16U, -12.0f, -1.0f, 2U, 4U, 2U,
     "R2-01..06", "R2: CHOIRWORKS SIGNAL TOWER", "Acoustic Loom & Relay Dynamo",
     "Defeat the coordinated patrol, claim Conductor Seal I [E], or advance East"},
    {4U, RegionId::R3_DrownedArchive, RoomRole::DiscoveryAndLore,
     48U, 14U, -12.0f, -1.0f, 3U, 5U, 2U,
     "R3-01..06", "R3: DROWNED ARCHIVE", "Submerged Cylinder Stacks",
     "Cross the upper dry catwalks, verify The Veil memory records, and claim Seal II"},
    {5U, RegionId::R4_GravityFoundry, RoomRole::VerticalShaft,
     32U, 22U, -12.0f, -1.0f, 4U, 6U, 1U,
     "R4-01..06", "R4: GRAVITY FOUNDRY SHAFT", "Vertical Intake Shaft & Mass Core",
     "Ascend the 22m vertical shaft platforms or rest at the Foundry Resonator before the Arena"},
    {6U, RegionId::R5_SilentReliquarySpire, RoomRole::BossArena,
     40U, 14U, -12.0f, -1.0f, 5U, 7U, 1U,
     "R0-08/R5-05", "R5: WARDEN & SPIRE ARENA", "Sanctum of the Twelve Anchors",
     "Parry Gold Cleaves [F], jump/dash over Crimson Waves, and activate the Spire Core [E]"},
    {7U, RegionId::R5_SilentReliquarySpire, RoomRole::ShortcutLoop,
     34U, 14U, -12.0f, -1.0f, 6U, 0U, 0U,
     "R0-09/R5-06", "R5: RETURN SHAFT SHORTCUT", "One-Way Lift to Ash-Sump Hub",
     "Pull the Shortcut Lever [E] and walk out the East screen edge to loop back to R0 Hub"},
}};

[[nodiscard]] constexpr const RoomTopologySpec& GetRoomTopology(u8 roomId) noexcept {
    return kRoomTopologyTable[roomId < kRoomCount ? roomId : 0U];
}

} // namespace vots
