#pragma once

#include "game/assets/GameAssetPipeline.h"
#include "game/components/GameComponents.h"
#include "gameplay/GameplayPipeline.h"
#include "world/World.h"

namespace vots {

// Spawns and steps GDD v0.2 enemies (Dissonant Husk, Choir Enforcer, High Conductor Boss) and projectiles.
// Stateless system operating over World component pools; marks room mobs with kRoomTransientCategoryBit.
class EnemyAiSystem {
public:
    // Spawns a Standard enemy (100 HP, 100 Break, susceptible to Stagger/Stun/Micro-stagger/Knockback/Pull).
    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnDissonantHusk(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos) noexcept;

    // Spawns a Heavy/Elite enemy (300 HP, 100 Break, BreakableGuard + DisplacementResistant, Gold/Crimson telegraphs).
    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnChoirEnforcer(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos) noexcept;

    // Spawns the High Conductor Boss (1200 HP per phase, 100 Break, 3 phases, CounterEchoId Gold waves & Crimson waves).
    [[nodiscard]] static en::Result<EntityId, en::WorldErrc> SpawnHighConductorBoss(
        en::World& world,
        const GameAssetHandles& assets,
        Vec2 pos) noexcept;

    // Spawns an enemy or boss projectile (normal clearable or protected Boss projectile).
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
        en::GameplayPipeline& pipeline) noexcept;
};

} // namespace vots
