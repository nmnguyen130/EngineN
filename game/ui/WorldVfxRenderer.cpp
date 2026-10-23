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

    const f32 bgW = static_cast<f32>(roomSpec.tileWidth) + 6.0f;
    const f32 bgH = static_cast<f32>(roomSpec.tileHeight) + 4.0f;
    list.PushGradientRect(
        en::Rect{roomSpec.MinWorldX() - 3.0f, roomSpec.MinWorldY() - 2.0f, bgW, bgH},
        regionDef.skyTopColor,
        regionDef.skyBottomColor,
        true,
        0U, 0U, 32768U, 0U);

    for (i32 p = static_cast<i32>(roomSpec.MinWorldX()) + 4;
         p <= static_cast<i32>(roomSpec.MaxWorldX()) - 4;
         p += 4) {
        const f32 px = static_cast<f32>(p);
        const f32 colH = static_cast<f32>(roomSpec.tileHeight) - 2.0f;
        list.PushRect(
            en::Rect{px - 0.32f, 0.0f, 0.64f, colH},
            Color{0.13f, 0.15f, 0.22f, 0.85f},
            0U, 1U, 32768U, 0U);
        list.PushRect(
            en::Rect{px - 0.44f, 0.0f, 0.88f, 0.45f},
            Color{0.18f, 0.21f, 0.30f, 0.90f},
            0U, 2U, 32768U, 0U);
    }

    if (roomSpec.westNeighborRoomId < kRoomCount) {
        const RoomTopologySpec& westSpec = GetRoomTopology(roomSpec.westNeighborRoomId);
        char westBuf[48]{};
        std::snprintf(westBuf, sizeof(westBuf), "<< %.*s",
                      static_cast<int>(westSpec.hudTitle.size()), westSpec.hudTitle.data());
        list.PushText(
            Vec2{roomSpec.MinWorldX() + 0.8f, 4.15f},
            westBuf, Color{0.70f, 0.94f, 1.0f, 0.92f}, 0.025f, 0U, 20U, 0U);
    }
    if (roomSpec.eastNeighborRoomId < kRoomCount) {
        const RoomTopologySpec& eastSpec = GetRoomTopology(roomSpec.eastNeighborRoomId);
        char eastBuf[48]{};
        std::snprintf(eastBuf, sizeof(eastBuf), "%.*s >>",
                      static_cast<int>(eastSpec.hudTitle.size()), eastSpec.hudTitle.data());
        list.PushText(
            Vec2{roomSpec.MaxWorldX() - 4.8f, 4.15f},
            eastBuf, Color{0.70f, 0.94f, 1.0f, 0.92f}, 0.025f, 0U, 20U, 0U);
    }

    if (physics == nullptr) return;
    const en::SpatialIndex2D& spatial = physics->Spatial();
    const u16 w = spatial.WidthTiles();
    const u16 h = spatial.HeightTiles();
    if (w == 0U || h == 0U || w > 64U || h > 24U) return;

    const f32 cell = spatial.TileSizeMeters();
    const f32 origX = spatial.OriginX();
    const f32 origY = spatial.OriginY();

    // 2D Rectangular Greedy Meshing for Solid and OneWay tilemaps:
    // Merges adjacent identical tiles into maximal rectangular slabs so shared internal edges are never double-drawn!
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
                list.PushLine(
                    Vec2{wx + 0.18f, wy + cell - 0.24f},
                    Vec2{wx + 0.45f, wy + cell - 0.46f},
                    0.06f,
                    Color{0.45f, 0.68f, 0.82f, 0.90f},
                    0U, 4U, 32768U, 0U);
                list.PushLine(
                    Vec2{wx + rectW - 0.18f, wy + cell - 0.24f},
                    Vec2{wx + rectW - 0.45f, wy + cell - 0.46f},
                    0.06f,
                    Color{0.45f, 0.68f, 0.82f, 0.90f},
                    0U, 4U, 32768U, 0U);
            }
        }
    }
}

