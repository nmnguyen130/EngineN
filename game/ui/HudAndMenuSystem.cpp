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
            en::Rect{340.0f, 656.0f, 600.0f, 16.0f}, hpRatio,
            Color{0.92f, 0.18f, 0.28f, 1.0f}, Color{0.10f, 0.05f, 0.08f, 0.92f}, Color{0.92f, 0.78f, 0.38f, 1.0f},
            0U, 26U, kHudView);
        list.PushProgressBar(
            en::Rect{340.0f, 676.0f, 600.0f, 8.0f}, brkRatio,
            Color{0.95f, 0.80f, 0.25f, 1.0f}, Color{0.08f, 0.08f, 0.10f, 0.90f}, Color{0.75f, 0.70f, 0.55f, 1.0f},
            0U, 26U, kHudView);

        char bossBuf[80]{};
        std::snprintf(bossBuf, sizeof(bossBuf), "ASH-SUMP WARDEN  [PHASE %u/%u]",
                      ai ? ai->bossPhase : 1U, kBossTotalPhases);
        list.PushText(Vec2{520.0f, 636.0f}, bossBuf, Color{1.0f, 0.92f, 0.72f, 1.0f}, 0.85f, 0U, 30U, kHudView);
    }

    const auto* ps = world.IsAlive(playerId) ? world.TryGet<PlayerStateComp>(playerId) : nullptr;
    if (ps == nullptr) return;
    const auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (recv == nullptr) return;

    const bool hurtVignette = (recv->iframeTicks > (kPlayerHitProtectionTicks - kPlayerHurtFlashDurationTicks));
    if (hurtVignette) {
        const Color vigColor{0.96f, 0.12f, 0.20f, 0.45f};
        list.PushRect(en::Rect{0.0f, 0.0f, 1280.0f, 14.0f}, vigColor, 0U, 29U, 32768U, kHudView);
        list.PushRect(en::Rect{0.0f, 706.0f, 1280.0f, 14.0f}, vigColor, 0U, 29U, 32768U, kHudView);
        list.PushRect(en::Rect{0.0f, 14.0f, 14.0f, 692.0f}, vigColor, 0U, 29U, 32768U, kHudView);
        list.PushRect(en::Rect{1266.0f, 14.0f, 14.0f, 692.0f}, vigColor, 0U, 29U, 32768U, kHudView);
    }

    // Flush Top-Left Dark-Fantasy Metroidvania Player HUD Frame (x = 14, y = 12)
    constexpr f32 kLeftX = 14.0f;
    constexpr f32 kTopY  = 12.0f;
    const EchoIdentityDef& activeEchoDef = GetEchoDef(ps->deck.ActiveEcho());

    // Backing plate & antique bronze-gold trim
    list.PushRect(
        en::Rect{kLeftX, kTopY, 248.0f, 64.0f},
        Color{0.05f, 0.06f, 0.10f, 0.88f},
        0U, 24U, 32768U, kHudView);
    list.PushRectOutline(
        en::Rect{kLeftX, kTopY, 248.0f, 64.0f},
        1.5f, Color{0.56f, 0.48f, 0.34f, 0.90f},
        0U, 25U, 32768U, kHudView);

    // Left Pharos Lantern crest diamond (shows active Echo resonance color)
    list.PushRect(
        en::Rect{kLeftX + 6.0f, kTopY + 8.0f, 22.0f, 22.0f},
        Color{0.10f, 0.12f, 0.18f, 0.96f},
        0U, 25U, 32768U, kHudView);
    list.PushRectOutline(
        en::Rect{kLeftX + 6.0f, kTopY + 8.0f, 22.0f, 22.0f},
        1.5f, Color{0.92f, 0.80f, 0.46f, 0.98f},
        0U, 26U, 32768U, kHudView);
    list.PushRect(
        en::Rect{kLeftX + 11.0f, kTopY + 13.0f, 12.0f, 12.0f},
        activeEchoDef.primaryColor,
        0U, 27U, 32768U, kHudView);

    // 1. Ornate Segmented Vitality (HP) Bar (x = 40, y = 18, w = 212, h = 14)
    constexpr f32 kHpX = kLeftX + 26.0f;
    constexpr f32 kHpY = kTopY + 6.0f;
    constexpr f32 kHpW = 212.0f;
    constexpr f32 kHpH = 14.0f;
    const f32 hpScaleDenom = 100.0f;
    const f32 curHpFillW = std::clamp(static_cast<f32>(recv->hp) / hpScaleDenom, 0.0f, 1.0f) * (kHpW - 4.0f);

    list.PushRect(
        en::Rect{kHpX, kHpY, kHpW, kHpH},
        Color{0.12f, 0.05f, 0.07f, 0.96f},
        0U, 25U, 32768U, kHudView);

    // Lost Wick Husk 25% Max-HP Lock Indicator on right end of HP bar
    if (ps->HuskPenaltyActive()) {
        const f32 lockW = (kHpW - 4.0f) * 0.25f;
        list.PushRect(
            en::Rect{kHpX + kHpW - 2.0f - lockW, kHpY + 2.0f, lockW, kHpH - 4.0f},
            Color{0.38f, 0.26f, 0.10f, 0.92f},
            0U, 26U, 32768U, kHudView);
    }

    if (curHpFillW > 0.5f) {
        const Color hpMainCol = hurtVignette
                                    ? Color{1.0f, 0.94f, 0.94f, 1.0f}
                                    : Color{0.86f, 0.15f, 0.23f, 1.0f};
        const Color hpSheenCol = hurtVignette
                                     ? Color{1.0f, 1.0f, 1.0f, 1.0f}
                                     : Color{1.0f, 0.45f, 0.50f, 0.92f};
        list.PushRect(
            en::Rect{kHpX + 2.0f, kHpY + 2.0f, curHpFillW, kHpH - 4.0f},
            hpMainCol,
            0U, 26U, 32768U, kHudView);
        list.PushRect(
            en::Rect{kHpX + 2.0f, kHpY + 2.0f, curHpFillW, 3.5f},
            hpSheenCol,
            0U, 27U, 32768U, kHudView);
    }

    // 25 / 50 / 75 HP segment notch ticks
    for (u32 seg = 1U; seg <= 3U; ++seg) {
        const f32 sx = kHpX + 2.0f + (kHpW - 4.0f) * (static_cast<f32>(seg) * 0.25f);
        list.PushRect(
            en::Rect{sx, kHpY + 2.0f, 1.5f, kHpH - 4.0f},
            Color{0.06f, 0.04f, 0.06f, 0.85f},
            0U, 28U, 32768U, kHudView);
    }

    list.PushRectOutline(
        en::Rect{kHpX, kHpY, kHpW, kHpH},
        1.5f,
        hurtVignette ? Color{1.0f, 0.30f, 0.35f, 1.0f} : Color{0.86f, 0.74f, 0.48f, 0.98f},
        0U, 28U, 32768U, kHudView);

    char hpNumBuf[24]{};
    std::snprintf(hpNumBuf, sizeof(hpNumBuf), "%d/%d", recv->hp, recv->maxHp);
    list.PushText(
        Vec2{kHpX + kHpW - 54.0f, kHpY + 3.0f},
        hpNumBuf, Color{1.0f, 0.98f, 0.94f, 1.0f}, 0.75f, 0U, 30U, kHudView);

    // 2. Sleek Cyan Harmony Gauge (x = 40, y = 36, w = 168, h = 8)
    constexpr f32 kHarmX = kHpX;
    constexpr f32 kHarmY = kHpY + 18.0f;
    constexpr f32 kHarmW = 168.0f;
    constexpr f32 kHarmH = 8.0f;
    const f32 harmRatio = std::clamp(static_cast<f32>(ps->harmony) / static_cast<f32>(kMaxHarmonyGauge), 0.0f, 1.0f);
    const f32 harmFillW = harmRatio * (kHarmW - 2.0f);

    list.PushRect(
        en::Rect{kHarmX, kHarmY, kHarmW, kHarmH},
        Color{0.06f, 0.11f, 0.17f, 0.95f},
        0U, 25U, 32768U, kHudView);
    if (harmFillW > 0.5f) {
        list.PushRect(
            en::Rect{kHarmX + 1.0f, kHarmY + 1.0f, harmFillW, kHarmH - 2.0f},
            Color{0.20f, 0.82f, 0.96f, 1.0f},
            0U, 26U, 32768U, kHudView);
        list.PushRect(
            en::Rect{kHarmX + 1.0f, kHarmY + 1.0f, harmFillW, 2.0f},
            Color{0.78f, 0.98f, 1.0f, 0.95f},
            0U, 27U, 32768U, kHudView);
    }
    // 50 Harmony Unison threshold tick
    list.PushRect(
        en::Rect{kHarmX + kHarmW * 0.5f, kHarmY + 1.0f, 1.5f, kHarmH - 2.0f},
        Color{0.95f, 0.88f, 0.45f, 0.90f},
        0U, 28U, 32768U, kHudView);
    list.PushRectOutline(
        en::Rect{kHarmX, kHarmY, kHarmW, kHarmH},
        1.0f, Color{0.48f, 0.76f, 0.92f, 0.95f},
        0U, 28U, 32768U, kHudView);

    // 3. Compact 3-Slot Triad Gem Indicators + Visual Air-Dash Pips (zero noisy Echo name strings)
    constexpr f32 kGemRowY = kHarmY + 13.0f;
    for (u32 s = 0U; s < kTriadDeckSize; ++s) {
        const EchoIdentityDef& edef = GetEchoDef(ps->deck.slots[s]);
        const bool isActive = (ps->deck.activeSlot == s);
        const f32 gx = kHpX + static_cast<f32>(s) * 26.0f;

        list.PushRect(
            en::Rect{gx, kGemRowY, 20.0f, 16.0f},
            isActive ? Color{0.18f, 0.21f, 0.29f, 0.98f} : Color{0.08f, 0.09f, 0.13f, 0.88f},
            0U, 25U, 32768U, kHudView);
        list.PushRect(
            en::Rect{gx + 4.0f, kGemRowY + 3.0f, 12.0f, 10.0f},
            edef.primaryColor,
            0U, 26U, 32768U, kHudView);
        list.PushRectOutline(
            en::Rect{gx, kGemRowY, 20.0f, 16.0f},
            isActive ? 2.0f : 1.0f,
            isActive ? Color{1.0f, 0.88f, 0.38f, 1.0f} : Color{0.35f, 0.39f, 0.48f, 0.85f},
            0U, 27U, 32768U, kHudView);
    }

    // Visual Air-Dash Wing Pips (next to Triad gems)
    const u8 maxDash = ps->MaxDashCharges();
    for (u8 d = 0U; d < maxDash; ++d) {
        const bool ready = (d < ps->airDashCharges);
        const f32 dx = kHpX + 86.0f + static_cast<f32>(d) * 16.0f;
        list.PushRect(
            en::Rect{dx, kGemRowY + 4.0f, 12.0f, 8.0f},
            ready ? Color{0.42f, 0.94f, 1.0f, 0.98f} : Color{0.14f, 0.18f, 0.24f, 0.85f},
            0U, 26U, 32768U, kHudView);
        list.PushRectOutline(
            en::Rect{dx, kGemRowY + 4.0f, 12.0f, 8.0f},
            1.0f, Color{0.68f, 0.88f, 0.98f, 0.90f},
            0U, 27U, 32768U, kHudView);
    }

    // Compact Tuning Shards counter & Husk icon badge
    list.PushRect(
        en::Rect{kHpX + 130.0f, kGemRowY + 3.0f, 8.0f, 10.0f},
        mv.ActiveHusk().IsActive() ? Color{1.0f, 0.78f, 0.25f, 1.0f} : Color{0.78f, 0.86f, 0.98f, 0.95f},
        0U, 26U, 32768U, kHudView);
    char shardBuf[24]{};
    std::snprintf(shardBuf, sizeof(shardBuf), "%u", ps->carriedTuningShards);
    list.PushText(
        Vec2{kHpX + 144.0f, kGemRowY + 4.0f},
        shardBuf,
        mv.ActiveHusk().IsActive() ? Color{1.0f, 0.84f, 0.36f, 1.0f} : Color{0.90f, 0.94f, 1.0f, 1.0f},
        0.75f, 0U, 30U, kHudView);
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
    list.PushRect(en::Rect{140.0f, 70.0f, 1000.0f, 580.0f}, Color{0.05f, 0.07f, 0.12f, 0.95f}, 0U, 28U, 32768U, kHudView);
    list.PushRectOutline(en::Rect{140.0f, 70.0f, 1000.0f, 580.0f}, 2.0f, Color{0.45f, 0.82f, 0.95f, 1.0f}, 0U, 29U, 32768U, kHudView);
    list.PushText(Vec2{180.0f, 96.0f}, "VAELEN MACRO-TOPOLOGY & LORE JOURNAL [TAB / SELECT TO CLOSE]", Color{0.75f, 0.95f, 1.0f, 1.0f}, 0.92f, 0U, 30U, kHudView);

    for (u8 r = 0U; r < kRoomCount; ++r) {
        const RoomTopologySpec& spec = GetRoomTopology(r);
        const RegionSheetDef& reg = GetRegionSheet(spec.region);
        const bool visited = mv.IsRoomDiscovered(r);
        const bool current = (r == activeRoomId);
        const f32 rx = 180.0f + static_cast<f32>(r % 4U) * 235.0f;
        const f32 ry = 148.0f + static_cast<f32>(r / 4U) * 125.0f;
        const f32 nodeW = 150.0f + static_cast<f32>(spec.tileWidth) * 1.6f;
        const f32 nodeH = 68.0f + static_cast<f32>(spec.tileHeight) * 1.4f;

        list.PushRect(
            en::Rect{rx, ry, nodeW, nodeH},
            current ? Color{0.25f, 0.55f, 0.78f, 0.98f}
                    : (visited ? Color{0.14f, 0.22f, 0.32f, 0.92f} : Color{0.08f, 0.09f, 0.12f, 0.75f}),
            0U, 29U, 32768U, kHudView);
        list.PushRectOutline(
            en::Rect{rx, ry, nodeW, nodeH},
            current ? 2.5f : 1.0f,
            current ? Color{1.0f, 0.92f, 0.40f, 1.0f} : reg.skyTopColor,
            0U, 29U, 32768U, kHudView);

        char rbuf[72]{};
        std::snprintf(rbuf, sizeof(rbuf), "[%.*s] %.*s",
                      static_cast<int>(spec.gddRoomCode.size()), spec.gddRoomCode.data(),
                      static_cast<int>(spec.hudTitle.size()), spec.hudTitle.data());
        list.PushText(Vec2{rx + 8.0f, ry + 12.0f}, rbuf, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.68f, 0U, 30U, kHudView);
    }

    for (u8 f = 0U; f < kMemoryTuningFlagCount; ++f) {
        const MemoryTuningSpec& fspec = kMemoryTuningSpecTable[f];
        char fbuf[160]{};
        std::snprintf(fbuf, sizeof(fbuf), "%.*s: %.*s",
                      static_cast<int>(fspec.title.size()), fspec.title.data(),
                      static_cast<int>(fspec.journalSummary.size()), fspec.journalSummary.data());
        list.PushText(
            Vec2{180.0f, 440.0f + static_cast<f32>(f) * 32.0f},
            fbuf, Color{0.75f, 0.88f, 0.98f, 1.0f}, 0.70f, 0U, 30U, kHudView);
    }
}

