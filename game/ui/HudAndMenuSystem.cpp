#include "game/ui/HudAndMenuSystem.h"
#include "game/ui/WorldVfxRenderer.h"
#include "game/world/WorldTopology.h"

#include <cstdio>

namespace vots {

void HudAndMenuSystem::EmitGameplayHudAndWorldIndicators(
    EntityId playerId,
    const en::World& world,
    const en::PhysicsSystem* physics,
    const MetroidvaniaSystem& mv,
    u8 activeRoomId,
    en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;

    WorldVfxRenderer::EmitWorldBackdropTilemapAndEntities(playerId, world, physics, activeRoomId, list);

    for (auto [id, recv, brk] : world.View<en::DamageReceiver2D, BreakAndCcComp>()) {
        if (brk.category != TargetCategory::Boss || recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) continue;
        const auto* ai = world.TryGet<EnemyAiComp>(id);
        const f32 hpRatio = (recv.maxHp > 0) ? static_cast<f32>(recv.hp) / static_cast<f32>(recv.maxHp) : 0.0f;
        const f32 brkRatio = static_cast<f32>(brk.breakMeter) / static_cast<f32>(kMaxBreakMeter);

        list.PushProgressBar(
            en::Rect{340.0f, 650.0f, 600.0f, 20.0f}, hpRatio,
            Color{0.92f, 0.18f, 0.28f, 1.0f}, Color{0.12f, 0.06f, 0.10f, 0.92f}, Color{0.95f, 0.82f, 0.40f, 1.0f},
            0U, 26U, kHudView);
        list.PushProgressBar(
            en::Rect{340.0f, 674.0f, 600.0f, 12.0f}, brkRatio,
            Color{0.95f, 0.80f, 0.25f, 1.0f}, Color{0.10f, 0.10f, 0.12f, 0.90f}, Color{0.80f, 0.75f, 0.60f, 1.0f},
            0U, 26U, kHudView);

        char bossBuf[128]{};
        std::snprintf(bossBuf, sizeof(bossBuf),
                      "SANCTUM BOSS (PHASE %u/%u | HP %d/%d) | BREAK %u/100",
                      ai ? ai->bossPhase : 1U, kBossTotalPhases, recv.hp, recv.maxHp, brk.breakMeter);
        list.PushText(Vec2{340.0f, 628.0f}, bossBuf, Color{1.0f, 0.94f, 0.75f, 1.0f}, 0.82f, 0U, 30U, kHudView);
    }

    const auto* ps = world.IsAlive(playerId) ? world.TryGet<PlayerStateComp>(playerId) : nullptr;
    constexpr f32 kHudLeftX = 350.0f;

    if (ps != nullptr) {
        const auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
        if (recv != nullptr) {
            const RoomTopologySpec& roomSpec = GetRoomTopology(activeRoomId);
            const f32 hpRatio = (recv->maxHp > 0) ? static_cast<f32>(recv->hp) / static_cast<f32>(recv->maxHp) : 0.0f;
            const bool hurtVignette = (recv->iframeTicks > (kPlayerHitProtectionTicks - kPlayerHurtFlashDurationTicks));

            // Screen-Space Crimson Hit Vignette Flash around all 4 viewport borders when Kaelen takes damage!
            if (hurtVignette) {
                const Color vigColor{0.96f, 0.12f, 0.20f, 0.52f};
                list.PushRect(en::Rect{0.0f, 0.0f, 1280.0f, 18.0f}, vigColor, 0U, 29U, 32768U, kHudView);
                list.PushRect(en::Rect{0.0f, 702.0f, 1280.0f, 18.0f}, vigColor, 0U, 29U, 32768U, kHudView);
                list.PushRect(en::Rect{0.0f, 18.0f, 18.0f, 684.0f}, vigColor, 0U, 29U, 32768U, kHudView);
                list.PushRect(en::Rect{1262.0f, 18.0f, 18.0f, 684.0f}, vigColor, 0U, 29U, 32768U, kHudView);
            }

            list.PushProgressBar(
                en::Rect{kHudLeftX, 16.0f, 260.0f, 18.0f}, hpRatio,
                hurtVignette ? Color{1.0f, 0.95f, 0.95f, 1.0f} : Color{0.88f, 0.22f, 0.28f, 1.0f},
                Color{0.15f, 0.08f, 0.10f, 0.92f},
                hurtVignette ? Color{1.0f, 0.25f, 0.30f, 1.0f} : Color{0.92f, 0.85f, 0.75f, 1.0f},
                0U, 25U, kHudView);

            char hpBuf[144]{};
            std::snprintf(hpBuf, sizeof(hpBuf), "HP %d/%d%s | DASH %u/%u | [%.*s] %.*s (%ux%um)",
                          recv->hp, recv->maxHp,
                          ps->HuskPenaltyActive() ? " [HUSK 75]" : (recv->iframeTicks > 0U ? " [I-FRAME]" : ""),
                          ps->airDashCharges, ps->MaxDashCharges(),
                          static_cast<int>(roomSpec.gddRoomCode.size()), roomSpec.gddRoomCode.data(),
                          static_cast<int>(roomSpec.hudTitle.size()), roomSpec.hudTitle.data(),
                          roomSpec.tileWidth, roomSpec.tileHeight);
            list.PushText(Vec2{kHudLeftX + 6.0f, 18.0f}, hpBuf, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.70f, 0U, 30U, kHudView);

            const f32 harmRatio = static_cast<f32>(ps->harmony) / static_cast<f32>(kMaxHarmonyGauge);
            list.PushProgressBar(
                en::Rect{kHudLeftX, 38.0f, 260.0f, 14.0f}, harmRatio,
                Color{0.25f, 0.85f, 0.95f, 1.0f}, Color{0.08f, 0.14f, 0.20f, 0.92f}, Color{0.70f, 0.90f, 1.00f, 1.0f},
                0U, 25U, kHudView);

            const SacredChordSpec* chord = FindMatchingSacredChord(ps->deck);
            char harmBuf[144]{};
            if (ps->harmony >= kSacredChordHarmonyCost && chord != nullptr) {
                std::snprintf(harmBuf, sizeof(harmBuf), "HARMONY %u/100 [T/O CHORD: %.*s]",
                              ps->harmony, static_cast<int>(chord->name.size()), chord->name.data());
            } else {
                std::snprintf(harmBuf, sizeof(harmBuf),
                              "HARMONY %u/100 | SHARDS %u | %.*s",
                              ps->harmony, ps->carriedTuningShards,
                              static_cast<int>(roomSpec.inferredObjective.size()), roomSpec.inferredObjective.data());
            }
            list.PushText(Vec2{kHudLeftX + 6.0f, 39.0f}, harmBuf, Color{0.95f, 0.98f, 1.0f, 1.0f}, 0.66f, 0U, 30U, kHudView);

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
                    edef.primaryColor, 0U, 26U, 32768U, kHudView);
                if (isActive) {
                    list.PushRectOutline(
                        en::Rect{boxX, boxY, 142.0f, 24.0f}, 2.0f, edef.primaryColor, 0U, 27U, 32768U, kHudView);
                }

                char slotText[48]{};
                std::snprintf(slotText, sizeof(slotText), "%u:%s[%.*s] %.*s",
                              s + 1U, isActive ? ">" : "",
                              static_cast<int>(edef.glyph.size()), edef.glyph.data(),
                              static_cast<int>(edef.name.size()), edef.name.data());
                list.PushText(Vec2{boxX + 26.0f, boxY + 5.0f}, slotText, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.70f, 0U, 30U, kHudView);
            }
        }
    }

    if (mv.ActiveHusk().IsActive()) {
        char huskBuf[80]{};
        std::snprintf(huskBuf, sizeof(huskBuf), "LOST WICK HUSK ACTIVE (ROOM %u | %u SHARDS)",
                      mv.ActiveHusk().roomId, mv.ActiveHusk().bankedTuningShards);
        list.PushText(Vec2{kHudLeftX, 88.0f}, huskBuf, Color{1.0f, 0.78f, 0.30f, 1.0f}, 0.75f, 0U, 30U, kHudView);
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
    list.PushText(Vec2{380.0f, 300.0f}, "PHAROS OF TWELVE WICKS - GDD V0.5", Color{0.70f, 0.88f, 1.0f, 1.0f}, 1.0f, 0U, 30U, kHudView);
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
    list.PushRect(en::Rect{140.0f, 70.0f, 1000.0f, 580.0f}, Color{0.06f, 0.09f, 0.14f, 0.95f}, 0U, 28U, 32768U, kHudView);
    list.PushText(Vec2{180.0f, 96.0f}, "VAELEN LEVEL GRAPH & MEMORY TUNING JOURNAL (GDD V0.5)", Color{0.75f, 0.92f, 1.0f, 1.0f}, 1.0f, 0U, 30U, kHudView);

    for (u8 r = 0U; r < kRoomCount; ++r) {
        const RoomTopologySpec& rSpec = GetRoomTopology(r);
        const u8 col = static_cast<u8>(r % 4U);
        const u8 row = static_cast<u8>(r / 4U);
        const f32 rx = 180.0f + static_cast<f32>(col) * 230.0f;
        const f32 ry = 148.0f + static_cast<f32>(row) * 125.0f;
        const bool disc = mv.IsRoomDiscovered(r);
        list.PushRect(en::Rect{rx, ry, 210.0f, 92.0f},
                      disc ? (r == activeRoomId ? Color{0.25f, 0.42f, 0.60f, 0.95f} : Color{0.14f, 0.22f, 0.32f, 0.90f})
                           : Color{0.08f, 0.08f, 0.10f, 0.60f},
                      0U, 29U, 32768U, kHudView);
        list.PushText(Vec2{rx + 8.0f, ry + 26.0f},
                      disc ? rSpec.hudTitle : "??? (FOG OF WAR)",
                      Color{0.95f, 0.95f, 1.0f, 1.0f}, 0.60f, 0U, 30U, kHudView);
        if (disc) {
            list.PushText(Vec2{rx + 8.0f, ry + 54.0f},
                          rSpec.landmarkName, Color{0.72f, 0.88f, 0.96f, 1.0f}, 0.54f, 0U, 30U, kHudView);
        }
    }

    for (u32 m = 0U; m < kMemoryTuningFlagCount; ++m) {
        const MemoryTuningSpec& mSpec = kMemoryTuningSpecTable[m];
        list.PushText(Vec2{180.0f, 415.0f + static_cast<f32>(m) * 32.0f},
                      mSpec.title, Color{0.98f, 0.86f, 0.55f, 1.0f}, 0.72f, 0U, 30U, kHudView);
        list.PushText(Vec2{360.0f, 415.0f + static_cast<f32>(m) * 32.0f},
                      mSpec.journalSummary, Color{0.84f, 0.90f, 0.98f, 1.0f}, 0.64f, 0U, 30U, kHudView);
    }

    char pinSummary[96]{};
    std::snprintf(pinSummary, sizeof(pinSummary), "PLACED MAP PINS: %zu / %u | WALK OFF ROOM EDGES TO TRAVERSE CONNECTED REGIONS",
                  mv.MapPins().size(), kMaxMapPins);
    list.PushText(Vec2{180.0f, 565.0f}, pinSummary, Color{0.95f, 0.88f, 0.60f, 1.0f}, 0.80f, 0U, 30U, kHudView);
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
        desc = "All 12 Echoes and 4 Memory Tuning flags rewrite the relay; Kaelen relinquishes the Lantern's blade.";
    }
    list.PushText(Vec2{240.0f, 220.0f}, title, Color{1.0f, 0.92f, 0.65f, 1.0f}, 1.3f, 0U, 30U, kHudView);
    list.PushText(Vec2{240.0f, 300.0f}, desc, Color{0.88f, 0.94f, 1.0f, 1.0f}, 0.85f, 0U, 30U, kHudView);
}

} // namespace vots
