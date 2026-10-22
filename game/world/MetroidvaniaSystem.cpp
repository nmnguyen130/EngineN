#include "game/world/MetroidvaniaSystem.h"
#include "game/player/PlayerController.h"

#include <algorithm>
#include <cmath>

namespace vots {

void MetroidvaniaSystem::ActivateTuningResonator(
    EntityId playerId,
    u8 roomId,
    Vec2 resonatorPos,
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    m_checkpointX = resonatorPos.x;
    m_checkpointY = resonatorPos.y;
    m_checkpointRoomId = roomId;
    DiscoverRoom(roomId);

    const u16 flagId = static_cast<u16>(WorldFlagId::ResonatorAshSumpActive) + std::min<u8>(roomId, 3U);
    pipeline.SetWorldFlag(flagId, true);

    if (!world.IsAlive(playerId)) return;
    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (ps != nullptr) {
        ps->harmony = kMaxHarmonyGauge; // GDD v0.2 Section 5.1: sets Harmony to 100
        ps->lastResonatorRoomId = roomId;
        ps->healFlashTicks = 30U;
    }
    if (recv != nullptr) {
        const i32 baseMax = (ps != nullptr) ? ps->baseMaxHp : kPlayerBaseMaxHp;
        const bool hasPenalty = (ps != nullptr && ps->huskPenaltyActive != 0U) || m_husk.IsActive();
        const i32 effectiveMax = hasPenalty
                                     ? static_cast<i32>(static_cast<f32>(baseMax) * kHuskMaxHpPenaltyMultiplier + 0.5f)
                                     : baseMax;
        recv->maxHp = effectiveMax;
        recv->hp = effectiveMax;
        recv->RemoveFlags(en::ReceiverFlags::Dead);
    }

    en::SimCue humCue{};
    humCue.kind = en::SimCueKind::AudioOneShot;
    humCue.assetId = en::AssetId::Hash("sfx_resonator_hum");
    humCue.worldX = resonatorPos.x;
    humCue.worldY = resonatorPos.y;
    humCue.emitter = playerId;
    pipeline.EmitCue(humCue);
}

bool MetroidvaniaSystem::ConfigureTriadDeckAtResonator(
    EntityId playerId,
    EchoId slot0,
    EchoId slot1,
    EchoId slot2,
    en::World& world) noexcept {
    if (!world.IsAlive(playerId)) return false;
    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    if (ps == nullptr) return false;
    if (!ps->deck.ConfigureAtResonator(slot0, slot1, slot2, ps->unlockedEchoMask)) {
        return false;
    }
    PlayerController::SwitchActiveEchoSlot(playerId, ps->deck.activeSlot, world);
    return true;
}

void MetroidvaniaSystem::HandlePlayerDeathAndSpawnHusk(
    EntityId playerId,
    Vec2 deathPos,
    u8 deathRoomId,
    en::World& world) noexcept {
    if (!world.IsAlive(playerId)) return;
    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (ps == nullptr || recv == nullptr) return;

    // GDD v0.2 Section 5.2: Only one Lost Wick Husk can exist at a time.
    // If Kaelen dies again before recovering the Husk, the Husk stays at its original position
    // and newly carried Tuning Shards are added to the same Husk pool.
    if (!m_husk.IsActive()) {
        m_husk.worldX = deathPos.x;
        m_husk.worldY = deathPos.y;
        m_husk.roomId = deathRoomId;
        m_husk.bankedTuningShards = ps->carriedTuningShards;
        m_husk.active = 1U;
    } else {
        m_husk.bankedTuningShards += ps->carriedTuningShards;
    }

    ps->carriedTuningShards = 0U;
    ps->harmony = 0U; // Harmony resets to 0 on death
    ps->huskPenaltyActive = 1U;
    ps->activeRoomId = m_checkpointRoomId;

    // Non-stacking 25% Max HP penalty
    const i32 penalizedMaxHp = static_cast<i32>(
        static_cast<f32>(ps->baseMaxHp) * kHuskMaxHpPenaltyMultiplier + 0.5f);
    recv->maxHp = penalizedMaxHp;
    recv->hp = penalizedMaxHp;
    recv->RemoveFlags(en::ReceiverFlags::Dead);
    recv->iframeTicks = 0U;

    en::LocalTransform2D respawnXf = world.GetLocalTransform(playerId);
    respawnXf.x = m_checkpointX;
    respawnXf.y = m_checkpointY;
    world.Teleport(playerId, respawnXf);
}

bool MetroidvaniaSystem::RecoverLostWickHusk(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    if (!m_husk.IsActive() || !world.IsAlive(playerId)) return false;

    auto* ps = world.TryGet<PlayerStateComp>(playerId);
    auto* recv = world.TryGet<en::DamageReceiver2D>(playerId);
    if (ps == nullptr || recv == nullptr) return false;

    ps->carriedTuningShards += m_husk.bankedTuningShards;
    ps->huskPenaltyActive = 0U;
    ps->harmony = kMaxHarmonyGauge; // Restores Harmony to 100

    // Restore Max HP cap; clamp current HP to restored Max HP without overhealing (GDD v0.2 Section 5.2).
    recv->maxHp = ps->baseMaxHp;
    recv->hp = std::min(recv->hp, recv->maxHp);

    const Vec2 huskPos{m_husk.worldX, m_husk.worldY};
    m_husk = LostWickHuskRecord{};

    en::SimCue cue{};
    cue.kind = en::SimCueKind::AudioOneShot;
    cue.assetId = en::AssetId::Hash("sfx_husk_recover");
    cue.worldX = huskPos.x;
    cue.worldY = huskPos.y;
    cue.emitter = playerId;
    pipeline.EmitCue(cue);
    return true;
}

void MetroidvaniaSystem::UnlockEchoAtAltar(
    EntityId playerId,
    EchoId echo,
    en::World& world) noexcept {
    if (!IsValidEcho(echo) || !world.IsAlive(playerId)) return;
    if (auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
        ps->unlockedEchoMask |= static_cast<u16>(1U << static_cast<u8>(echo));
    }
}

void MetroidvaniaSystem::CompleteMemoryTuningFork(
    u8 forkIndex,
    en::GameplayPipeline& pipeline) noexcept {
    if (forkIndex == 0U) pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningQuest01Done), true);
    else if (forkIndex == 1U) pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningQuest02Done), true);
    else if (forkIndex == 2U) pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningQuest03Done), true);

    if (pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningQuest01Done)) &&
        pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningQuest02Done)) &&
        pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningQuest03Done))) {
        pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningComplete), true);
    }
}

