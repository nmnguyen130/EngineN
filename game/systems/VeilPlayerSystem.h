#pragma once

#include "game/core/VeilTypes.h"

namespace en::veil {

// Player movement, katana 3-hit combo, parry stance, shuriken projectile, and grapple hook controller.
// Stateless system operating on PlayerControl2D and engine gameplay components.
// Invariant: structural spawns (shurikens) use World::Spawn outside pool iteration or deferred commands.
class VeilPlayerSystem final {
public:
    static EntityId SpawnPlayer(
        World& world,
        Vec2 spawnPos,
        AtlasHandle mainAtlas,
        BlobHandle playerRigBlob) noexcept;

    static void StepPlayerController(
        World& world,
        const SimStepView& step,
        const TickInput& tickInput,
        ActionBuffer& actionBuffer,
        GameplayPipeline& pipeline,
        VeilSessionState& session) noexcept;
};

} // namespace en::veil
