#pragma once

#include "game/combat/CombatSystem.h"

#include <span>

namespace vots {

// Stateless resolver for the 10 Signature Wave-Interference Reactions, Standard Unison Burst, and 4 Sacred Chords.
// Separated from CombatSystem to enforce Single-Responsibility Principle (SOLID).
// Invariant: secondary reaction payloads never apply Resonance Marks or recursively trigger reactions.
class WaveReactionResolver {
public:
    // Collects up to outTargets.size() living enemies within radiusMeters of center, sorted by ascending distance then EntityId.
    [[nodiscard]] static u32 GatherLivingEnemiesInRadius(
        en::World& world,
        Vec2 center,
        f32 radiusMeters,
        EntityId excludeEntity,
        std::span<EntityId> outTargets) noexcept;

    // Applies the specific secondary effect (Fracture, Stun, AoE Blast, Projectile Clear, Guard Shatter, Pull, Delay) of a triggered reaction.
    static void ApplySignatureReactionPayload(
        EntityId primaryTargetId,
        const WaveReactionSpec& reactionSpec,
        en::World& world,
        en::GameplayPipeline& pipeline,
        CombatResolutionReport& ioReport) noexcept;

    // Executes the Standard Unison Burst payload (50 Harmony, 3.0 wu radius, up to 5 enemies, 18 HP Damage, 15 Break).
    [[nodiscard]] static bool ExecuteUnisonBurst(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        CombatResolutionReport* outReport = nullptr) noexcept;

    // Executes the Sacred Chord payload matching the player's equipped Triad Deck (100 Harmony).
    [[nodiscard]] static bool ExecuteSacredChord(
        EntityId playerId,
        en::World& world,
        en::GameplayPipeline& pipeline,
        CombatResolutionReport* outReport = nullptr) noexcept;
};

} // namespace vots
