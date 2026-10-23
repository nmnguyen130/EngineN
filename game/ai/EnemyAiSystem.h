#pragma once

#include "game/assets/GameAssetPipeline.h"
#include "game/components/GameComponents.h"
#include "gameplay/GameplayPipeline.h"
#include "world/World.h"

namespace vots {

class MetroidvaniaSystem;

// Spawns and steps GDD v0.3–v0.5 enemy archetypes (Drifter, Sentry, Brute, Elite) and the 2-phase Ash-Sump Warden Boss.
// Stateless system operating over World component pools; enforces 2D elevation leashes, turn-around delays, and dual Attack-Token pools.
// Invariant: enemy facing is locked while actionTimerTicks > 0, and common enemies share 1 melee + 1 ranged attack token.
class EnemyAiSystem {
public:
    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnDissonantDrifter(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnChoirSentry(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnResonanceBrute(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnTuningHuskElite(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnAshSumpWardenBoss(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 6U) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnDrifter(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept {
        return SpawnDissonantDrifter(world, assets, pos, roomId, spawnSlotBit);
    }

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnSentry(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept {
        return SpawnChoirSentry(world, assets, pos, roomId, spawnSlotBit);
    }

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnBrute(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept {
        return SpawnResonanceBrute(world, assets, pos, roomId, spawnSlotBit);
    }

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnElite(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept {
        return SpawnTuningHuskElite(world, assets, pos, roomId, spawnSlotBit);
    }

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnWarden(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 6U) noexcept {
        return SpawnAshSumpWardenBoss(world, assets, pos, roomId);
    }

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnHarmonicProjectile(
        en::World& world,
        EntityId owner,
        Vec2 pos,
        Vec2 velocity,
        i16 damage,
        bool protectedFromClear,
        AttackTelegraphKind telegraph = AttackTelegraphKind::GoldHarmonicWave,
        EchoId counterEcho = EchoId::None,
        u16 lifetimeTicks = 180U,
        f32 heightMeters = 0.45f) noexcept;

    static void StepEnemiesAndProjectiles(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        MetroidvaniaSystem* optionalMetroidvania = nullptr) noexcept;

    static void Step(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        MetroidvaniaSystem* optionalMetroidvania = nullptr) noexcept {
        StepEnemiesAndProjectiles(playerId, world, pipeline, optionalMetroidvania);
    }
};

} // namespace vots
