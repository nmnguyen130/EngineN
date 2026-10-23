#pragma once

#include "game/world/MetroidvaniaSystem.h"
#include "physics/PhysicsSystem.h"
#include "renderer/RenderList.h"
#include "world/World.h"

namespace vots {

// Renders world-space backdrops, tilemaps, seamless room-boundary archways, Kaelen silhouettes/slash arcs,
// and 3-stage Enemy Telegraph & Parry Strike Glint indicators into RenderList view 0.
// Separated from HudAndMenuSystem to enforce Single-Responsibility Principle (SOLID).
class WorldVfxRenderer {
public:
    static void EmitWorldBackdropTilemapAndEntities(
        EntityId playerId,
        const en::World& world,
        const en::PhysicsSystem* physics,
        u8 activeRoomId,
        en::RenderList& list) noexcept;
};

} // namespace vots
