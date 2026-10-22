#pragma once

#include "game/assets/GameAssetPipeline.h"
#include "game/components/GameComponents.h"
#include "gameplay/GameplayPipeline.h"
#include "world/World.h"

namespace vots {

class MetroidvaniaSystem;

// Spawns and steps GDD v0.2 enemies (Dissonant Husk, Choir Enforcer, High Conductor Boss) and projectiles.
// Stateless system operating over World component pools; despawns dead enemies and records per-room clear masks.
class EnemyAiSystem {
public:
    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnDissonantHusk(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnChoirEnforcer(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos,
        u8 roomId = 0U,
        u8 spawnSlotBit = 0U) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnHighConductorBoss(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos) noexcept;

    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnHarmonicProjectile(
        en::World& world,
        EntityId owner,
        Vec2 pos,
        Vec2 velocity,
        i16 damage,
        bool protectedFromClear,
        AttackTelegraphKind telegraph = AttackTelegraphKind::GoldHarmonicWave,
        EchoId counterEcho = EchoId::None) noexcept;

    static void StepEnemiesAndProjectiles(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        MetroidvaniaSystem* optionalMetroidvania = nullptr) noexcept;
};

} // namespace vots
