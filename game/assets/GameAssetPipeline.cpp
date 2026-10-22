#include "game/assets/GameAssetPipeline.h"
#include "game/core/GameConfig.h"
#include "gameplay/GameplayTypes.h"

#include <cmath>
#include <cstring>

namespace vots {

namespace {

struct CookedRigPayload {
    en::CookedGameplayRigHeader hdr{};
    std::array<en::CookedStateEntry, static_cast<usize>(ActorFsmState::StateCount)> states{};
    std::array<en::CookedFrameDirective, 16> directives{};
};

[[nodiscard]] CookedRigPayload BuildActorRigPayload() noexcept {
    CookedRigPayload rig{};
    rig.hdr.magic = en::CookedGameplayRigHeader::kMagic;
    rig.hdr.version = en::CookedGameplayRigHeader::kVersion;
    rig.hdr.stateCount = static_cast<u16>(ActorFsmState::StateCount);
    rig.hdr.directiveCount = static_cast<u16>(rig.directives.size());

    using en::CancelCategory;
    const auto setEntry = [&](ActorFsmState st, u16 dur, u16 firstFrame, u16 frameCount,
                              u8 ticksPerFrame, u8 loop, ActorFsmState nextSt,
                              CancelCategory defCancels, CancelCategory hitCancels) noexcept {
        auto& e = rig.states[static_cast<usize>(st)];
        e.durationTicks = dur;
        e.animFirstFrame = firstFrame;
        e.animFrameCount = frameCount;
        e.animTicksPerFrame = ticksPerFrame;
        e.animLoop = loop;
        e.nextStateId = static_cast<u8>(nextSt);
        e.defaultCancels = defCancels;
        e.onHitCancels = hitCancels;
    };

    const CancelCategory kFreeCancels = CancelCategory::Any;
    const CancelCategory kChainLight = CancelCategory::Dash | CancelCategory::Parry | CancelCategory::Light | CancelCategory::Heavy | CancelCategory::Special;

    setEntry(ActorFsmState::Idle,         0U,   0U, 4U, 8U, 1U, ActorFsmState::Idle,        kFreeCancels, kFreeCancels);
    setEntry(ActorFsmState::Run,          0U,   4U, 6U, 5U, 1U, ActorFsmState::Run,         kFreeCancels, kFreeCancels);
    setEntry(ActorFsmState::JumpRise,     0U,  10U, 2U, 6U, 0U, ActorFsmState::JumpRise,    kFreeCancels, kFreeCancels);
    setEntry(ActorFsmState::Fall,         0U,  12U, 2U, 6U, 1U, ActorFsmState::Fall,        kFreeCancels, kFreeCancels);
    setEntry(ActorFsmState::Dash,        14U,  14U, 2U, 4U, 0U, ActorFsmState::Idle,        CancelCategory::Parry, CancelCategory::Any);
    setEntry(ActorFsmState::Light1,      20U,  16U, 3U, 4U, 0U, ActorFsmState::Idle,        CancelCategory::Dash | CancelCategory::Parry, kChainLight);
    setEntry(ActorFsmState::Light2,      22U,  19U, 3U, 4U, 0U, ActorFsmState::Idle,        CancelCategory::Dash | CancelCategory::Parry, kChainLight);
    setEntry(ActorFsmState::Light3,      29U,  22U, 4U, 5U, 0U, ActorFsmState::Idle,        CancelCategory::Dash, CancelCategory::Dash | CancelCategory::Special);
    setEntry(ActorFsmState::Air1,        20U,  26U, 3U, 4U, 0U, ActorFsmState::Fall,        CancelCategory::Dash, CancelCategory::Light | CancelCategory::Dash);
    setEntry(ActorFsmState::Air2,        25U,  29U, 3U, 5U, 0U, ActorFsmState::Fall,        CancelCategory::Dash, CancelCategory::Dash);
    setEntry(ActorFsmState::HeavyWindup, kHeavyAttackStartupTicks, 32U, 3U, 8U, 0U, ActorFsmState::HeavyActive, CancelCategory::Dash | CancelCategory::Parry, CancelCategory::None);
    setEntry(ActorFsmState::HeavyActive, 24U,  35U, 3U, 6U, 0U, ActorFsmState::Idle,        CancelCategory::None, CancelCategory::Dash | CancelCategory::Special);
    setEntry(ActorFsmState::ParryStance, 18U,  38U, 2U, 5U, 0U, ActorFsmState::Idle,        CancelCategory::None, CancelCategory::Any);
    setEntry(ActorFsmState::ParrySuccess,16U,  40U, 2U, 4U, 0U, ActorFsmState::Idle,        CancelCategory::Any,  CancelCategory::Any);
    setEntry(ActorFsmState::UnisonCast,  20U,  42U, 3U, 5U, 0U, ActorFsmState::Idle,        CancelCategory::None, CancelCategory::Any);
    setEntry(ActorFsmState::SacredCast,  30U,  45U, 3U, 6U, 0U, ActorFsmState::Idle,        CancelCategory::None, CancelCategory::Any);
    setEntry(ActorFsmState::HurtStagger, 45U,  48U, 2U, 8U, 1U, ActorFsmState::Idle,        CancelCategory::None, CancelCategory::None);
    setEntry(ActorFsmState::BreakExposed,150U, 50U, 2U, 10U,1U, ActorFsmState::Idle,        CancelCategory::None, CancelCategory::None);

    return rig;
}

} // namespace

en::Result<GameAssetHandles, en::FrameworkErrc> GameAssetPipeline::BuildAndCommitPersistentAssets(
    en::AssetBank& bank,
    en::AudioSystem& audio,
    u8 persistentScope) noexcept {
    GameAssetHandles out{};
    bool needsLoad = false;

    // 1. Procedural 64x64 RGBA8 character/VFX sprite sheet (8x8 uniform grid = 64 SpriteFrames)
    std::array<u8, 64U * 64U * 4U> sheetPixels{};
    for (u32 y = 0U; y < 64U; ++y) {
        for (u32 x = 0U; x < 64U; ++x) {
            const usize idx = (y * 64U + x) * 4U;
            const u32 cellX = x / 8U;
            const u32 cellY = y / 8U;
            const bool border = (x % 8U == 0U) || (y % 8U == 0U) || (x % 8U == 7U) || (y % 8U == 7U);
            sheetPixels[idx + 0U] = border ? 240U : static_cast<u8>(64U + cellX * 24U);
            sheetPixels[idx + 1U] = border ? 240U : static_cast<u8>(80U + cellY * 20U);
            sheetPixels[idx + 2U] = border ? 255U : static_cast<u8>(120U + ((cellX + cellY) * 14U));
            sheetPixels[idx + 3U] = 255U;
        }
    }

    const u16 texSlot = bank.AcquireSlot(
        en::AssetId::Hash("vots_char_sheet.tex"),
        en::AssetKind::Texture,
        persistentScope,
        en::AssetResidency::Sim,
        needsLoad);
    out.characterSheetTex = bank.CommitTexture(texSlot, 64U, 64U, sheetPixels);
    if (!out.characterSheetTex.IsValid()) {
        return en::Err(en::FrameworkErrc::AssetInitFailed);
    }

    const u16 atlasSlot = bank.AcquireSlot(
        en::AssetId::Hash("vots_char_sheet.atlas"),
        en::AssetKind::Atlas,
        persistentScope,
        en::AssetResidency::Sim,
        needsLoad);
    out.characterAtlas = bank.CommitGridAtlas(atlasSlot, out.characterSheetTex, 8U, 8U);
    if (!out.characterAtlas.IsValid()) {
        return en::Err(en::FrameworkErrc::AssetInitFailed);
    }

    // 2. Cooked "ERIG" v1 FSM blobs for Kaelen, Standard/Elite Enemies, and High Conductor Boss
    const CookedRigPayload rigPayload = BuildActorRigPayload();
    const std::span<const u8> rigBytes{reinterpret_cast<const u8*>(&rigPayload), sizeof(rigPayload)};

    const u16 playerRigSlot = bank.AcquireSlot(
        en::AssetId::Hash("kaelen.erig"), en::AssetKind::Blob, persistentScope, en::AssetResidency::Sim, needsLoad);
    out.playerRigBlob = bank.CommitBlob(playerRigSlot, rigBytes);

    const u16 enemyRigSlot = bank.AcquireSlot(
        en::AssetId::Hash("husk.erig"), en::AssetKind::Blob, persistentScope, en::AssetResidency::Sim, needsLoad);
    out.enemyRigBlob = bank.CommitBlob(enemyRigSlot, rigBytes);

    const u16 bossRigSlot = bank.AcquireSlot(
        en::AssetId::Hash("conductor_boss.erig"), en::AssetKind::Blob, persistentScope, en::AssetResidency::Sim, needsLoad);
    out.bossRigBlob = bank.CommitBlob(bossRigSlot, rigBytes);

    if (!out.playerRigBlob.IsValid() || !out.enemyRigBlob.IsValid() || !out.bossRigBlob.IsValid()) {
        return en::Err(en::FrameworkErrc::AssetInitFailed);
    }

    // 3. Cooked "ESCN" v1 Scene Manifest blobs for the 4 interconnected Vaelen rooms
    constexpr std::array<std::string_view, kRoomCount> kRoomManifestNames{{
        "room_ash_sump.escn",
        "room_resonator_sanctum.escn",
        "room_echo_vault.escn",
        "room_spire_arena.escn",
    }};

    struct RoomManifestBlob {
        en::CookedSceneManifestHeader hdr{};
        en::CookedSceneAssetEntry entries[2]{};
    };

    for (u32 r = 0U; r < kRoomCount; ++r) {
        RoomManifestBlob m{};
        m.hdr.magic = en::CookedSceneManifestHeader::kMagic;
        m.hdr.version = en::CookedSceneManifestHeader::kVersion;
        m.hdr.assetEntryCount = 2U;
        m.hdr.musicTrackId = 100U + r;
        m.entries[0].id = en::AssetId::Hash("vots_char_sheet.tex");
        m.entries[0].kind = en::AssetKind::Texture;
        m.entries[0].residency = en::AssetResidency::Sim;
        m.entries[1].id = en::AssetId::Hash("kaelen.erig");
        m.entries[1].kind = en::AssetKind::Blob;
        m.entries[1].residency = en::AssetResidency::Sim;
        m.hdr.checksum = en::ComputeSceneManifestChecksum(m.entries);

        const en::AssetId mId = en::AssetId::Hash(kRoomManifestNames[r]);
        out.roomManifestIds[r] = mId;
        const u16 slot = bank.AcquireSlot(
            mId, en::AssetKind::Blob, persistentScope, en::AssetResidency::Sim, needsLoad);
        (void)bank.CommitBlob(slot, std::span<const u8>{reinterpret_cast<const u8*>(&m), sizeof(m)});
    }

    // 4. Synthesize procedural PCM waveforms and register distinct combat/world SoundCueDefs + MusicTrackDefs
    constexpr std::array<std::string_view, 10> kCueNames{{
        "sfx_blade_slash",
        "sfx_heavy_impact",
        "sfx_parry_clang",
        "sfx_phase_counter",
        "sfx_reaction_burst",
        "sfx_break_shatter",
        "sfx_unison_pulse",
        "sfx_sacred_chord",
        "sfx_resonator_hum",
        "sfx_husk_recover",
    }};

    for (u32 i = 0U; i < kCueNames.size(); ++i) {
        std::array<f32, 512> pcm{};
        const f32 baseFreq = 180.0f + static_cast<f32>(i) * 65.0f;
        for (u32 s = 0U; s < pcm.size(); ++s) {
            const f32 t = static_cast<f32>(s) / 48000.0f;
            const f32 env = 1.0f - (static_cast<f32>(s) / static_cast<f32>(pcm.size()));
            pcm[s] = env * std::sin(2.0f * en::kPi * baseFreq * t);
        }

        const en::AssetId clipAssetId = en::AssetId::Hash(kCueNames[i]);
        const u16 aSlot = bank.AcquireSlot(
            clipAssetId, en::AssetKind::Audio, persistentScope, en::AssetResidency::Sim, needsLoad);
        const en::AudioHandle audioH = bank.CommitAudio(aSlot, pcm, 48000U, 1U, false);

        en::SoundCueDef cue{};
        cue.id = en::SoundCueId::FromAssetId(clipAssetId);
        cue.clipVariants[0] = audioH;
        cue.SetBus(i <= 7U ? en::AudioBusId::SfxCombat : en::AudioBusId::SfxWorld);
        cue.flags = en::VoiceFlags::Spatialized2D;
        (void)audio.RegisterCueDef(cue, persistentScope);
        out.soundCues[i] = cue.id;
    }

    // Register 4 ambient/combat music tracks (one per room)
    std::array<f32, 1024> musicLoop{};
    for (u32 s = 0U; s < musicLoop.size(); ++s) {
        const f32 t = static_cast<f32>(s) / static_cast<f32>(musicLoop.size());
        musicLoop[s] = 0.25f * std::sin(2.0f * en::kPi * 2.0f * t) +
                       0.15f * std::sin(2.0f * en::kPi * 3.0f * t);
    }
    const u16 musSlot = bank.AcquireSlot(
        en::AssetId::Hash("mus_vaelen_stem0.wav"),
        en::AssetKind::Audio,
        persistentScope,
        en::AssetResidency::Sim,
        needsLoad);
    const en::AudioHandle musH = bank.CommitAudio(musSlot, musicLoop, 48000U, 1U, true);

    for (u32 r = 0U; r < kRoomCount; ++r) {
        en::MusicTrackDef track{};
        track.trackId = 100U + r;
        track.stemCount = 1U;
        track.loop = 1U;
        track.stems[0] = musH;
        track.defaultWeights_q14[0] = 16384U;
        (void)audio.RegisterMusicTrackDef(track, persistentScope);
    }

    return out;
}

} // namespace vots
