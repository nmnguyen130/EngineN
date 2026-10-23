#pragma once

#include "game/assets/GameAssetPipeline.h"
#include "game/components/GameComponents.h"
#include "gameplay/GameplayPipeline.h"
#include "input/ActionBuffer.h"
#include "input/InputPipeline.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

namespace vots {

// Spawns and steps Kaelen (The Off-Beat Scavenger), enforcing GDD v0.3–v0.5 locomotion, Wall Slide/Jump,
// Double Jump, configurable 1-or-2 charge extended Dash (3.42 wu), hitstun escape-cancel, and Parry/Combo windows.
// Stateless controller operating directly on World component pools.
// Invariant: active weapon swings capture EchoId at start and never mutate mid-swing.
class PlayerController {
public:
    static void ConfigureDefaultInputMap(
        en::InputMap& map,
        f32 analogDeadzone = 0.18f) noexcept;

    [[nodiscard]] static bool TryRebindActionWithoutConflict(
        en::InputMap& map,
        GameAction action,
        u8 bindingSlot,
        en::InputSource newSource) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnKaelen(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 spawnPos,
        u8 roomId = 0U) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> Spawn(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 spawnPos,
        u8 roomId = 0U) noexcept {
        return SpawnKaelen(world, assets, spawnPos, roomId);
    }

    static void StepPlayer(
        EntityId playerId,
        en::World& world,
        const en::PhysicsSystem& physics,
        en::GameplayPipeline& pipeline,
        const en::TickInput& input,
        en::ActionBuffer& actions,
        u32& ioNextAttackInstanceId) noexcept;

    static void Step(
        EntityId playerId,
        en::World& world,
        const en::PhysicsSystem& physics,
        en::GameplayPipeline& pipeline,
        const en::TickInput& input,
        en::ActionBuffer& actions,
        u32& ioNextAttackInstanceId) noexcept {
        StepPlayer(playerId, world, physics, pipeline, input, actions, ioNextAttackInstanceId);
    }

    static void SetDoubleJumpEnabled(
        EntityId playerId,
        bool enabled,
        en::World& world) noexcept;

    static void SetMaxDashCharges(
        EntityId playerId,
        u8 maxCharges,
        en::World& world) noexcept;

    static void StartWeaponAttack(
        EntityId playerId,
        WeaponMoveKind move,
        en::World& world,
        en::GameplayPipeline& pipeline,
        u32& ioNextAttackInstanceId) noexcept;

    static void SwitchActiveEchoSlot(
        EntityId playerId,
        u8 targetSlot,
        en::World& world) noexcept;

    static bool StartParryStance(
        EntityId playerId,
        en::World& world) noexcept;

    static bool TryStartDash(
        EntityId playerId,
        en::World& world,
        f32 moveInputX = 0.0f) noexcept;

    [[nodiscard]] static constexpr bool CanCancelAttackWithDashOrParry(
        const WeaponAttackComp& atk) noexcept {
        if (atk.moveKind == WeaponMoveKind::None) return true;
        if (atk.moveKind == WeaponMoveKind::Heavy) {
            return atk.elapsedTicks + kRecoveryCancelWindowTicks >= atk.totalTicks;
        }
        return atk.elapsedTicks >= kLightDashParryCancelMinTicks;
    }
};

} // namespace vots
