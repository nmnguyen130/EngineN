#pragma once

#include "game/assets/GameAssetPipeline.h"
#include "game/components/GameComponents.h"
#include "gameplay/GameplayPipeline.h"
#include "input/ActionBuffer.h"
#include "input/InputPipeline.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

namespace vots {

// Spawns and controls Kaelen (The Off-Beat Scavenger), enforcing single-weapon Echo capture, 1 Air-Dash, and 10-tick Parry.
// Stateless controller operating over World components, InputMap, TickInput, and ActionBuffer.
// Invariant: switching Echoes never alters attack stats or already-started attack instance EchoIds.
class PlayerController {
public:
    static void ConfigureDefaultInputMap(en::InputMap& map) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnKaelen(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 spawnPos,
        u8 roomId = 0U) noexcept;

    static void StepPlayer(
        EntityId playerId,
        en::World& world,
        const en::PhysicsSystem& physics,
        en::GameplayPipeline& pipeline,
        const en::TickInput& input,
        en::ActionBuffer& actions,
        u32& ioNextAttackInstanceId) noexcept;

    // Starts a specific Resonance Blade move, capturing the currently active EchoId for the entire attack instance.
    static void StartWeaponAttack(
        EntityId playerId,
        WeaponMoveKind move,
        en::World& world,
        en::GameplayPipeline& pipeline,
        u32& ioNextAttackInstanceId) noexcept;

    // Switches active Triad Deck slot without interrupting an in-flight attack or altering its capturedEchoId.
    static void SwitchActiveEchoSlot(
        EntityId playerId,
        u8 targetSlot,
        en::World& world) noexcept;

    // Starts the 10-tick Parry window and captures the active EchoId for Phase-Inversion counter checking.
    static bool StartParryStance(
        EntityId playerId,
        en::World& world) noexcept;

    // Executes a ground or air dash, consuming 1 Air-Dash charge when airborne.
    static bool TryStartDash(
        EntityId playerId,
        en::World& world) noexcept;
};

} // namespace vots
