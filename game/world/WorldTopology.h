#pragma once

#include "game/core/GameConfig.h"
#include "gameplay/GameplayPipeline.h"
#include "world/World.h"

namespace vots {

// Evaluates GDD v0.4 Macro-Region topology, seamless room-boundary exits, flexible midgame branch seals,
// and readable main-path Journal objectives independent of equipped Echoes.
class WorldTopology {
public:
    // Checks if Kaelen has walked across the West or East room boundary and returns the connected roomId (0xFFU if none).
    [[nodiscard]] static u8 CheckSeamlessBoundaryExit(
        u8 currentRoomId,
        Vec2 playerPos,
        const en::GameplayPipeline& pipeline) noexcept;

    // Grants the Conductor Seal for one of the three flexible midgame branches (Choirworks, Drowned Archive, Gravity Foundry).
    static void GrantMidgameBranchSeal(
        RegionId branchRegion,
        en::GameplayPipeline& pipeline) noexcept;

    // Returns the number of collected Conductor Seals in [0..3].
    [[nodiscard]] static u8 CountCollectedConductorSeals(
        const en::GameplayPipeline& pipeline) noexcept;

    // Returns true when all three midgame branches (Choirworks, Drowned Archive, Gravity Foundry) have been completed.
    [[nodiscard]] static bool IsSilentReliquaryUnlocked(
        const en::GameplayPipeline& pipeline) noexcept;

    // Returns true if the specified GDD v0.4 Memory Tuning Flag is completed.
    [[nodiscard]] static bool IsMemoryTuningFlagComplete(
        MemoryTuningFlagId flag,
        const en::GameplayPipeline& pipeline) noexcept;

    // Counts how many of the 4 GDD v0.4 Memory Tuning Flags (Surface Lie, The Collars, The Veil, The Twelve) are complete.
    [[nodiscard]] static u8 CountCompletedMemoryTuningFlags(
        const en::GameplayPipeline& pipeline) noexcept;

    // Returns a clear main-path objective string for the Journal/HUD without requiring any specific equipped Echo.
    [[nodiscard]] static std::string_view GetMainPathObjective(
        u8 activeRoomId,
        u16 unlockedEchoMask,
        const en::GameplayPipeline& pipeline) noexcept;
};

} // namespace vots
