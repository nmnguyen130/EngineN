#include "game/ui/HudAndMenuSystem.h"

#include <cmath>
#include <cstdio>

namespace vots {

namespace {

constexpr std::array<std::string_view, kRoomCount> kRoomTitles{{
    "ROOM 0: ASH-SUMP SLUMS",
    "ROOM 1: RESONATOR SANCTUM",
    "ROOM 2: ECHO VAULT",
    "ROOM 3: SPIRE OF UNISON",
}};

void EmitRoomBackdropAndTilemap(
    const en::PhysicsSystem* physics,
    u8 activeRoomId,
    en::RenderList& list) noexcept {
    // 1. Atmospheric Gothic/Industrial room background in world view 0 (layers 0..2)
    const Color topTint = (activeRoomId == 3U)
                              ? Color{0.16f, 0.10f, 0.22f, 1.0f}
                              : (activeRoomId == 2U ? Color{0.08f, 0.16f, 0.24f, 1.0f}
                                                    : Color{0.10f, 0.12f, 0.18f, 1.0f});
    const Color botTint = Color{0.05f, 0.06f, 0.10f, 1.0f};
    list.PushGradientRect(
        en::Rect{-14.0f, -2.0f, 40.0f, 15.0f},
        topTint,
        botTint,
        true,
        0U, 0U, 32768U, 0U);

    // Background architectural pillars
    for (i32 p = -8; p <= 20; p += 4) {
        const f32 px = static_cast<f32>(p);
        list.PushRect(
            en::Rect{px - 0.35f, 0.0f, 0.70f, 10.0f},
            Color{0.13f, 0.15f, 0.22f, 0.85f},
            0U, 1U, 32768U, 0U);
    }

    if (physics == nullptr) return;
    const en::SpatialIndex2D& spatial = physics->Spatial();
    const u16 w = spatial.WidthTiles();
    const u16 h = spatial.HeightTiles();
    if (w == 0U || h == 0U) return;

    const f32 cell = spatial.TileSizeMeters();
    const f32 origX = spatial.OriginX();
    const f32 origY = spatial.OriginY();

    for (u16 ty = 0U; ty < h; ++ty) {
        for (u16 tx = 0U; tx < w; ++tx) {
            const en::TileMaterialFlags mat = spatial.SampleTileCoord(static_cast<i32>(tx), static_cast<i32>(ty));
            if (mat == en::TileMaterialFlags::Empty) continue;

            const f32 wx = origX + static_cast<f32>(tx) * cell;
            const f32 wy = origY + static_cast<f32>(ty) * cell;

            if (en::HasPhysicsFlag(mat, en::TileMaterialFlags::Solid)) {
                // Solid floor, walls, and ceiling blocks with bright stone rim
                list.PushRect(
                    en::Rect{wx, wy, cell, cell},
                    Color{0.24f, 0.27f, 0.35f, 1.0f},
                    0U, 3U, 32768U, 0U);
                list.PushRect(
                    en::Rect{wx + 0.04f, wy + cell - 0.14f, cell - 0.08f, 0.12f},
                    Color{0.52f, 0.60f, 0.74f, 1.0f},
                    0U, 4U, 32768U, 0U);
            } else if (en::HasPhysicsFlag(mat, en::TileMaterialFlags::OneWay)) {
                // Jump-through one-way platform ledge with glowing brass/cyan trim
                list.PushRect(
                    en::Rect{wx, wy + cell - 0.26f, cell, 0.26f},
                    Color{0.35f, 0.55f, 0.68f, 0.96f},
                    0U, 4U, 32768U, 0U);
                list.PushRect(
                    en::Rect{wx + 0.03f, wy + cell - 0.10f, cell - 0.06f, 0.09f},
                    Color{0.78f, 0.92f, 1.0f, 1.0f},
                    0U, 5U, 32768U, 0U);
            }
        }
    }
}

void EmitPlayerSlashAndAbilityVfx(
    EntityId playerId,
    const en::World& world,
    en::RenderList& list) noexcept {
    if (!world.IsAlive(playerId)) return;
    const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
    const auto* ps = world.TryGet<PlayerStateComp>(playerId);
    const auto* atk = world.TryGet<WeaponAttackComp>(playerId);
    const auto* motion = world.TryGet<en::MotionStack2D>(playerId);
    if (ps == nullptr) return;

    const EchoId activeEcho = (atk != nullptr && atk->moveKind != WeaponMoveKind::None && IsValidEcho(atk->capturedEchoId))
                                  ? atk->capturedEchoId
                                  : ps->deck.ActiveEcho();
    const EchoIdentityDef& edef = GetEchoDef(activeEcho);
    const Color coreGlow{1.0f, 0.98f, 0.88f, 0.96f};
    const f32 facing = (motion != nullptr && motion->FacingSign() < 0) ? -1.0f : 1.0f;

    // 1. Active weapon swing animation (Startup glint, Active sweeping crescent SlashArc + Beam, Recovery trail)
    if (atk != nullptr && atk->moveKind != WeaponMoveKind::None) {
        const WeaponMoveSpec& spec = GetWeaponMoveSpec(atk->moveKind);
        const Vec2 handCenter{pxf.x + facing * 0.35f, pxf.y + 0.08f};
        if (atk->elapsedTicks < spec.startupTicks) {
            list.PushRing(
                Vec2{pxf.x + facing * 0.45f, pxf.y + 0.15f},
                0.08f, 0.22f, coreGlow, 0U, 18U, 0U);
        } else if (atk->elapsedTicks < static_cast<u16>(spec.startupTicks + spec.activeTicks + 5U)) {
            const u8 moveIdx = static_cast<u8>(atk->moveKind);
            const bool isHeavy = (atk->moveKind == WeaponMoveKind::Heavy);
            const f32 radius = isHeavy ? 1.85f : 1.38f;
            const f32 thickness = isHeavy ? 0.48f : 0.34f;

            // Alternate sweep angles across combo steps so 3-hit combos visibly slash down, up, and wide
            f32 startAngle = (facing > 0.0f) ? -1.05f : 2.09f;
            f32 sweepAngle = 2.10f;
            if ((moveIdx & 1U) != 0U) {
                startAngle = (facing > 0.0f) ? -0.75f : 2.39f;
                sweepAngle = 2.00f;
            }

            list.PushSlashArc(
                handCenter,
                radius,
                startAngle,
                sweepAngle,
                thickness,
                edef.primaryColor,
                0U, 19U, 0U);

            list.PushBeam(
                handCenter,
                Vec2{pxf.x + facing * (radius + 0.25f), pxf.y + ((moveIdx & 1U) != 0U ? 0.35f : -0.20f)},
                isHeavy ? 0.28f : 0.16f,
                coreGlow,
                edef.primaryColor,
                0U, 20U, 0U);
        }
    }

    // 2. Parry 10-tick harmonic ward ring
    if (ps->parryWindowTicks > 0U) {
        list.PushRing(
            Vec2{pxf.x, pxf.y},
            0.70f, 0.92f,
            Color{1.0f, 0.92f, 0.35f, 0.95f},
            0U, 20U, 0U);
        list.PushRing(
            Vec2{pxf.x, pxf.y},
            0.52f, 0.68f,
            edef.primaryColor,
            0U, 20U, 0U);
    }

    // 3. Unison Burst / Sacred Chord / Resonator expanding shockwave ring
    if (ps->burstVfxTicks > 0U) {
        const f32 progress = 1.0f - (static_cast<f32>(ps->burstVfxTicks) / 28.0f);
        const f32 outerR = 0.8f + progress * 2.8f;
        list.PushRing(
            Vec2{pxf.x, pxf.y},
            outerR - 0.22f,
            outerR,
            edef.primaryColor,
            0U, 21U, 0U);
    }
}

void EmitWorldPropsAndPortals(
    const en::World& world,
    en::RenderList& list) noexcept {
    for (auto [propId, inter] : world.View<InteractableComp>()) {
        const en::WorldTransform2D& ixf = world.GetWorldByIndex(propId.index);
        const Vec2 pos{ixf.x, ixf.y};

        switch (inter.kind) {
            case InteractableKind::RoomExitPortal: {
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.2f},
                    0.55f, 0.82f,
                    Color{0.35f, 0.88f, 1.0f, 0.92f},
                    0U, 10U, 0U);
                char portalText[32]{};
                std::snprintf(portalText, sizeof(portalText), "PORTAL -> R%u", inter.paramByte);
                list.PushText(
                    Vec2{pos.x - 0.95f, pos.y + 1.35f},
                    portalText,
                    Color{0.75f, 0.96f, 1.0f, 1.0f},
                    0.028f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::TuningResonator: {
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.1f},
                    0.45f, 0.65f,
                    Color{0.30f, 0.95f, 1.0f, 0.90f},
                    0U, 10U, 0U);
                list.PushText(
                    Vec2{pos.x - 0.95f, pos.y + 1.15f},
                    "RESONATOR [E]",
                    Color{0.55f, 0.98f, 1.0f, 1.0f},
                    0.026f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::EchoAltar: {
                const EchoIdentityDef& edef = GetEchoDef(static_cast<EchoId>(inter.paramByte));
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.2f},
                    0.35f, 0.55f,
                    edef.primaryColor,
                    0U, 10U, 0U);
                char altarBuf[32]{};
                std::snprintf(altarBuf, sizeof(altarBuf), "ALTAR [%.*s]",
                              static_cast<int>(edef.glyph.size()), edef.glyph.data());
                list.PushText(
                    Vec2{pos.x - 0.85f, pos.y + 1.15f},
                    altarBuf,
                    edef.primaryColor,
                    0.026f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::LostWickHusk: {
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.15f},
                    0.40f, 0.68f,
                    Color{1.0f, 0.82f, 0.28f, 0.95f},
                    0U, 12U, 0U);
                list.PushText(
                    Vec2{pos.x - 0.80f, pos.y + 1.10f},
                    "HUSK [E]",
                    Color{1.0f, 0.88f, 0.35f, 1.0f},
                    0.028f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::FrequencyGate: {
                list.PushText(
                    Vec2{pos.x - 0.75f, pos.y + 1.15f},
                    inter.activated != 0U ? "GATE [OPEN]" : "FREQ GATE [E]",
                    Color{0.98f, 0.80f, 0.32f, 1.0f},
                    0.026f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::MemoryTuningFork: {
                list.PushText(
                    Vec2{pos.x - 0.85f, pos.y + 1.15f},
                    "TUNING FORK [E]",
                    Color{0.96f, 0.65f, 0.92f, 1.0f},
                    0.025f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::LoreTablet: {
                list.PushText(
                    Vec2{pos.x - 0.65f, pos.y + 1.15f},
                    "LORE [E]",
                    Color{0.85f, 0.90f, 1.0f, 1.0f},
                    0.025f, 0U, 22U, 0U);
                break;
            }
            case InteractableKind::SpireDecisionCore: {
                list.PushRing(
                    Vec2{pos.x, pos.y + 0.3f},
                    0.65f, 0.95f,
                    Color{1.0f, 0.92f, 0.50f, 0.95f},
                    0U, 11U, 0U);
                list.PushText(
                    Vec2{pos.x - 0.85f, pos.y + 1.35f},
                    "SPIRE CORE [E]",
                    Color{1.0f, 0.95f, 0.65f, 1.0f},
                    0.028f, 0U, 22U, 0U);
                break;
            }
        }
    }
}

} // namespace

void HudAndMenuSystem::EmitGameplayHudAndWorldIndicators(
    EntityId playerId,
    const en::World& world,
    const en::PhysicsSystem* physics,
    const MetroidvaniaSystem& mv,
    u8 activeRoomId,
    en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;

    // 1. Render room architecture, Solid floors/walls/ceiling, and OneWay jump-through platforms
    EmitRoomBackdropAndTilemap(physics, activeRoomId, list);

    // 2. Render interactive props, Resonators, Altars, Husk, and Room Exit Portals
    EmitWorldPropsAndPortals(world, list);

    // 3. Render Kaelen's slash arc animation, parry ward, and Unison/Sacred Chord shockwaves
    EmitPlayerSlashAndAbilityVfx(playerId, world, list);

    // 4. Player Top HUD (placed at x = 350 so it never overlaps the top-left Debug Profiler at x = 12..332)
    constexpr f32 kHudLeftX = 350.0f;
    if (world.IsAlive(playerId)) {
        const auto* ps = world.TryGet<PlayerStateComp>(playerId);
        const auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
        if (ps != nullptr && recv != nullptr) {
            const f32 hpRatio = (recv->maxHp > 0)
                                    ? static_cast<f32>(recv->hp) / static_cast<f32>(recv->maxHp)
                                    : 0.0f;
            list.PushProgressBar(
                en::Rect{kHudLeftX, 16.0f, 260.0f, 18.0f},
                hpRatio,
                Color{0.88f, 0.22f, 0.28f, 1.0f},
                Color{0.15f, 0.08f, 0.10f, 0.92f},
                Color{0.92f, 0.85f, 0.75f, 1.0f},
                0U, 25U, kHudView);

            char hpBuf[80]{};
            std::snprintf(hpBuf, sizeof(hpBuf), "HP %d/%d%s | %.*s",
                          recv->hp, recv->maxHp,
                          (ps->huskPenaltyActive != 0U) ? " [HUSK -25%]" : "",
                          static_cast<int>(kRoomTitles[activeRoomId % kRoomCount].size()),
                          kRoomTitles[activeRoomId % kRoomCount].data());
            list.PushText(Vec2{kHudLeftX + 6.0f, 18.0f}, hpBuf, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.78f, 0U, 30U, kHudView);

            const f32 harmRatio = static_cast<f32>(ps->harmony) / static_cast<f32>(kMaxHarmonyGauge);
            list.PushProgressBar(
                en::Rect{kHudLeftX, 38.0f, 260.0f, 14.0f},
                harmRatio,
                Color{0.25f, 0.85f, 0.95f, 1.0f},
                Color{0.08f, 0.14f, 0.20f, 0.92f},
                Color{0.70f, 0.90f, 1.00f, 1.0f},
                0U, 25U, kHudView);

            const SacredChordSpec* chord = FindMatchingSacredChord(ps->deck);
            char harmBuf[96]{};
            if (ps->harmony >= kSacredChordHarmonyCost && chord != nullptr) {
                std::snprintf(harmBuf, sizeof(harmBuf), "HARMONY %u/100 [CHORD: %.*s]",
                              ps->harmony, static_cast<int>(chord->name.size()), chord->name.data());
            } else if (ps->harmony >= kUnisonHarmonyCost) {
                std::snprintf(harmBuf, sizeof(harmBuf), "HARMONY %u/100 [UNISON READY] | SHARDS %u",
                              ps->harmony, ps->carriedTuningShards);
            } else {
                std::snprintf(harmBuf, sizeof(harmBuf), "HARMONY %u/100 | SHARDS %u",
                              ps->harmony, ps->carriedTuningShards);
            }
            list.PushText(Vec2{kHudLeftX + 6.0f, 39.0f}, harmBuf, Color{0.95f, 0.98f, 1.0f, 1.0f}, 0.74f, 0U, 30U, kHudView);

            // Render Triad Deck slots with BOTH color swatch AND explicit [GLYPH] + Name (GDD v0.2 Section 3.1 & 4.9)
            for (u32 s = 0U; s < kTriadDeckSize; ++s) {
                const EchoIdentityDef& edef = GetEchoDef(ps->deck.slots[s]);
                const bool isActive = (ps->deck.activeSlot == s);
                const f32 boxX = kHudLeftX + static_cast<f32>(s) * 150.0f;
                const f32 boxY = 58.0f;
                list.PushRect(
                    en::Rect{boxX, boxY, 142.0f, 24.0f},
                    isActive ? Color{0.22f, 0.24f, 0.30f, 0.95f} : Color{0.10f, 0.11f, 0.14f, 0.85f},
                    0U, 24U, 32768U, kHudView);
                list.PushRect(
                    en::Rect{boxX + 4.0f, boxY + 3.0f, 18.0f, 18.0f},
                    edef.primaryColor,
                    0U, 26U, 32768U, kHudView);
                if (isActive) {
                    list.PushRectOutline(
                        en::Rect{boxX, boxY, 142.0f, 24.0f},
                        2.0f,
                        edef.primaryColor,
                        0U, 27U, 32768U, kHudView);
                }

                char slotText[48]{};
                std::snprintf(slotText, sizeof(slotText), "%s[%.*s] %.*s",
                              isActive ? ">" : " ",
                              static_cast<int>(edef.glyph.size()), edef.glyph.data(),
                              static_cast<int>(edef.name.size()), edef.name.data());
                list.PushText(Vec2{boxX + 26.0f, boxY + 5.0f}, slotText, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.72f, 0U, 30U, kHudView);
            }
        }
    }

    // 5. Lost Wick Husk HUD Banner when active
    if (mv.ActiveHusk().IsActive()) {
        char huskBuf[80]{};
        std::snprintf(huskBuf, sizeof(huskBuf), "LOST WICK HUSK ACTIVE (ROOM %u | %u SHARDS)",
                      mv.ActiveHusk().roomId, mv.ActiveHusk().bankedTuningShards);
        list.PushText(Vec2{kHudLeftX, 88.0f}, huskBuf, Color{1.0f, 0.78f, 0.30f, 1.0f}, 0.75f, 0U, 30U, kHudView);
    }

    // 6. World-space Enemy & Boss Combat Readability Indicators (HP, Break, Mark Glyph, Hit Flash, Reaction & Telegraph Rings)
    for (auto [id, recv, brk, mark] : world.View<en::DamageReceiver2D, BreakAndCcComp, ResonanceMarkComp>()) {
        if (recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) continue;
        const en::WorldTransform2D& xf = world.GetWorldByIndex(id.index);

        const f32 hpRatio = (recv.maxHp > 0) ? static_cast<f32>(recv.hp) / static_cast<f32>(recv.maxHp) : 0.0f;
        const f32 brkRatio = static_cast<f32>(brk.breakMeter) / static_cast<f32>(kMaxBreakMeter);

        // Background track + filled HP & Break bars above enemy head
        list.PushRect(en::Rect{xf.x - 0.72f, xf.y + 1.13f, 1.44f, 0.16f},
                      Color{0.08f, 0.08f, 0.10f, 0.90f}, 0U, 21U, 32768U, 0U);
        list.PushRect(en::Rect{xf.x - 0.70f, xf.y + 1.15f, 1.40f * hpRatio, 0.12f},
                      Color{0.92f, 0.22f, 0.28f, 0.98f}, 0U, 22U, 32768U, 0U);
        list.PushRect(en::Rect{xf.x - 0.70f, xf.y + 0.98f, 1.40f * brkRatio, 0.10f},
                      (brk.breakPhase == BreakPhase::BreakState)
                          ? Color{1.0f, 0.92f, 0.22f, 1.0f}
                          : Color{0.88f, 0.68f, 0.22f, 0.95f},
                      0U, 22U, 32768U, 0U);

        // Active Resonance Mark indicator (Color Ring + [GLYPH])
        if (mark.HasActiveMark()) {
            const EchoIdentityDef& mdef = GetEchoDef(mark.activeMark);
            list.PushRing(Vec2{xf.x, xf.y + 1.50f}, 0.14f, 0.26f, mdef.primaryColor, 0U, 24U, 0U);
            list.PushText(Vec2{xf.x - 0.35f, xf.y + 1.75f}, mdef.glyph, mdef.primaryColor, 0.028f, 0U, 25U, 0U);
        }

        // Hit-flash impact burst when struck by player
        if (mark.hitFlashTicks > 0U) {
            list.PushRing(
                Vec2{xf.x, xf.y + 0.1f},
                0.35f, 0.78f,
                Color{1.0f, 0.96f, 0.82f, 0.95f},
                0U, 23U, 0U);
        }

        // Wave Reaction detonation ring when a 2-Echo reaction triggers
        if (mark.reactionVfxTicks > 0U) {
            const f32 prog = 1.0f - (static_cast<f32>(mark.reactionVfxTicks) / 30.0f);
            const f32 rad = 0.65f + prog * 1.65f;
            list.PushRing(
                Vec2{xf.x, xf.y + 0.1f},
                rad - 0.18f,
                rad,
                Color{0.98f, 0.85f, 0.32f, 0.92f},
                0U, 24U, 0U);
        }

        // Enemy attack wind-up telegraph ring (Gold = Parryable, Crimson = Unparryable)
        if (const auto* ai = world.TryGet<EnemyAiComp>(id)) {
            if (ai->actionTimerTicks > 0U) {
                const bool isCrimson = (ai->activeTelegraph == AttackTelegraphKind::CrimsonOverloadWave);
                const Color teleColor = isCrimson
                                            ? Color{1.0f, 0.18f, 0.24f, 0.92f}
                                            : Color{1.0f, 0.86f, 0.22f, 0.92f};
                list.PushRing(
                    Vec2{xf.x, xf.y + 0.1f},
                    0.60f, 0.82f,
                    teleColor,
                    0U, 22U, 0U);
            }
        }

        // Boss HUD Banner at bottom of screen
        if (brk.category == TargetCategory::Boss) {
            list.PushProgressBar(
                en::Rect{340.0f, 650.0f, 600.0f, 20.0f},
                hpRatio,
                Color{0.92f, 0.18f, 0.28f, 1.0f},
                Color{0.12f, 0.06f, 0.10f, 0.92f},
                Color{0.95f, 0.82f, 0.40f, 1.0f},
                0U, 26U, kHudView);

            list.PushProgressBar(
                en::Rect{340.0f, 674.0f, 600.0f, 12.0f},
                brkRatio,
                Color{0.95f, 0.80f, 0.25f, 1.0f},
                Color{0.10f, 0.10f, 0.12f, 0.90f},
                Color{0.80f, 0.75f, 0.60f, 1.0f},
                0U, 26U, kHudView);

            const auto* ai = world.TryGet<EnemyAiComp>(id);
            char bossBuf[128]{};
            std::snprintf(bossBuf, sizeof(bossBuf),
                          "THE HIGH CONDUCTOR OF THE SPIRE (PHASE %u) | BREAK %u/100 [%s]",
                          ai ? ai->bossPhase : 1U,
                          brk.breakMeter,
                          brk.breakPhase == BreakPhase::BreakState ? "BREAK STATE 1.25X" :
                          (brk.breakPhase == BreakPhase::BreakRecovery ? "RECOVERY IMMUNE" : "ACTIVE"));
            list.PushText(Vec2{344.0f, 628.0f}, bossBuf, Color{1.0f, 0.94f, 0.75f, 1.0f}, 0.85f, 0U, 30U, kHudView);
        }
    }
}

void HudAndMenuSystem::EmitTitleModal(en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;
    list.PushGradientRect(
        en::Rect{0.0f, 0.0f, 1280.0f, 720.0f},
        Color{0.05f, 0.06f, 0.10f, 0.95f},
        Color{0.12f, 0.08f, 0.18f, 0.95f},
        true, 0U, 28U, 32768U, kHudView);
    list.PushText(Vec2{420.0f, 240.0f}, "VEIL OF THE SILENT", Color{1.0f, 0.92f, 0.70f, 1.0f}, 1.8f, 0U, 30U, kHudView);
    list.PushText(Vec2{390.0f, 300.0f}, "PHAROS OF TWELVE WICKS - GDD V0.2", Color{0.70f, 0.88f, 1.0f, 1.0f}, 1.0f, 0U, 30U, kHudView);
    list.PushText(Vec2{440.0f, 400.0f}, "PRESS ENTER / START TO AWAKEN", Color{0.95f, 0.95f, 0.95f, 1.0f}, 0.95f, 0U, 30U, kHudView);
}

void HudAndMenuSystem::EmitResonatorDeckModal(
    EntityId playerId,
    const en::World& world,
    en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;
    list.PushRect(en::Rect{240.0f, 120.0f, 800.0f, 480.0f}, Color{0.08f, 0.10f, 0.15f, 0.94f}, 0U, 28U, 32768U, kHudView);
    list.PushRectOutline(en::Rect{240.0f, 120.0f, 800.0f, 480.0f}, 2.0f, Color{0.90f, 0.78f, 0.40f, 1.0f}, 0U, 29U, 32768U, kHudView);
    list.PushText(Vec2{280.0f, 148.0f}, "TUNING RESONATOR - TRIAD DECK CONFIGURATION", Color{1.0f, 0.92f, 0.65f, 1.0f}, 1.0f, 0U, 30U, kHudView);

    if (world.IsAlive(playerId)) {
        if (const auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
            for (u32 i = 0U; i < kEchoCount; ++i) {
                const EchoIdentityDef& def = kEchoTable[i];
                const bool unlocked = ((ps->unlockedEchoMask & (1U << i)) != 0U);
                const f32 x = 280.0f + static_cast<f32>(i % 3U) * 240.0f;
                const f32 y = 210.0f + static_cast<f32>(i / 3U) * 75.0f;
                list.PushRect(en::Rect{x, y, 220.0f, 56.0f},
                              unlocked ? Color{0.16f, 0.18f, 0.24f, 0.95f} : Color{0.07f, 0.07f, 0.09f, 0.8f},
                              0U, 29U, 32768U, kHudView);
                char buf[64]{};
                std::snprintf(buf, sizeof(buf), "[%.*s] %.*s %s",
                              static_cast<int>(def.glyph.size()), def.glyph.data(),
                              static_cast<int>(def.name.size()), def.name.data(),
                              unlocked ? "" : "(LOCKED)");
                list.PushText(Vec2{x + 12.0f, y + 18.0f}, buf,
                              unlocked ? def.primaryColor : Color{0.45f, 0.45f, 0.50f, 1.0f},
                              0.78f, 0U, 30U, kHudView);
            }
        }
    }
}

void HudAndMenuSystem::EmitMapAndPinsModal(
    const MetroidvaniaSystem& mv,
    u8 activeRoomId,
    en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;
    list.PushRect(en::Rect{200.0f, 100.0f, 880.0f, 520.0f}, Color{0.06f, 0.09f, 0.14f, 0.94f}, 0U, 28U, 32768U, kHudView);
    list.PushText(Vec2{240.0f, 128.0f}, "VAELEN ACOUSTIC MAP & PLAYER PINS", Color{0.75f, 0.92f, 1.0f, 1.0f}, 1.0f, 0U, 30U, kHudView);

    for (u8 r = 0U; r < kRoomCount; ++r) {
        const f32 rx = 250.0f + static_cast<f32>(r) * 195.0f;
        const f32 ry = 220.0f;
        const bool disc = mv.IsRoomDiscovered(r);
        list.PushRect(en::Rect{rx, ry, 175.0f, 90.0f},
                      disc ? (r == activeRoomId ? Color{0.25f, 0.42f, 0.60f, 0.95f} : Color{0.14f, 0.22f, 0.32f, 0.90f})
                           : Color{0.08f, 0.08f, 0.10f, 0.60f},
                      0U, 29U, 32768U, kHudView);
        list.PushText(Vec2{rx + 10.0f, ry + 36.0f},
                      disc ? kRoomTitles[r] : "??? (FOG OF WAR)",
                      Color{0.95f, 0.95f, 1.0f, 1.0f}, 0.65f, 0U, 30U, kHudView);
    }

    char pinSummary[64]{};
    std::snprintf(pinSummary, sizeof(pinSummary), "PLACED MAP PINS: %zu / %u",
                  mv.MapPins().size(), kMaxMapPins);
    list.PushText(Vec2{250.0f, 360.0f}, pinSummary, Color{0.95f, 0.88f, 0.60f, 1.0f}, 0.85f, 0U, 30U, kHudView);
}

void HudAndMenuSystem::EmitPauseModal(en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;
    list.PushRect(en::Rect{440.0f, 220.0f, 400.0f, 260.0f}, Color{0.07f, 0.08f, 0.12f, 0.95f}, 0U, 28U, 32768U, kHudView);
    list.PushRectOutline(en::Rect{440.0f, 220.0f, 400.0f, 260.0f}, 2.0f, Color{0.75f, 0.85f, 1.0f, 1.0f}, 0U, 29U, 32768U, kHudView);
    list.PushText(Vec2{540.0f, 260.0f}, "PAUSED", Color{1.0f, 1.0f, 1.0f, 1.0f}, 1.3f, 0U, 30U, kHudView);
    list.PushText(Vec2{485.0f, 340.0f}, "PRESS ESC / BACK TO RESUME", Color{0.85f, 0.90f, 0.95f, 1.0f}, 0.85f, 0U, 30U, kHudView);
}

void HudAndMenuSystem::EmitEndingModal(
    EndingId ending,
    en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;
    list.PushRect(en::Rect{180.0f, 140.0f, 920.0f, 440.0f}, Color{0.05f, 0.06f, 0.10f, 0.96f}, 0U, 28U, 32768U, kHudView);
    std::string_view title = "ENDING A - THE FALSE DAWN";
    std::string_view desc = "Kaelen ascends to the living surface, leaving the Spire's harvesting network intact below.";
    if (ending == EndingId::ShatteredSpire) {
        title = "ENDING B - THE SHATTERED SPIRE";
        desc = "The Spire core shatters and Tuning Collars fall silent across Vaelen's destabilized halls.";
    } else if (ending == EndingId::SymphonyOfOffBeat) {
        title = "ENDING C - SYMPHONY OF THE OFF-BEAT";
        desc = "All 12 Echoes and Memory Tuning harmonize the Spire; Kaelen relinquishes the Lantern's blade.";
    }
    list.PushText(Vec2{240.0f, 220.0f}, title, Color{1.0f, 0.92f, 0.65f, 1.0f}, 1.3f, 0U, 30U, kHudView);
    list.PushText(Vec2{240.0f, 300.0f}, desc, Color{0.88f, 0.94f, 1.0f, 1.0f}, 0.85f, 0U, 30U, kHudView);
}

} // namespace vots