bool MetroidvaniaSystem::PlaceMapPin(
    u8 roomId,
    Vec2 worldPos,
    MapPinKind kind,
    u16 noteTag) noexcept {
    if (kind == MapPinKind::None) return false;
    for (u8 i = 0U; i < m_pinCount; ++i) {
        if (m_mapPins[i].roomId == roomId) {
            const f32 dx = m_mapPins[i].worldX - worldPos.x;
            const f32 dy = m_mapPins[i].worldY - worldPos.y;
            if (dx * dx + dy * dy <= 1.0f) {
                m_mapPins[i].kind = kind;
                m_mapPins[i].noteTag = noteTag;
                return true;
            }
        }
    }
    if (m_pinCount >= kMaxMapPins) return false;
    m_mapPins[m_pinCount++] = MapPinRecord{worldPos.x, worldPos.y, roomId, kind, noteTag};
    return true;
}

bool MetroidvaniaSystem::RemoveNearestMapPin(
    u8 roomId,
    Vec2 worldPos,
    f32 maxRadius) noexcept {
    const f32 rSq = maxRadius * maxRadius;
    u8 bestIdx = 0xFFU;
    f32 bestDistSq = rSq;

    for (u8 i = 0U; i < m_pinCount; ++i) {
        if (m_mapPins[i].roomId != roomId) continue;
        const f32 dx = m_mapPins[i].worldX - worldPos.x;
        const f32 dy = m_mapPins[i].worldY - worldPos.y;
        const f32 dSq = dx * dx + dy * dy;
        if (dSq <= bestDistSq) {
            bestDistSq = dSq;
            bestIdx = i;
        }
    }
    if (bestIdx == 0xFFU) return false;
    for (u8 i = bestIdx; i + 1U < m_pinCount; ++i) {
        m_mapPins[i] = m_mapPins[i + 1U];
    }
    m_mapPins[--m_pinCount] = MapPinRecord{};
    return true;
}

FrequencyGateDiagnostic MetroidvaniaSystem::EvaluateFrequencyGate(
    const InteractableComp& gate,
    const en::GameplayPipeline& pipeline) noexcept {
    FrequencyGateDiagnostic diag{};
    diag.isMainRoute = (gate.isMainRouteGate != 0U);
    diag.isUnlocked = (gate.requiredWorldFlag == 0U) || pipeline.TestWorldFlag(gate.requiredWorldFlag);

    if (gate.requiredWorldFlag == static_cast<u16>(WorldFlagId::UnlockedGrappleHook)) {
        diag.observableBlocker = "High Acoustic Chasm & Tether Anchor";
        diag.requiredInteraction = "Permanent World Grapple Upgrade";
    } else if (gate.requiredWorldFlag == static_cast<u16>(WorldFlagId::UnlockedPhaseGateKey)) {
        diag.observableBlocker = "Harmonic Seal Lattice";
        diag.requiredInteraction = "Sanctum Resonance Key / Bell Switch";
    } else {
        diag.observableBlocker = "Locked Acoustic Bulkhead";
        diag.requiredInteraction = "World Progression Switch / Flag";
    }
    return diag;
}