void EmitWorldPropsAndProjectiles(
    const en::World& world,
    bool preferGamepadGlyphs,
    en::RenderList& list) noexcept {
    const char* interactGlyph = preferGamepadGlyphs ? "[D-UP]" : "[E]";

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
                char resBuf[36]{};
                std::snprintf(resBuf, sizeof(resBuf), "RESONATOR %s", interactGlyph);
                list.PushText(
                    Vec2{pos.x - 0.95f, pos.y + 1.18f},
                    resBuf, Color{0.55f, 0.98f, 1.0f, 1.0f}, 0.026f, 0U, 22U, 0U);
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
                char altarBuf[40]{};
                std::snprintf(altarBuf, sizeof(altarBuf), "ALTAR [%.*s] %s",
                              static_cast<int>(edef.glyph.size()), edef.glyph.data(), interactGlyph);
                list.PushText(
                    Vec2{pos.x - 0.95f, pos.y + 1.15f},
                    altarBuf, edef.primaryColor, 0.026f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::LostWickHusk: {
                list.PushRectOutline(
                    en::Rect{pos.x - 0.26f, pos.y - 0.25f, 0.52f, 0.65f},
                    0.05f, Color{1.0f, 0.84f, 0.28f, 1.0f}, 0U, 11U, 32768U, 0U);
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.08f},
                    0.42f, 0.58f, Color{1.0f, 0.80f, 0.25f, 0.90f}, 0U, 12U, 0U);
                char huskBuf[32]{};
                std::snprintf(huskBuf, sizeof(huskBuf), "HUSK %s", interactGlyph);
                list.PushText(
                    Vec2{pos.x - 0.80f, pos.y + 1.10f},
                    huskBuf, Color{1.0f, 0.88f, 0.35f, 1.0f}, 0.028f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::FrequencyGate: {
                const bool open = (inter.activated != 0U);
                list.PushRectOutline(
                    en::Rect{pos.x - 0.48f, pos.y - 0.95f, 0.96f, 2.05f},
                    0.07f,
                    open ? Color{0.30f, 0.92f, 0.55f, 0.95f} : Color{0.96f, 0.76f, 0.24f, 1.0f},
                    0U, 9U, 32768U, 0U);
                list.PushText(
                    Vec2{pos.x - 0.85f, pos.y + 1.25f},
                    open ? "GATE [OPEN]" : "FREQ GATE [E]",
                    open ? Color{0.45f, 0.98f, 0.65f, 1.0f} : Color{0.98f, 0.80f, 0.32f, 1.0f},
                    0.026f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::MemoryTuningFork: {
                list.PushLine(Vec2{pos.x, pos.y - 0.85f}, Vec2{pos.x, pos.y + 0.15f},
                              0.08f, Color{0.96f, 0.58f, 0.88f, 1.0f}, 0U, 9U, 32768U, 0U);
                list.PushText(
                    Vec2{pos.x - 0.85f, pos.y + 1.15f},
                    "MEMORY FORK [E]",
                    Color{0.96f, 0.65f, 0.92f, 1.0f}, 0.025f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::LoreTablet: {
                list.PushRectOutline(
                    en::Rect{pos.x - 0.38f, pos.y - 0.90f, 0.76f, 1.45f},
                    0.05f, Color{0.75f, 0.84f, 0.98f, 1.0f}, 0U, 9U, 32768U, 0U);
                list.PushText(
                    Vec2{pos.x - 0.65f, pos.y + 1.15f},
                    "EVIDENCE [E]",
                    Color{0.85f, 0.90f, 1.0f, 1.0f}, 0.025f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::SpireDecisionCore: {
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.3f},
                    0.65f, 0.95f, Color{1.0f, 0.92f, 0.50f, 0.95f}, 0U, 11U, 0U);
                list.PushText(
                    Vec2{pos.x - 0.85f, pos.y + 1.35f},
                    "SPIRE CORE [E]",
                    Color{1.0f, 0.95f, 0.65f, 1.0f}, 0.028f, 0U, 22U, 0U);
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
                list.PushText(
                    Vec2{pos.x - 1.15f, pos.y + 1.15f},
                    pulled ? "SHORTCUT OPEN [WALK EAST -> R0]" : "SHORTCUT LEVER [E -> OPEN R0]",
                    Color{0.45f, 0.98f, 0.68f, 1.0f}, 0.026f, 0U, 22U, 0U);
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
        list.PushText(
            Vec2{txf.x - 0.95f, txf.y + 1.45f},
            "CRACKED WALL [CATACLYSM]",
            Color{1.0f, 0.75f, 0.35f, 1.0f}, 0.024f, 0U, 22U, 0U);
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
            list.PushBeam(Vec2{pxf.x - 0.45f, pxf.y - 0.15f}, Vec2{pxf.x + 0.45f, pxf.y + 0.18f},
                          0.16f, Color{1.0f, 0.92f, 0.92f, 1.0f}, c, 0U, 17U, 0U);
            list.PushText(Vec2{pxf.x - 0.65f, pxf.y + 0.32f}, "CRIMSON WAVE [JUMP/DASH!]", c, 0.022f, 0U, 22U, 0U);
        } else {
            list.PushRing(Vec2{pxf.x, pxf.y}, 0.08f, 0.28f, c, 0U, 16U, 0U);
            list.PushCircleOutline(Vec2{pxf.x, pxf.y}, 0.38f, isCrimson ? 4U : 6U, 0.06f,
                                   Color{1.0f, 1.0f, 1.0f, 0.95f}, 0U, 17U, 0U);
        }
    }
}

void EmitKaelenSilhouetteSlashAndHitFeedback(
    EntityId playerId,
    const en::World& world,
    en::RenderList& list) noexcept {
    if (!world.IsAlive(playerId)) return;
    const auto* ps = world.TryGet<PlayerStateComp>(playerId);
    const auto* atk = world.TryGet<WeaponAttackComp>(playerId);
    const auto* motion = world.TryGet<en::MotionStack2D>(playerId);
    const auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (ps == nullptr) return;

    const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
    const f32 facing = (motion != nullptr && motion->FacingSign() < 0) ? -1.0f : 1.0f;
    const EchoId activeEcho = (atk != nullptr && atk->moveKind != WeaponMoveKind::None)
                                  ? atk->capturedEchoId
                                  : ps->deck.ActiveEcho();
    const EchoIdentityDef& edef = GetEchoDef(activeEcho);
    const Color coreGlow{1.0f, 0.98f, 0.92f, 0.98f};

    const u16 iframes = (recv != nullptr) ? recv->iframeTicks : 0U;
    const bool inHurtFlash = (iframes > (kPlayerHitProtectionTicks - kPlayerHurtFlashDurationTicks));
    const bool iframeGhostBlink = (!inHurtFlash && iframes > 0U && ((iframes >> 2U) & 1U) != 0U);
    const f32 bodyAlpha = iframeGhostBlink ? 0.34f : 1.0f;

    const Color bodyColor = inHurtFlash
                                ? Color{1.0f, 0.18f, 0.24f, 1.0f}
                                : Color{0.24f, 0.28f, 0.40f, bodyAlpha};
    const Color outlineColor = inHurtFlash
                                   ? Color{1.0f, 0.96f, 0.92f, 1.0f}
                                   : Color{edef.primaryColor.r, edef.primaryColor.g, edef.primaryColor.b, bodyAlpha};

    if (inHurtFlash) {
        const f32 flashProg = static_cast<f32>(kPlayerHitProtectionTicks - iframes) /
                              static_cast<f32>(kPlayerHurtFlashDurationTicks);
        const f32 shockR = 0.55f + flashProg * 1.45f;
        list.PushRing(Vec2{pxf.x, pxf.y}, shockR - 0.16f, shockR, Color{1.0f, 0.20f, 0.26f, 0.96f}, 0U, 24U, 0U);
        list.PushLine(Vec2{pxf.x - 0.85f, pxf.y - 0.85f}, Vec2{pxf.x + 0.85f, pxf.y + 0.85f},
                      0.09f, Color{1.0f, 0.92f, 0.92f, 0.98f}, 0U, 24U, 32768U, 0U);
        list.PushLine(Vec2{pxf.x - 0.85f, pxf.y + 0.85f}, Vec2{pxf.x + 0.85f, pxf.y - 0.85f},
                      0.09f, Color{1.0f, 0.92f, 0.92f, 0.98f}, 0U, 24U, 32768U, 0U);
        list.PushText(Vec2{pxf.x - 0.95f, pxf.y + 1.35f}, "! DAMAGED [RECOIL] !",
                      Color{1.0f, 0.28f, 0.32f, 1.0f}, 0.028f, 0U, 26U, 0U);
    } else if (iframes > 0U) {
        list.PushCircleOutline(Vec2{pxf.x, pxf.y}, 0.92f, 8U, 0.05f,
                               Color{0.85f, 0.95f, 1.0f, 0.55f}, 0U, 19U, 0U);
    }

    if (ps->IsDashing()) {
        for (i32 g = 1; g <= 3; ++g) {
            const f32 gx = pxf.x - facing * static_cast<f32>(g) * 0.58f;
            list.PushRectOutline(
                en::Rect{gx - 0.23f, pxf.y - 0.80f, 0.46f, 1.50f},
                0.05f,
                Color{edef.primaryColor.r, edef.primaryColor.g, edef.primaryColor.b, 0.65f / static_cast<f32>(g)},
                0U, 11U, 32768U, 0U);
        }
    }

    list.PushLine(
        Vec2{pxf.x, pxf.y + 0.18f},
        Vec2{pxf.x - facing * 0.42f, pxf.y - 0.62f},
        0.16f, Color{0.16f, 0.20f, 0.30f, bodyAlpha}, 0U, 12U, 32768U, 0U);
    list.PushLine(
        Vec2{pxf.x - 0.14f, pxf.y - 0.25f},
        Vec2{pxf.x - 0.18f, pxf.y - 0.80f},
        0.10f, Color{0.20f, 0.24f, 0.34f, bodyAlpha}, 0U, 12U, 32768U, 0U);
    list.PushLine(
        Vec2{pxf.x + 0.14f, pxf.y - 0.25f},
        Vec2{pxf.x + 0.18f, pxf.y - 0.80f},
        0.10f, Color{0.20f, 0.24f, 0.34f, bodyAlpha}, 0U, 12U, 32768U, 0U);
    list.PushRect(
        en::Rect{pxf.x - 0.22f, pxf.y - 0.28f, 0.44f, 0.62f},
        bodyColor, 0U, 13U, 32768U, 0U);
    list.PushRectOutline(
        en::Rect{pxf.x - 0.23f, pxf.y - 0.28f, 0.46f, 0.62f},
        0.04f, outlineColor, 0U, 14U, 32768U, 0U);
    list.PushRect(
        en::Rect{pxf.x - 0.17f, pxf.y + 0.36f, 0.34f, 0.34f},
        inHurtFlash ? Color{1.0f, 0.90f, 0.90f, 1.0f} : Color{0.30f, 0.35f, 0.48f, bodyAlpha},
        0U, 14U, 32768U, 0U);
    list.PushRect(
        en::Rect{pxf.x + (facing > 0.0f ? 0.0f : -0.16f), pxf.y + 0.48f, 0.16f, 0.09f},
        outlineColor, 0U, 15U, 32768U, 0U);

    const Vec2 lanternPos{pxf.x - facing * 0.48f, pxf.y + 0.56f};
    list.PushRectOutline(
        en::Rect{lanternPos.x - 0.14f, lanternPos.y - 0.18f, 0.28f, 0.36f},
        0.04f, Color{0.85f, 0.72f, 0.35f, bodyAlpha}, 0U, 14U, 32768U, 0U);
    list.PushRing(lanternPos, 0.03f, 0.10f, outlineColor, 0U, 15U, 0U);

    const Vec2 handCenter{pxf.x + facing * 0.32f, pxf.y + 0.05f};
    if (atk == nullptr || atk->moveKind == WeaponMoveKind::None) {
        list.PushBeam(
            handCenter,
            Vec2{handCenter.x + facing * 0.75f, handCenter.y + 0.38f},
            0.10f, coreGlow, outlineColor, 0U, 15U, 0U);
    } else {
        const WeaponMoveSpec& spec = GetWeaponMoveSpec(atk->moveKind);
        if (atk->elapsedTicks < spec.startupTicks) {
            const f32 windRatio = static_cast<f32>(atk->elapsedTicks) / static_cast<f32>(std::max<u8>(1U, spec.startupTicks));
            list.PushBeam(
                handCenter,
                Vec2{pxf.x - facing * 0.55f, pxf.y + 0.65f},
                0.14f, coreGlow, edef.primaryColor, 0U, 18U, 0U);
            list.PushRing(
                handCenter, 0.10f, 0.24f + windRatio * 0.25f, edef.primaryColor, 0U, 18U, 0U);
        } else if (atk->elapsedTicks < static_cast<u16>(spec.startupTicks + spec.activeTicks + 6U)) {
            const bool isHeavy = (atk->moveKind == WeaponMoveKind::Heavy);
            const f32 radius = isHeavy ? 1.90f : (atk->moveKind == WeaponMoveKind::Light3 ? 1.62f : 1.38f);
            const f32 thickness = isHeavy ? 0.50f : 0.34f;
            const f32 startAngle = (facing > 0.0f) ? -1.10f : 2.04f;
            const f32 sweepAngle = 2.20f;
            const Vec2 bladeTip{pxf.x + facing * (radius + 0.30f), pxf.y - 0.25f};

            list.PushSlashArc(
                handCenter, radius, startAngle, sweepAngle, thickness, edef.primaryColor, 0U, 19U, 0U);
            list.PushSlashArc(
                handCenter, radius * 0.82f, startAngle + 0.12f, sweepAngle - 0.24f, thickness * 0.45f,
                coreGlow, 0U, 20U, 0U);
            list.PushBeam(
                handCenter, bladeTip, isHeavy ? 0.30f : 0.18f, coreGlow, edef.primaryColor, 0U, 20U, 0U);
        }
    }

    if (ps->parryWindowTicks > 0U) {
        const EchoIdentityDef& pdef = GetEchoDef(ps->capturedParryEcho);
        list.PushCircleOutline(
            Vec2{pxf.x, pxf.y + 0.05f}, 1.05f, 6U, 0.10f,
            Color{1.0f, 0.92f, 0.35f, 0.98f}, 0U, 20U, 0U);
        list.PushRing(
            Vec2{pxf.x, pxf.y + 0.05f}, 0.58f, 0.78f, pdef.primaryColor, 0U, 20U, 0U);
        list.PushText(
            Vec2{pxf.x - 0.55f, pxf.y + 1.08f},
            "PARRY WARD",
            Color{1.0f, 0.95f, 0.45f, 1.0f}, 0.026f, 0U, 25U, 0U);
    }

    if (ps->burstVfxTicks > 0U) {
        const f32 progress = 1.0f - (static_cast<f32>(ps->burstVfxTicks) / 28.0f);
        const f32 outerR = 0.85f + progress * 2.85f;
        list.PushRing(Vec2{pxf.x, pxf.y}, outerR - 0.22f, outerR, edef.primaryColor, 0U, 21U, 0U);
    }
}

void EmitDistinctWaveReactionVfx(
    Vec2 pos,
    WaveReactionId reactionId,
    u16 vfxTicksRemaining,
    en::RenderList& list) noexcept {
    const f32 prog = 1.0f - (static_cast<f32>(vfxTicksRemaining) / 30.0f);
    const f32 rad = 0.65f + prog * 1.75f;
    const Color whiteCore{1.0f, 0.98f, 0.90f, 0.96f};
    list.PushRing(pos, rad - 0.18f, rad, whiteCore, 0U, 24U, 0U);
    (void)reactionId;
}

void EmitEnemySilhouettesAnd3StageTelegraphs(
    const en::World& world,
    en::RenderList& list) noexcept {
    for (auto [id, recv, brk, mark] : world.View<en::DamageReceiver2D, BreakAndCcComp, ResonanceMarkComp>()) {
        if (recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) continue;
        const en::WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        const Vec2 pos{xf.x, xf.y};
        const auto* ai = world.TryGet<EnemyAiComp>(id);
        const auto* motion = world.TryGet<en::MotionStack2D>(id);
        const f32 facing = (motion != nullptr && motion->FacingSign() < 0) ? -1.0f : 1.0f;
        const EnemyArchetype archetype = ai ? static_cast<EnemyArchetype>(ai->archetypeId)
                                            : EnemyArchetype::DissonantDrifter;
        const bool enemyFlashing = (mark.hitFlashTicks > 0U);
        const Color flashOverride{1.0f, 0.96f, 0.88f, 1.0f};

        if (brk.category == TargetCategory::Boss || archetype == EnemyArchetype::AshSumpWardenBoss) {
            const u8 phase = ai ? ai->bossPhase : 1U;
            const Color phaseColor = (phase == 1U) ? Color{0.98f, 0.84f, 0.32f, 1.0f}
                                                   : Color{1.00f, 0.28f, 0.42f, 1.0f};
            list.PushRect(en::Rect{pos.x - 0.58f, pos.y - 1.30f, 1.16f, 2.05f},
                          enemyFlashing ? flashOverride : Color{0.18f, 0.14f, 0.24f, 1.0f}, 0U, 11U, 32768U, 0U);
            list.PushRectOutline(en::Rect{pos.x - 0.58f, pos.y - 1.30f, 1.16f, 2.05f},
                                 0.07f, phaseColor, 0U, 12U, 32768U, 0U);
            list.PushCircleOutline(Vec2{pos.x, pos.y + 1.05f}, 0.42f, 8U, 0.07f, phaseColor, 0U, 13U, 0U);
        } else if (archetype == EnemyArchetype::ChoirSentry) {
            list.PushCircleOutline(Vec2{pos.x, pos.y + 0.10f}, 0.48f, 6U, 0.06f,
                                   Color{0.38f, 0.88f, 1.0f, 1.0f}, 0U, 11U, 0U);
            list.PushRect(en::Rect{pos.x - 0.22f, pos.y - 0.45f, 0.44f, 0.88f},
                          enemyFlashing ? flashOverride : Color{0.16f, 0.26f, 0.36f, 1.0f}, 0U, 11U, 32768U, 0U);
        } else if (archetype == EnemyArchetype::ResonanceBrute || brk.category == TargetCategory::HeavyElite) {
            list.PushRect(en::Rect{pos.x - 0.52f, pos.y - 0.98f, 1.04f, 1.42f},
                          enemyFlashing ? flashOverride : Color{0.32f, 0.20f, 0.15f, 1.0f}, 0U, 11U, 32768U, 0U);
            list.PushRectOutline(en::Rect{pos.x - 0.52f, pos.y - 0.98f, 1.04f, 1.42f},
                                 0.07f, Color{0.94f, 0.60f, 0.24f, 1.0f}, 0U, 12U, 32768U, 0U);
        } else {
            list.PushRect(en::Rect{pos.x - 0.28f, pos.y - 0.72f, 0.56f, 0.92f},
                          enemyFlashing ? flashOverride : Color{0.32f, 0.16f, 0.22f, 1.0f}, 0U, 11U, 32768U, 0U);
            list.PushRect(en::Rect{pos.x - 0.20f, pos.y + 0.22f, 0.40f, 0.34f},
                          Color{0.78f, 0.72f, 0.68f, 1.0f}, 0U, 12U, 32768U, 0U);
        }

        if (enemyFlashing) {
            list.PushRing(pos, 0.28f, 0.78f, flashOverride, 0U, 24U, 0U);
            list.PushLine(Vec2{pos.x - 0.70f, pos.y - 0.70f}, Vec2{pos.x + 0.70f, pos.y + 0.70f},
                          0.08f, flashOverride, 0U, 24U, 32768U, 0U);
        }

        const f32 hpRatio = (recv.maxHp > 0) ? static_cast<f32>(recv.hp) / static_cast<f32>(recv.maxHp) : 0.0f;
        const f32 brkRatio = static_cast<f32>(brk.breakMeter) / static_cast<f32>(kMaxBreakMeter);

        list.PushRect(en::Rect{pos.x - 0.72f, pos.y + 1.13f, 1.44f, 0.16f},
                      Color{0.08f, 0.08f, 0.10f, 0.90f}, 0U, 21U, 32768U, 0U);
        list.PushRect(en::Rect{pos.x - 0.70f, pos.y + 1.15f, 1.40f * hpRatio, 0.12f},
                      Color{0.92f, 0.22f, 0.28f, 0.98f}, 0U, 22U, 32768U, 0U);
        list.PushRect(en::Rect{pos.x - 0.70f, pos.y + 0.98f, 1.40f * brkRatio, 0.10f},
                      Color{0.92f, 0.74f, 0.22f, 0.95f}, 0U, 22U, 32768U, 0U);

        if (mark.HasActiveMark()) {
            const EchoIdentityDef& mdef = GetEchoDef(mark.activeMark);
            list.PushRing(Vec2{pos.x, pos.y + 1.62f}, 0.16f, 0.28f, mdef.primaryColor, 0U, 24U, 0U);
            list.PushText(Vec2{pos.x - 0.28f, pos.y + 1.56f}, mdef.glyph, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.026f, 0U, 25U, 0U);
        }
        if (mark.reactionVfxTicks > 0U && mark.lastReactionTriggered != WaveReactionId::None) {
            EmitDistinctWaveReactionVfx(pos, mark.lastReactionTriggered, mark.reactionVfxTicks, list);
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
                    list.PushCircleOutline(pos, 1.35f - windProg * 0.45f, 4U, 0.11f,
                                           Color{1.0f, 0.16f, 0.22f, 0.98f}, 0U, 23U, 0U);
                    const f32 boxX = (facing > 0.0f) ? pos.x : (pos.x - spec.reachWidthWu);
                    list.PushRectOutline(
                        en::Rect{boxX, pos.y - 0.95f, spec.reachWidthWu, 0.55f},
                        0.07f, Color{1.0f, 0.20f, 0.26f, 0.95f}, 0U, 22U, 32768U, 0U);
                    for (i32 p = 1; p <= 3; ++p) {
                        const f32 px = pos.x + facing * (static_cast<f32>(p) * 0.45f);
                        list.PushBeam(Vec2{px, pos.y - 0.90f}, Vec2{px, pos.y - 0.90f + windProg * 1.45f},
                                      0.12f, Color{1.0f, 0.85f, 0.85f, 1.0f}, Color{1.0f, 0.16f, 0.22f, 0.95f}, 0U, 23U, 0U);
                    }
                    list.PushText(Vec2{pos.x - 1.35f, pos.y + 1.92f}, "! CRIMSON [JUMP / DASH!] !",
                                  Color{1.0f, 0.25f, 0.30f, 1.0f}, 0.027f, 0U, 25U, 0U);
                } else {
                    const f32 outerR = 1.45f - windProg * 0.80f;
                    list.PushRing(pos, outerR - 0.12f, outerR, Color{1.0f, 0.88f, 0.22f, 0.96f}, 0U, 23U, 0U);
                    const f32 reachX = (facing > 0.0f) ? pos.x : (pos.x - spec.reachWidthWu);
                    list.PushRectOutline(
                        en::Rect{reachX, pos.y - 0.65f, spec.reachWidthWu, spec.reachHeightWu},
                        0.05f, Color{1.0f, 0.88f, 0.25f, 0.65f}, 0U, 22U, 32768U, 0U);

                    if (inStrikeWindow) {
                        const Vec2 glintPos{pos.x + facing * 0.65f, pos.y + 0.20f};
                        list.PushBeam(Vec2{glintPos.x - 0.75f, glintPos.y}, Vec2{glintPos.x + 0.75f, glintPos.y},
                                      0.16f, Color{1.0f, 1.0f, 1.0f, 1.0f}, Color{1.0f, 0.92f, 0.25f, 1.0f}, 0U, 25U, 0U);
                        list.PushBeam(Vec2{glintPos.x, glintPos.y - 0.75f}, Vec2{glintPos.x, glintPos.y + 0.75f},
                                      0.16f, Color{1.0f, 1.0f, 1.0f, 1.0f}, Color{1.0f, 0.92f, 0.25f, 1.0f}, 0U, 25U, 0U);
                        list.PushText(Vec2{pos.x - 1.15f, pos.y + 1.95f}, ">>> PRESS [F] PARRY NOW! <<<",
                                      Color{1.0f, 0.98f, 0.55f, 1.0f}, 0.028f, 0U, 26U, 0U);
                    } else {
                        list.PushText(Vec2{pos.x - 0.95f, pos.y + 1.92f}, "WINDUP [READY PARRY F]",
                                      Color{1.0f, 0.88f, 0.30f, 0.95f}, 0.025f, 0U, 25U, 0U);
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
    EmitKaelenSilhouetteSlashAndHitFeedback(playerId, world, list);
    EmitEnemySilhouettesAnd3StageTelegraphs(world, list);
}

} // namespace vots
