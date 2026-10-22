#include "game/ui/HudAndMenuSystem.h"

#include <cstdio>

namespace vots {

void HudAndMenuSystem::EmitGameplayHudAndWorldIndicators(
    EntityId playerId,
    const en::World& world,
    const MetroidvaniaSystem& mv,
    en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;

    // 1. Player Top-Left HUD: HP Bar (with Husk -25% penalty indicator), Harmony Bar, Shards, and Triad Deck
    if (world.IsAlive(playerId)) {
        const auto* ps = world.TryGet<PlayerStateComp>(playerId);
        const auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
        if (ps != nullptr && recv != nullptr) {
            const f32 hpRatio = (recv->maxHp > 0)
                                    ? static_cast<f32>(recv->hp) / static_cast<f32>(recv->maxHp)
                                    : 0.0f;
            list.PushProgressBar(
                en::Rect{24.0f, 20.0f, 240.0f, 18.0f},
                hpRatio,
                Color{0.88f, 0.22f, 0.28f, 1.0f},
                Color{0.15f, 0.08f, 0.10f, 0.9f},
                Color{0.92f, 0.85f, 0.75f, 1.0f},
                0U, 25U, kHudView);

            char hpBuf[64]{};
            std::snprintf(hpBuf, sizeof(hpBuf), "HP %d/%d%s",
                          recv->hp, recv->maxHp,
                          (ps->huskPenaltyActive != 0U) ? " [HUSK -25%]" : "");
            list.PushText(Vec2{30.0f, 22.0f}, hpBuf, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.85f, 0U, 30U, kHudView);

            const f32 harmRatio = static_cast<f32>(ps->harmony) / static_cast<f32>(kMaxHarmonyGauge);
            list.PushProgressBar(
                en::Rect{24.0f, 44.0f, 240.0f, 14.0f},
                harmRatio,
                Color{0.25f, 0.85f, 0.95f, 1.0f},
                Color{0.08f, 0.14f, 0.20f, 0.9f},
                Color{0.70f, 0.90f, 1.00f, 1.0f},
                0U, 25U, kHudView);

            const SacredChordSpec* chord = FindMatchingSacredChord(ps->deck);
            char harmBuf[96]{};
            if (ps->harmony >= kSacredChordHarmonyCost && chord != nullptr) {
                std::snprintf(harmBuf, sizeof(harmBuf), "HARMONY %u/100 [CHORD: %.*s]",
                              ps->harmony, static_cast<int>(chord->name.size()), chord->name.data());
            } else if (ps->harmony >= kUnisonHarmonyCost) {
                std::snprintf(harmBuf, sizeof(harmBuf), "HARMONY %u/100 [UNISON READY]", ps->harmony);
            } else {
                std::snprintf(harmBuf, sizeof(harmBuf), "HARMONY %u/100 | SHARDS %u",
                              ps->harmony, ps->carriedTuningShards);
            }
            list.PushText(Vec2{30.0f, 45.0f}, harmBuf, Color{0.95f, 0.98f, 1.0f, 1.0f}, 0.75f, 0U, 30U, kHudView);

            // Render Triad Deck slots with BOTH color swatch AND explicit [GLYPH] + Name (GDD v0.2 Section 3.1 & 4.9)
            for (u32 s = 0U; s < kTriadDeckSize; ++s) {
                const EchoIdentityDef& edef = GetEchoDef(ps->deck.slots[s]);
                const bool isActive = (ps->deck.activeSlot == s);
                const f32 boxX = 24.0f + static_cast<f32>(s) * 150.0f;
                const f32 boxY = 66.0f;
                list.PushRect(
                    en::Rect{boxX, boxY, 142.0f, 26.0f},
                    isActive ? Color{0.22f, 0.24f, 0.30f, 0.95f} : Color{0.10f, 0.11f, 0.14f, 0.85f},
                    0U, 24U, 32768U, kHudView);
                list.PushRect(
                    en::Rect{boxX + 4.0f, boxY + 4.0f, 18.0f, 18.0f},
                    edef.primaryColor,
                    0U, 26U, 32768U, kHudView);
                if (isActive) {
                    list.PushRectOutline(
                        en::Rect{boxX, boxY, 142.0f, 26.0f},
                        2.0f,
                        edef.primaryColor,
                        0U, 27U, 32768U, kHudView);
                }

                char slotText[48]{};
                std::snprintf(slotText, sizeof(slotText), "%s[%.*s] %.*s",
                              isActive ? ">" : " ",
                              static_cast<int>(edef.glyph.size()), edef.glyph.data(),
                              static_cast<int>(edef.name.size()), edef.name.data());
                list.PushText(Vec2{boxX + 26.0f, boxY + 6.0f}, slotText, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.72f, 0U, 30U, kHudView);
            }
        }
    }

    // 2. Lost Wick Husk HUD Banner when active
    if (mv.ActiveHusk().IsActive()) {
        char huskBuf[80]{};
        std::snprintf(huskBuf, sizeof(huskBuf), "LOST WICK HUSK ACTIVE (ROOM %u | %u SHARDS)",
                      mv.ActiveHusk().roomId, mv.ActiveHusk().bankedTuningShards);
        list.PushText(Vec2{24.0f, 100.0f}, huskBuf, Color{1.0f, 0.75f, 0.30f, 1.0f}, 0.75f, 0U, 30U, kHudView);
    }

    // 3. World-space Enemy & Boss Combat Readability Indicators (Mark Glyph, Break Meter, Telegraph Icon)
    for (auto [id, recv, brk, mark] : world.View<en::DamageReceiver2D, BreakAndCcComp, ResonanceMarkComp>()) {
        if (recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) continue;
        const en::WorldTransform2D& xf = world.GetWorldByIndex(id.index);

        // Above-head HP & Break bars in world view 0
        const f32 hpRatio = (recv.maxHp > 0) ? static_cast<f32>(recv.hp) / static_cast<f32>(recv.maxHp) : 0.0f;
        const f32 brkRatio = static_cast<f32>(brk.breakMeter) / static_cast<f32>(kMaxBreakMeter);
        list.PushRect(en::Rect{xf.x - 0.7f, xf.y + 1.15f, 1.4f * hpRatio, 0.12f},
                      Color{0.90f, 0.20f, 0.25f, 0.95f}, 0U, 22U, 32768U, 0U);
        list.PushRect(en::Rect{xf.x - 0.7f, xf.y + 1.00f, 1.4f * brkRatio, 0.10f},
                      (brk.breakPhase == BreakPhase::BreakState)
                          ? Color{1.0f, 0.90f, 0.20f, 1.0f}
                          : Color{0.85f, 0.65f, 0.20f, 0.95f},
                      0U, 22U, 32768U, 0U);

        // Active Resonance Mark indicator (Color + Glyph)
        if (mark.HasActiveMark()) {
            const EchoIdentityDef& mdef = GetEchoDef(mark.activeMark);
            list.PushRing(Vec2{xf.x, xf.y + 1.45f}, 0.12f, 0.22f, mdef.primaryColor, 0U, 24U, 0U);
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

    constexpr std::array<std::string_view, kRoomCount> kRoomNames{{
        "0: ASH-SUMP SLUMS",
        "1: RESONATOR SANCTUM",
        "2: ECHO VAULT",
        "3: SPIRE OF UNISON",
    }};
    for (u8 r = 0U; r < kRoomCount; ++r) {
        const f32 rx = 250.0f + static_cast<f32>(r) * 195.0f;
        const f32 ry = 220.0f;
        const bool disc = mv.IsRoomDiscovered(r);
        list.PushRect(en::Rect{rx, ry, 175.0f, 90.0f},
                      disc ? (r == activeRoomId ? Color{0.25f, 0.42f, 0.60f, 0.95f} : Color{0.14f, 0.22f, 0.32f, 0.90f})
                           : Color{0.08f, 0.08f, 0.10f, 0.60f},
                      0U, 29U, 32768U, kHudView);
        list.PushText(Vec2{rx + 10.0f, ry + 36.0f},
                      disc ? kRoomNames[r] : "??? (FOG OF WAR)",
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