EndingId MetroidvaniaSystem::ResolveSpireEnding(
    EntityId playerId,
    bool chooseShatterCore,
    bool chooseSymphonyOffBeat,
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    if (!pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated))) {
        return EndingId::None;
    }

    auto* ps = world.IsAlive(playerId) ? world.TryGet<PlayerStateComp>(playerId) : nullptr;
    const bool hasAll12Echoes = (ps != nullptr && (ps->unlockedEchoMask & 0x0FFFU) == 0x0FFFU);
    const bool memoryTuningDone = pipeline.TestWorldFlag(static_cast<u16>(WorldFlagId::MemoryTuningComplete));

    if (chooseSymphonyOffBeat && hasAll12Echoes && memoryTuningDone) {
        // Ending C: Symphony of the Off-Beat — Kaelen transforms the Spire and relinquishes the Lantern as a weapon.
        if (ps != nullptr) {
            ps->lanternRelinquished = 1U;
        }
        m_achievedEnding = EndingId::SymphonyOfOffBeat;
        return m_achievedEnding;
    }

    if (chooseShatterCore) {
        // Ending B: The Shattered Spire — destroys Spire core and severs Tuning Collars.
        pipeline.SetWorldFlag(static_cast<u16>(WorldFlagId::SpireCoreShatteredChoice), true);
        m_achievedEnding = EndingId::ShatteredSpire;
        return m_achievedEnding;
    }

    // Ending A: The False Dawn — Kaelen reaches the surface without dismantling the harvesting network.
    m_achievedEnding = EndingId::FalseDawn;
    return m_achievedEnding;
}

SaveData MetroidvaniaSystem::ExportSaveData(
    EntityId playerId,
    const en::World& world,
    const en::GameplayPipeline& pipeline) const noexcept {
    SaveData out{};
    out.checkpointX = m_checkpointX;
    out.checkpointY = m_checkpointY;
    out.checkpointRoomId = m_checkpointRoomId;
    out.discoveredRoomsMask = m_discoveredRoomsMask;
    out.equippedSigilMask = m_equippedSigilMask;
    out.achievedEnding = static_cast<u8>(m_achievedEnding);
    out.pinCount = m_pinCount;
    out.husk = m_husk;
    out.mapPins = m_mapPins;

    u16 flags0 = 0U;
    for (u16 b = 0U; b < 16U; ++b) {
        if (pipeline.TestWorldFlag(b)) {
            flags0 |= static_cast<u16>(1U << b);
        }
    }
    out.worldFlagsWord0 = flags0;

    if (world.IsAlive(playerId)) {
        if (const auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
            out.unlockedEchoMask = ps->unlockedEchoMask;
            out.carriedTuningShards = ps->carriedTuningShards;
            out. playerBaseMaxHp = ps->baseMaxHp;
            out.triadDeck = ps->deck;
            out.harmonyGauge = ps->harmony;
        }
        if (const auto* recv = world.TryGet<en::DamageReceiver2D>(playerId)) {
            out.playerCurrentHp = recv->hp;
        }
    }
    out.checksum = out.ComputePayloadChecksum();
    return out;
}

void MetroidvaniaSystem::ImportSaveData(
    const SaveData& data,
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline) noexcept {
    m_checkpointX = data.checkpointX;
    m_checkpointY = data.checkpointY;
    m_checkpointRoomId = data.checkpointRoomId;
    m_discoveredRoomsMask = data.discoveredRoomsMask;
    m_equippedSigilMask = data.equippedSigilMask;
    m_achievedEnding = static_cast<EndingId>(data.achievedEnding);
    m_pinCount = std::min<u8>(data.pinCount, static_cast<u8>(kMaxMapPins));
    m_husk = data.husk;
    m_mapPins = data.mapPins;

    for (u16 b = 0U; b < 16U; ++b) {
        pipeline.SetWorldFlag(b, (data.worldFlagsWord0 & (1U << b)) != 0U);
    }

    if (world.IsAlive(playerId)) {
        if (auto* ps = world.TryGet<PlayerStateComp>(playerId)) {
            ps->unlockedEchoMask = data.unlockedEchoMask;
            ps->carriedTuningShards = data.carriedTuningShards;
            ps->baseMaxHp = data.playerBaseMaxHp;
            ps->deck = data.triadDeck;
            ps->harmony = std::min<u8>(data.harmonyGauge, kMaxHarmonyGauge);
            ps->huskPenaltyActive = m_husk.IsActive() ? 1U : 0U;
            ps->lastResonatorRoomId = m_checkpointRoomId;
            PlayerController::SwitchActiveEchoSlot(playerId, ps->deck.activeSlot, world);
        }
        if (auto* recv = world.TryGet<en::DamageReceiver2D>(playerId)) {
            const i32 effectiveMax = m_husk.IsActive()
                                         ? static_cast<i32>(static_cast<f32>(data.playerBaseMaxHp) * kHuskMaxHpPenaltyMultiplier + 0.5f)
                                         : data.playerBaseMaxHp;
            recv->maxHp = effectiveMax;
            recv->hp = std::clamp(data.playerCurrentHp, 1, effectiveMax);
        }
    }
}

} // namespace vots