void HudAndMenuSystem::EmitPauseModal(en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;
    list.PushRect(en::Rect{260.0f, 140.0f, 760.0f, 440.0f}, Color{0.06f, 0.07f, 0.11f, 0.95f}, 0U, 28U, 32768U, kHudView);
    list.PushRectOutline(en::Rect{260.0f, 140.0f, 760.0f, 440.0f}, 2.0f, Color{0.92f, 0.78f, 0.42f, 1.0f}, 0U, 29U, 32768U, kHudView);

    list.PushText(Vec2{300.0f, 180.0f}, "PAUSED - ACCESSIBILITY, AUDIO & CONTROLS", Color{1.0f, 0.94f, 0.75f, 1.0f}, 1.0f, 0U, 30U, kHudView);
    list.PushText(Vec2{300.0f, 240.0f}, "MOVE: A/D | JUMP: SPACE | LIGHT: J | HEAVY: K | DASH: L/SHIFT | PARRY: F", Color{0.85f, 0.90f, 0.98f, 1.0f}, 0.78f, 0U, 30U, kHudView);
    list.PushText(Vec2{300.0f, 280.0f}, "UNISON: U/R (50 HARM) | SACRED CHORD: O/T (100 HARM) | ECHO: 1/2/3/Q/C", Color{0.85f, 0.90f, 0.98f, 1.0f}, 0.78f, 0U, 30U, kHudView);
    list.PushText(Vec2{300.0f, 320.0f}, "INTERACT: E | JOURNAL/MAP: TAB | DEBUG HUD (TOP-RIGHT): F1..F4", Color{0.85f, 0.90f, 0.98f, 1.0f}, 0.78f, 0U, 30U, kHudView);
    list.PushText(Vec2{300.0f, 380.0f}, "PRESS ESC / START TO RESUME", Color{1.0f, 0.90f, 0.55f, 1.0f}, 0.88f, 0U, 30U, kHudView);
}

