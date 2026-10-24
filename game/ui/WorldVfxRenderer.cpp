#include "game/ui/WorldVfxRenderer.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace vots {

namespace {

void EmitRoomBackdropAndTilemap(
    const en::PhysicsSystem* physics,
    u8 activeRoomId,
    en::RenderList& list) noexcept {
    const RoomTopologySpec& roomSpec = GetRoomTopology(activeRoomId);
    const RegionSheetDef& regionDef = GetRegionSheet(roomSpec.region);
    const f32 camX = list.View(0U).worldCenterX;
    const f32 colH = static_cast<f32>(roomSpec.tileHeight) - 2.0f;

    const f32 bgW = static_cast<f32>(roomSpec.tileWidth) + 6.0f;
    const f32 bgH = static_cast<f32>(roomSpec.tileHeight) + 4.0f;
    list.PushGradientRect(
        en::Rect{roomSpec.MinWorldX() - 3.0f, roomSpec.MinWorldY() - 2.0f, bgW, bgH},
        regionDef.skyTopColor,
        regionDef.skyBottomColor,
        true,
        0U, 0U, 32768U, 0U);

    // Parallax Layer 1 (0.20x camera X): Distant Cathedral Spires & Horizontal Galleries
    const f32 paraFarX = camX * 0.80f;
    for (i32 s = -1; s <= 4; ++s) {
        const f32 sx = roomSpec.MinWorldX() + paraFarX * 0.20f + static_cast<f32>(s) * 9.5f;
        list.PushRect(
            en::Rect{sx - 0.85f, 0.0f, 1.70f, colH * 0.92f},
            Color{0.08f, 0.10f, 0.16f, 0.72f},
            0U, 1U, 32768U, 0U);
    }
    list.PushRect(
        en::Rect{roomSpec.MinWorldX() - 2.0f, 3.15f, bgW, 0.22f},
        Color{0.10f, 0.12f, 0.19f, 0.68f},
        0U, 1U, 32768U, 0U);
    list.PushRect(
        en::Rect{roomSpec.MinWorldX() - 2.0f, 6.15f, bgW, 0.22f},
        Color{0.10f, 0.12f, 0.19f, 0.68f},
        0U, 1U, 32768U, 0U);

    // Parallax Layer 2 (0.45x camera X): Mid-ground Acoustic Arches & Hanging Resonance Lanterns
    const f32 paraMidX = camX * 0.55f;
    for (i32 a = 0; a <= 5; ++a) {
        const f32 ax = roomSpec.MinWorldX() + paraMidX * 0.35f + static_cast<f32>(a) * 7.0f;
        list.PushRect(
            en::Rect{ax - 0.48f, 0.0f, 0.96f, colH * 0.82f},
            Color{0.11f, 0.13f, 0.20f, 0.78f},
            0U, 1U, 32768U, 0U);
        list.PushRect(
            en::Rect{ax - 0.72f, 2.85f, 1.44f, 0.24f},
            Color{0.15f, 0.18f, 0.27f, 0.82f},
            0U, 1U, 32768U, 0U);
        list.PushRing(
            Vec2{ax, 2.45f}, 0.05f, 0.14f,
            Color{0.32f, 0.82f, 0.96f, 0.55f},
            0U, 2U, 0U);
    }

    // Parallax Layer 3 (0.75x camera X): Foreground Buttress Columns & Human-Scale Pedestals (1.85 wu tier marks)
    const f32 paraNearOffset = camX * 0.15f;
    for (i32 p = static_cast<i32>(roomSpec.MinWorldX()) + 4;
         p <= static_cast<i32>(roomSpec.MaxWorldX()) - 4;
         p += 5) {
        const f32 px = static_cast<f32>(p) + paraNearOffset;
        list.PushRect(
            en::Rect{px - 0.30f, 0.0f, 0.60f, colH},
            Color{0.14f, 0.16f, 0.24f, 0.85f},
            0U, 2U, 32768U, 0U);
        list.PushRect(
            en::Rect{px - 0.44f, 0.0f, 0.88f, 0.46f},
            Color{0.19f, 0.22f, 0.32f, 0.90f},
            0U, 2U, 32768U, 0U);
        list.PushRect(
            en::Rect{px - 0.38f, 1.85f, 0.76f, 0.16f},
            Color{0.22f, 0.26f, 0.36f, 0.88f},
            0U, 2U, 32768U, 0U);
    }

    if (physics == nullptr) return;
    const en::SpatialIndex2D& spatial = physics->Spatial();
    const u16 w = spatial.WidthTiles();
    const u16 h = spatial.HeightTiles();
    if (w == 0U || h == 0U || w > 64U || h > 24U) return;

    const f32 cell = spatial.TileSizeMeters();
    const f32 origX = spatial.OriginX();
    const f32 origY = spatial.OriginY();

    // 2D Rectangular Greedy Meshing for Solid and OneWay tilemaps
    std::array<u8, 64U * 24U> visited{};

    for (u16 ty = 0U; ty < h; ++ty) {
        for (u16 tx = 0U; tx < w; ++tx) {
            if (visited[static_cast<usize>(ty) * w + tx] != 0U) continue;
            const en::TileMaterialFlags mat = spatial.SampleTileCoord(static_cast<i32>(tx), static_cast<i32>(ty));
            if (mat == en::TileMaterialFlags::Empty) continue;

            u16 runW = 1U;
            while (static_cast<u16>(tx + runW) < w &&
                   visited[static_cast<usize>(ty) * w + (tx + runW)] == 0U &&
                   spatial.SampleTileCoord(static_cast<i32>(tx + runW), static_cast<i32>(ty)) == mat) {
                ++runW;
            }

            u16 runH = 1U;
            if (en::HasPhysicsFlag(mat, en::TileMaterialFlags::Solid)) {
                while (static_cast<u16>(ty + runH) < h) {
                    bool rowMatches = true;
                    for (u16 dx = 0U; dx < runW; ++dx) {
                        if (visited[static_cast<usize>(ty + runH) * w + (tx + dx)] != 0U ||
                            spatial.SampleTileCoord(static_cast<i32>(tx + dx), static_cast<i32>(ty + runH)) != mat) {
                            rowMatches = false;
                            break;
                        }
                    }
                    if (!rowMatches) break;
                    ++runH;
                }
            }

            for (u16 dy = 0U; dy < runH; ++dy) {
                for (u16 dx = 0U; dx < runW; ++dx) {
                    visited[static_cast<usize>(ty + dy) * w + (tx + dx)] = 1U;
                }
            }

            const f32 wx = origX + static_cast<f32>(tx) * cell;
            const f32 wy = origY + static_cast<f32>(ty) * cell;
            const f32 rectW = static_cast<f32>(runW) * cell;
            const f32 rectH = static_cast<f32>(runH) * cell;

            if (en::HasPhysicsFlag(mat, en::TileMaterialFlags::Solid)) {
                list.PushRect(
                    en::Rect{wx, wy, rectW, rectH},
                    Color{0.22f, 0.25f, 0.33f, 1.0f},
                    0U, 3U, 32768U, 0U);
                list.PushRectOutline(
                    en::Rect{wx + 0.03f, wy + 0.03f, rectW - 0.06f, rectH - 0.06f},
                    0.04f,
                    Color{0.34f, 0.39f, 0.50f, 1.0f},
                    0U, 4U, 32768U, 0U);
                list.PushRect(
                    en::Rect{wx + 0.04f, wy + rectH - 0.14f, rectW - 0.08f, 0.12f},
                    Color{0.55f, 0.64f, 0.78f, 1.0f},
                    0U, 4U, 32768U, 0U);
            } else if (en::HasPhysicsFlag(mat, en::TileMaterialFlags::OneWay)) {
                list.PushRect(
                    en::Rect{wx, wy + cell - 0.24f, rectW, 0.24f},
                    Color{0.28f, 0.46f, 0.60f, 0.96f},
                    0U, 4U, 32768U, 0U);
                list.PushRect(
                    en::Rect{wx + 0.02f, wy + cell - 0.09f, rectW - 0.04f, 0.09f},
                    Color{0.78f, 0.94f, 1.0f, 1.0f},
                    0U, 5U, 32768U, 0U);
            }
        }
    }
}

void EmitWorldPropsAndProjectiles(
    const en::World& world,
    bool preferGamepadGlyphs,
    en::RenderList& list) noexcept {
    const char* interactGlyph = preferGamepadGlyphs ? "[UP]" : "[E]";

    for (auto [propId, inter] : world.View<InteractableComp>()) {
        const en::WorldTransform2D& ixf = world.GetWorldByIndex(propId.index);
        const Vec2 pos{ixf.x, ixf.y};

        switch (inter.kind) {
            case InteractableKind::RoomExitPortal:
                break;
            case InteractableKind::TuningResonator: {
                list.PushRect(
                    en::Rect{pos.x - 0.48f, pos.y - 0.95f, 0.96f, 0.30f},
                    Color{0.28f, 0.34f, 0.44f, 1.0f},
                    0U, 8U, 32768U, 0U);
                list.PushRect(
                    en::Rect{pos.x - 0.34f, pos.y - 0.25f, 0.12f, 1.15f},
                    Color{0.45f, 0.92f, 1.0f, 1.0f},
                    0U, 9U, 32768U, 0U);
                list.PushRect(
                    en::Rect{pos.x + 0.22f, pos.y - 0.25f, 0.12f, 1.15f},
                    Color{0.45f, 0.92f, 1.0f, 1.0f},
                    0U, 9U, 32768U, 0U);
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.45f},
                    0.42f, 0.56f,
                    Color{0.30f, 0.95f, 1.0f, 0.85f},
                    0U, 10U, 0U);
                char resBuf[32]{};
                std::snprintf(resBuf, sizeof(resBuf), "RESONATOR %s", interactGlyph);
                list.PushText(
                    Vec2{pos.x - 0.85f, pos.y + 1.18f},
                    resBuf, Color{0.55f, 0.98f, 1.0f, 1.0f}, 0.024f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::EchoAltar: {
                const EchoIdentityDef& edef = GetEchoDef(static_cast<EchoId>(inter.paramByte));
                list.PushRect(
                    en::Rect{pos.x - 0.42f, pos.y - 0.95f, 0.84f, 0.55f},
                    Color{0.22f, 0.24f, 0.32f, 1.0f},
                    0U, 8U, 32768U, 0U);
                list.PushCircleOutline(
                    Vec2{pos.x, pos.y + 0.25f},
                    0.34f, 4U, 0.08f, edef.primaryColor, 0U, 10U, 0U);
                char altarBuf[32]{};
                std::snprintf(altarBuf, sizeof(altarBuf), "ALTAR %s", interactGlyph);
                list.PushText(
                    Vec2{pos.x - 0.65f, pos.y + 1.15f},
                    altarBuf, edef.primaryColor, 0.024f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::LostWickHusk: {
                list.PushRectOutline(
                    en::Rect{pos.x - 0.26f, pos.y - 0.25f, 0.52f, 0.65f},
                    0.05f, Color{1.0f, 0.84f, 0.28f, 1.0f}, 0U, 11U, 32768U, 0U);
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.08f},
                    0.42f, 0.58f, Color{1.0f, 0.80f, 0.25f, 0.90f}, 0U, 12U, 0U);
                char huskBuf[28]{};
                std::snprintf(huskBuf, sizeof(huskBuf), "HUSK %s", interactGlyph);
                list.PushText(
                    Vec2{pos.x - 0.55f, pos.y + 1.10f},
                    huskBuf, Color{1.0f, 0.88f, 0.35f, 1.0f}, 0.025f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::FrequencyGate: {
                const bool open = (inter.activated != 0U);
                list.PushRectOutline(
                    en::Rect{pos.x - 0.48f, pos.y - 0.95f, 0.96f, 2.05f},
                    0.07f,
                    open ? Color{0.30f, 0.92f, 0.55f, 0.95f} : Color{0.96f, 0.76f, 0.24f, 1.0f},
                    0U, 9U, 32768U, 0U);
                break;
            }
            case InteractableKind::MemoryTuningFork: {
                list.PushLine(Vec2{pos.x, pos.y - 0.85f}, Vec2{pos.x, pos.y + 0.15f},
                              0.08f, Color{0.96f, 0.58f, 0.88f, 1.0f}, 0U, 9U, 32768U, 0U);
                break;
            }
            case InteractableKind::LoreTablet: {
                list.PushRectOutline(
                    en::Rect{pos.x - 0.38f, pos.y - 0.90f, 0.76f, 1.45f},
                    0.05f, Color{0.75f, 0.84f, 0.98f, 1.0f}, 0U, 9U, 32768U, 0U);
                break;
            }
            case InteractableKind::SpireDecisionCore: {
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.3f},
                    0.65f, 0.95f, Color{1.0f, 0.92f, 0.50f, 0.95f}, 0U, 11U, 0U);
                break;
            }
            case InteractableKind::ShortcutLever: {
                const bool pulled = (inter.activated != 0U);
                list.PushLine(
                    Vec2{pos.x, pos.y - 0.60f},
                    Vec2{pos.x + (pulled ? 0.45f : -0.45f), pos.y + 0.45f},
                    0.09f,
                    pulled ? Color{0.35f, 0.98f, 0.58f, 1.0f} : Color{0.98f, 0.82f, 0.30f, 1.0f},
                    0U, 10U, 32768U, 0U);
                break;
            }
        }
    }

    for (auto [terId, ter] : world.View<DestructibleTerrainComp>()) {
        if (ter.broken != 0U) continue;
        const en::WorldTransform2D& txf = world.GetWorldByIndex(terId.index);
        list.PushRectOutline(
            en::Rect{txf.x - 0.60f, txf.y - 1.30f, 1.20f, 2.60f},
            0.07f, Color{1.0f, 0.55f, 0.20f, 1.0f}, 0U, 11U, 32768U, 0U);
    }

    for (auto [projId, proj] : world.View<ProjectileComp>()) {
        if (proj.active == 0U) continue;
        const en::WorldTransform2D& pxf = world.GetWorldByIndex(projId.index);
        const auto* spr = world.TryGet<en::Sprite2D>(projId);
        const bool isCrimson = (proj.telegraph == AttackTelegraphKind::CrimsonOverloadWave);
        const bool isGroundWave = (spr != nullptr && spr->height <= 0.36f);
        const Color c = isCrimson ? Color{1.0f, 0.18f, 0.26f, 0.98f} : Color{1.0f, 0.88f, 0.24f, 0.98f};

        if (isGroundWave) {
            list.PushRect(en::Rect{pxf.x - 0.42f, pxf.y - 0.18f, 0.84f, 0.35f}, c, 0U, 16U, 32768U, 0U);
        } else {
            list.PushRing(Vec2{pxf.x, pxf.y}, 0.08f, 0.28f, c, 0U, 16U, 0U);
        }
    }
}

// Kaelen's body, dash afterimages, parry stance, and 24-frame 3-hit sword combo are rendered
// directly via the 96x96 Sprite2D + SpriteAnimator2D. Only room-wide Unison/Sacred Chord pulses
// emit an expanding resonance ring.
void EmitPlayerUnisonPulse(
    EntityId playerId,
    const en::World& world,
    en::RenderList& list) noexcept {
    if (!world.IsAlive(playerId)) return;
    const auto* ps = world.TryGet<PlayerStateComp>(playerId);
    if (ps == nullptr || ps->burstVfxTicks == 0U) return;

    const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
    const EchoIdentityDef& edef = GetEchoDef(ps->deck.ActiveEcho());
    const f32 progress = 1.0f - (static_cast<f32>(ps->burstVfxTicks) / 28.0f);
    const f32 outerR = 0.85f + progress * 2.85f;
    list.PushRing(Vec2{pxf.x, pxf.y}, outerR - 0.18f, outerR, edef.primaryColor, 0U, 21U, 0U);
}

void EmitEnemyStatusAndTelegraphs(
    const en::World& world,
    en::RenderList& list) noexcept {
    for (auto [id, recv, brk, mark] : world.View<en::DamageReceiver2D, BreakAndCcComp, ResonanceMarkComp>()) {
        if (recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) continue;
        const en::WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        const Vec2 pos{xf.x, xf.y};
        const auto* ai = world.TryGet<EnemyAiComp>(id);
        const auto* motion = world.TryGet<en::MotionStack2D>(id);
        const f32 facing = (motion != nullptr && motion->FacingSign() < 0) ? -1.0f : 1.0f;

        if (mark.hitFlashTicks > 0U) {
            list.PushRing(pos, 0.24f, 0.62f, Color{1.0f, 0.96f, 0.88f, 0.95f}, 0U, 24U, 0U);
        }

        const f32 hpRatio = (recv.maxHp > 0) ? static_cast<f32>(recv.hp) / static_cast<f32>(recv.maxHp) : 0.0f;
        const f32 brkRatio = static_cast<f32>(brk.breakMeter) / static_cast<f32>(kMaxBreakMeter);

        list.PushRect(en::Rect{pos.x - 0.62f, pos.y + 1.08f, 1.24f, 0.14f},
                      Color{0.07f, 0.08f, 0.11f, 0.90f}, 0U, 21U, 32768U, 0U);
        list.PushRect(en::Rect{pos.x - 0.60f, pos.y + 1.10f, 1.20f * hpRatio, 0.10f},
                      Color{0.92f, 0.22f, 0.28f, 0.98f}, 0U, 22U, 32768U, 0U);
        list.PushRect(en::Rect{pos.x - 0.60f, pos.y + 0.96f, 1.20f * brkRatio, 0.07f},
                      Color{0.92f, 0.74f, 0.22f, 0.95f}, 0U, 22U, 32768U, 0U);

        if (mark.HasActiveMark()) {
            const EchoIdentityDef& mdef = GetEchoDef(mark.activeMark);
            list.PushRing(Vec2{pos.x, pos.y + 1.44f}, 0.12f, 0.22f, mdef.primaryColor, 0U, 24U, 0U);
        }
        if (mark.reactionVfxTicks > 0U && mark.lastReactionTriggered != WaveReactionId::None) {
            const f32 prog = 1.0f - (static_cast<f32>(mark.reactionVfxTicks) / 30.0f);
            const f32 rad = 0.60f + prog * 1.50f;
            list.PushRing(pos, rad - 0.16f, rad, Color{1.0f, 0.98f, 0.90f, 0.96f}, 0U, 24U, 0U);
        }

        if (ai != nullptr && ai->actionTimerTicks > 0U) {
            const EnemyAttackSpec& spec = GetEnemyAttackSpec(ai->activeAttackId);
            const u16 activeEnd = static_cast<u16>(spec.windupTicks + spec.activeTicks);
            if (ai->actionTimerTicks <= activeEnd) {
                const bool isCrimson = (ai->activeTelegraph == AttackTelegraphKind::CrimsonOverloadWave);
                const f32 windProg = std::min(1.0f, static_cast<f32>(ai->actionTimerTicks) /
                                                    static_cast<f32>(std::max<u16>(1U, spec.windupTicks)));
                const u16 parryLeadStart = (spec.windupTicks > kParryStrikeInterceptLeadTicks)
                                               ? static_cast<u16>(spec.windupTicks - kParryStrikeInterceptLeadTicks)
                                               : 1U;
                const bool inStrikeWindow = (ai->actionTimerTicks >= parryLeadStart && ai->actionTimerTicks <= activeEnd);

                if (isCrimson) {
                    list.PushCircleOutline(pos, 1.25f - windProg * 0.40f, 4U, 0.09f,
                                           Color{1.0f, 0.16f, 0.22f, 0.96f}, 0U, 23U, 0U);
                } else {
                    const f32 outerR = 1.25f - windProg * 0.65f;
                    list.PushRing(pos, outerR - 0.10f, outerR, Color{1.0f, 0.88f, 0.22f, 0.92f}, 0U, 23U, 0U);
                    if (inStrikeWindow) {
                        const Vec2 glintPos{pos.x + facing * 0.55f, pos.y + 0.18f};
                        list.PushBeam(Vec2{glintPos.x - 0.55f, glintPos.y}, Vec2{glintPos.x + 0.55f, glintPos.y},
                                      0.12f, Color{1.0f, 1.0f, 1.0f, 1.0f}, Color{1.0f, 0.92f, 0.25f, 1.0f}, 0U, 25U, 0U);
                        list.PushBeam(Vec2{glintPos.x, glintPos.y - 0.55f}, Vec2{glintPos.x, glintPos.y + 0.55f},
                                      0.12f, Color{1.0f, 1.0f, 1.0f, 1.0f}, Color{1.0f, 0.92f, 0.25f, 1.0f}, 0U, 25U, 0U);
                    }
                }
            }
        }
    }
}

} // namespace

void WorldVfxRenderer::EmitWorldBackdropTilemapAndEntities(
    EntityId playerId,
    const en::World& world,
    const en::PhysicsSystem* physics,
    u8 activeRoomId,
    en::RenderList& list) noexcept {
    EmitRoomBackdropAndTilemap(physics, activeRoomId, list);
    EmitWorldPropsAndProjectiles(world, false, list);
    EmitPlayerUnisonPulse(playerId, world, list);
    EmitEnemyStatusAndTelegraphs(world, list);
}

} // namespace vots
