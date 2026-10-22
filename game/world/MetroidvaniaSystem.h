#pragma once

#include "game/components/GameComponents.h"
#include "game/core/SaveSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "world/World.h"

#include <array>
#include <string_view>

namespace vots {

// Diagnostic description of a Frequency Gate's observable blocker, interaction type, and route role (GDD v0.2 Section 5.4).
struct FrequencyGateDiagnostic {
    std::string_view observableBlocker{};
    std::string_view requiredInteraction{};
    bool isMainRoute{true};
    bool isUnlocked{false};
};

// Manages Tuning Resonators, the single Lost Wick Husk, enemy respawn masks, Fog of War, Map Pins, Frequency Gates, and Endings.
// Value type owned by VeilGameApp; synchronizes directly with SaveData and GameplayPipeline world flags.
// Invariant: at most one LostWickHuskRecord is active at any time, and cleared enemies only respawn on Resonator rest or player death.
class MetroidvaniaSystem {
public:
    // Activates a Tuning Resonator checkpoint: heals HP to max, sets Harmony to 100, resets cleared enemies, and updates respawn point.
    void ActivateTuningResonator(
        EntityId playerId,
        u8 roomId,
        Vec2 resonatorPos,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    // Reconfigures Kaelen's 3-Echo Triad Deck while resting at a Tuning Resonator.
    [[nodiscard]] bool ConfigureTriadDeckAtResonator(
        EntityId playerId,
        EchoId slot0,
        EchoId slot1,
        EchoId slot2,
        en::World& world) noexcept;

    // Handles Kaelen's death per GDD v0.2 Section 5.2: respawns at Resonator, sets Harmony to 0, applies non-stacking 25% Max HP penalty,
    // resets cleared room enemies, and accumulates carried Tuning Shards into the single Lost Wick Husk.
    void HandlePlayerDeathAndSpawnHusk(
        EntityId playerId,
        Vec2 deathPos,
        u8 deathRoomId,
        en::World& world) noexcept;

    // Recovers the Lost Wick Husk: restores banked Shards, removes the 25% Max HP penalty (clamping HP to new Max HP without overhealing),
    // and sets Harmony to 100.
    [[nodiscard]] bool RecoverLostWickHusk(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    // Unlocks one of the 12 Apostate Echoes at an Echo Altar.
    void UnlockEchoAtAltar(
        EntityId playerId,
        EchoId echo,
        en::World& world) noexcept;

    // Advances the Memory Tuning questline required for Ending C (Symphony of the Off-Beat).
    void CompleteMemoryTuningFork(
        u8 forkIndex,
        en::GameplayPipeline& pipeline) noexcept;

    constexpr void MarkEnemyCleared(u8 roomId, u8 slotBit) noexcept {
        if (roomId < kRoomCount) {
            m_clearedEnemiesMask[roomId] |= slotBit;
        }
    }

    [[nodiscard]] constexpr bool IsEnemyCleared(u8 roomId, u8 slotBit) const noexcept {
        return roomId < kRoomCount && ((m_clearedEnemiesMask[roomId] & slotBit) != 0U);
    }

    constexpr void ResetClearedEnemies() noexcept {
        m_clearedEnemiesMask.fill(0U);
    }

    // Marks a room discovered in the Acoustic Fog of War bitmask.
    constexpr void DiscoverRoom(u8 roomId) noexcept {
        if (roomId < 8U) {
            m_discoveredRoomsMask |= static_cast<u8>(1U << roomId);
        }
    }

    [[nodiscard]] constexpr bool IsRoomDiscovered(u8 roomId) const noexcept {
        return roomId < 8U && ((m_discoveredRoomsMask & (1U << roomId)) != 0U);
    }

    // Places or updates a player Map Pin (FrequencyGate, UnreachedLedge, LoreTablet, BossArena, TuningForkQuest).
    [[nodiscard]] bool PlaceMapPin(
        u8 roomId,
        Vec2 worldPos,
        MapPinKind kind,
        u16 noteTag = 0U) noexcept;

    [[nodiscard]] bool RemoveNearestMapPin(
        u8 roomId,
        Vec2 worldPos,
        f32 maxRadius = 2.0f) noexcept;

    // Evaluates a Frequency Gate using permanent world-progress flags (never requiring an equipped Echo or deferred mutation).
    [[nodiscard]] static FrequencyGateDiagnostic EvaluateFrequencyGate(
        const InteractableComp& gate,
        const en::GameplayPipeline& pipeline) noexcept;

    // Resolves the achieved ending (Ending A: False Dawn, Ending B: Shattered Spire, Ending C: Symphony of the Off-Beat).
    [[nodiscard]] EndingId ResolveSpireEnding(
        EntityId playerId,
        bool chooseShatterCore,
        bool chooseSymphonyOffBeat,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    [[nodiscard]] SaveData ExportSaveData(
        EntityId playerId,
        const en::World& world,
        const en::GameplayPipeline& pipeline) const noexcept;

    void ImportSaveData(
        const SaveData& data,
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline) noexcept;

    [[nodiscard]] constexpr const LostWickHuskRecord& ActiveHusk() const noexcept { return m_husk; }
    [[nodiscard]] constexpr std::span<const MapPinRecord> MapPins() const noexcept {
        return {m_mapPins.data(), m_pinCount};
    }
    [[nodiscard]] constexpr u8 DiscoveredRoomsMask() const noexcept { return m_discoveredRoomsMask; }
    [[nodiscard]] constexpr EndingId AchievedEnding() const noexcept { return m_achievedEnding; }
    [[nodiscard]] constexpr Vec2 LastCheckpointPos() const noexcept { return {m_checkpointX, m_checkpointY}; }
    [[nodiscard]] constexpr u8 LastCheckpointRoom() const noexcept { return m_checkpointRoomId; }

private:
    LostWickHuskRecord m_husk{};
    std::array<MapPinRecord, kMaxMapPins> m_mapPins{};
    std::array<u8, kRoomCount> m_clearedEnemiesMask{};
    f32 m_checkpointX{-6.0f};
    f32 m_checkpointY{1.0f};
    u8 m_checkpointRoomId{0U};
    u8 m_discoveredRoomsMask{0x01U};
    u8 m_equippedSigilMask{0U};
    u8 m_pinCount{0U};
    EndingId m_achievedEnding{EndingId::None};
};

} // namespace vots