void HudAndMenuSystem::EmitEndingModal(
    EndingId ending,
    en::RenderList& list) noexcept {
    constexpr u8 kHudView = en::RenderList::kHudViewSlot;
    list.PushRect(en::Rect{260.0f, 140.0f, 760.0f, 440.0f}, Color{0.06f, 0.07f, 0.11f, 0.95f}, 0U, 28U, 32768U, kHudView);
    list.PushRectOutline(en::Rect{260.0f, 140.0f, 760.0f, 440.0f}, 2.0f, Color{0.92f, 0.78f, 0.42f, 1.0f}, 0U, 29U, 32768U, kHudView);

    list.PushText(Vec2{300.0f, 175.0f}, "SPIRE OF UNISON - ENDING RESOLUTION", Color{1.0f, 0.92f, 0.65f, 1.0f}, 1.05f, 0U, 30U, kHudView);
    const char* endTitle = "ENDING A: THE FALSE DAWN";
    if (ending == EndingId::ShatteredSpire) {
        endTitle = "ENDING B: THE SHATTERED SPIRE";
    } else if (ending == EndingId::SymphonyOfOffBeat) {
        endTitle = "ENDING C: SYMPHONY OF THE OFF-BEAT (TRUE ENDING)";
    }
    list.PushText(Vec2{300.0f, 235.0f}, endTitle, Color{0.55f, 0.95f, 1.0f, 1.0f}, 0.95f, 0U, 30U, kHudView);
}

} // namespace vots
