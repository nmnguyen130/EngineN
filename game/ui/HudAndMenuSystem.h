#pragma once

#include "game/world/MetroidvaniaSystem.h"
#include "physics/PhysicsSystem.h"
#include "renderer/RenderList.h"
#include "world/World.h"

namespace vots {

// Renders level tilemaps, slash/parry/reaction VFX, accessible combat HUD, and modal UI screens into RenderList.
// Stateless presentation layer using RenderList::kHudViewSlot and world-space view 0; zero heap allocations.
// Invariant: every Echo indicator renders both its color swatch AND its 3-letter glyph + name (never color alone).
class HudAndMenuSystem {
public:
    static void EmitGameplayHudAndWorldIndicators(
        EntityId playerId,
        const en::World& world,
        const en::PhysicsSystem* physics,
        const MetroidvaniaSystem& mv,
        u8 activeRoomId,
        en::RenderList& list) noexcept;

    static void EmitTitleModal(en::RenderList& list) noexcept;

    static void EmitResonatorDeckModal(
        EntityId playerId,
        const en::World& world,
        en::RenderList& list) noexcept;

    static void EmitMapAndPinsModal(
        const MetroidvaniaSystem& mv,
        u8 activeRoomId,
        en::RenderList& list) noexcept;

    static void EmitPauseModal(en::RenderList& list) noexcept;

    static void EmitEndingModal(
        EndingId ending,
        en::RenderList& list) noexcept;
};

} // namespace vots
