#include "game/world/WorldTopology.h"

namespace vots {

u8 WorldTopology::CheckSeamlessBoundaryExit(
    u8 currentRoomId,
    Vec2 playerPos,
    const en::GameplayPipeline& pipeline) noexcept {
    if (currentRoomId >= kRoomCount) return 0xFFU;
    const RoomTopologySpec& spec = GetRoomTopology(currentRoomId);

    // Only transition when Kaelen walks through the open doorway vertical band (y in [0.4 .. 4.3])
    // all the way to the actual outer screen/camera edge — never when touching upper walls!
    if (playerPos.y < 0.4f || playerPos.y > 4.3f) {
        return 0xFFU;
    }

    if (playerPos.x <= spec.WestExitThresholdX() && spec.westNeighborRoomId < kRoomCount) {
        return spec.westNeighborRoomId;
    }

    if (playerPos.x >= spec.EastExitThresholdX() && spec.eastNeighborRoomId < kRoomCount) {
        if (currentRoomId == 7U && spec.eastNeighborRoomId == 0U) {
            if (!pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::ShortcutToAshSumpOpened))) {
                return 0xFFU;
            }
        }
        return spec.eastNeighborRoomId;
    }
    return 0xFFU;
}

void WorldTopology::GrantMidgameBranchSeal(
    RegionId branchRegion,
    en::GameplayPipeline& pipeline) noexcept {
    switch (branchRegion) {
        case RegionId::R2_Choirworks:
            pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::ConductorSealChoirworks), true);
            break;
        case RegionId::R3_DrownedArchive:
            pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::ConductorSealArchive), true);
            break;
        case RegionId::R4_GravityFoundry:
            pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::ConductorSealFoundry), true);
            break;
        default:
            break;
    }
    if (CountCollectedConductorSeals(pipeline) == 3U) {
        pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::GateSpireOpened), true);
    }
}

u8 WorldTopology::CountCollectedConductorSeals(
    const en::GameplayPipeline& pipeline) noexcept {
    u8 count = 0U;
    if (pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::ConductorSealChoirworks))) ++count;
    if (pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::ConductorSealArchive)))    ++count;
    if (pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::ConductorSealFoundry)))    ++count;
    return count;
}

bool WorldTopology::IsSilentReliquaryUnlocked(
    const en::GameplayPipeline& pipeline) noexcept {
    return CountCollectedConductorSeals(pipeline) == 3U ||
           pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::GateSpireOpened));
}

bool WorldTopology::IsMemoryTuningFlagComplete(
    MemoryTuningFlagId flag,
    const en::GameplayPipeline& pipeline) noexcept {
    if (pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningComplete))) {
        return true;
    }
    const u8 idx = static_cast<u8>(flag);
    if (idx >= kMemoryTuningSpecTable.size()) return false;
    return pipeline.TestWorldFlag(static_cast<u16>(kMemoryTuningSpecTable[idx].completionWorldFlag));
}

u8 WorldTopology::CountCompletedMemoryTuningFlags(
    const en::GameplayPipeline& pipeline) noexcept {
    if (pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningComplete))) {
        return 4U;
    }
    u8 count = 0U;
    for (const MemoryTuningSpec& spec : kMemoryTuningSpecTable) {
        if (pipeline.TestWorldFlag(static_cast<u16>(spec.completionWorldFlag))) {
            ++count;
        }
    }
    return count;
}

std::string_view WorldTopology::GetMainPathObjective(
    u8 activeRoomId,
    u16 unlockedEchoMask,
    const en::GameplayPipeline& pipeline) noexcept {
    if (pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated))) {
        const bool allEchoes = ((unlockedEchoMask & 0x0FFFU) == 0x0FFFU);
        const bool allMemories = (CountCompletedMemoryTuningFlags(pipeline) >= 4U);
        if (allEchoes && allMemories) {
            return "OBJECTIVE: Interact with the Spire Core [E] — Ending C (Symphony of the Off-Beat) Unlocked";
        }
        return "OBJECTIVE: Interact with the Spire Core [E] for Ending A/B, or complete 12 Echoes + 4 Memory Flags for Ending C";
    }
    return GetRoomTopology(activeRoomId).inferredObjective;
}

} // namespace vots
