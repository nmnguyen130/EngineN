#include "audio/AudioMixer.h"
#include "audio/AudioSystem.h"
#include "audio/AudioTypes.h"

#include "assets/AssetBank.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, int line) noexcept {
    if (!cond) {
        std::printf("[FAIL] Line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define EN_CHECK(expr) Check((expr), #expr, __LINE__)

void FillSineWavePcm(std::span<en::i16> pcm, en::f32 freqHz) noexcept {
    constexpr en::f32 kTwoPi = 6.283185307179586f;
    for (en::usize i = 0; i < pcm.size(); ++i) {
        const en::f32 phase = kTwoPi * freqHz * static_cast<en::f32>(i) / static_cast<en::f32>(en::audio::kSampleRate);
        pcm[i]              = static_cast<en::i16>(std::sin(phase) * 24000.0f);
    }
}

void TestCacheLineSizesAndPanLaw() noexcept {
    using namespace en;
    using namespace en::audio;

    EN_CHECK(sizeof(VoiceHandle) == 4);
    EN_CHECK(sizeof(SoundCueId) == 4);
    EN_CHECK(sizeof(AudioTriggerReq) == 16);
    EN_CHECK(sizeof(AudioEmitterView) == 16);
    EN_CHECK(sizeof(SoundCueDef) == 32);
    EN_CHECK(sizeof(MusicTrackDef) == 32);
    EN_CHECK(sizeof(CookedAudioBankHeader) == 16);
    EN_CHECK(sizeof(AudioCommand) == 32);
    EN_CHECK(sizeof(AudioFrameSnapshot) == 576);
    EN_CHECK(sizeof(AudioTelemetrySnapshot) == 32);
    EN_CHECK(sizeof(VoicePoolSoA) == 3648);
    EN_CHECK(sizeof(MusicDeckRuntime) == 64);
    EN_CHECK(sizeof(BusDspState) == 192);
    EN_CHECK(sizeof(MixerArenaState) == 20352);
    EN_CHECK(sizeof(AudioMixer) == 64);
    EN_CHECK(sizeof(AudioSystem) == 128);

    f32 l = 0.0f, r = 0.0f;
    EvalConstantPowerPan(0.0f, l, r);
    EN_CHECK(std::abs(l - 0.70710678f) < 1.0e-5f);
    EN_CHECK(std::abs(r - 0.70710678f) < 1.0e-5f);

    EvalConstantPowerPan(-1.0f, l, r);
    EN_CHECK(std::abs(l - 1.0f) < 1.0e-6f);
    EN_CHECK(std::abs(r - 0.0f) < 1.0e-6f);

    EvalConstantPowerPan(1.0f, l, r);
    EN_CHECK(std::abs(l - 0.0f) < 1.0e-6f);
    EN_CHECK(std::abs(r - 1.0f) < 1.0e-6f);
}

void TestLayer1AudioMixerDeterminism() noexcept {
    using namespace en;
    using namespace en::audio;

    alignas(64) std::array<u8, AudioMixer::RequiredArenaBytes()> arenaA{};
    alignas(64) std::array<u8, AudioMixer::RequiredArenaBytes()> arenaB{};
    AudioMixer mixerA{};
    AudioMixer mixerB{};
    EN_CHECK(mixerA.Init(arenaA).has_value());
    EN_CHECK(mixerB.Init(arenaB).has_value());

    std::array<i16, 2048> sinePcm{};
    FillSineWavePcm(sinePcm, 440.0f);

    AudioCommand spawn{};
    spawn.type                         = AudioCmdType::SpawnVoice;
    spawn.packedBusAndChannel          = static_cast<u8>(AudioBusId::SfxPlayer);
    spawn.voiceSlot                    = 0;
    spawn.generation                   = 1;
    spawn.flags                        = static_cast<u8>(VoiceFlags::Looping | VoiceFlags::FollowTimeScale);
    spawn.assetScopeId                 = 3;
    spawn.payload.spawn.pcm            = sinePcm.data();
    spawn.payload.spawn.frameCount     = static_cast<u32>(sinePcm.size());
    spawn.payload.spawn.loopStartFrame = 128;
    spawn.payload.spawn.sampleRate     = 48000;
    spawn.payload.spawn.gainL_Q14      = 12000;
    spawn.payload.spawn.gainR_Q14      = 12000;
    spawn.payload.spawn.pitchRatioQ12  = 4096;

    mixerA.ApplyCommand(spawn);
    mixerB.ApplyCommand(spawn);

    AudioFrameSnapshot snap{};
    snap.busTargetGainQ14.fill(16384);
    snap.channelTimeScaleQ12.fill(4096);
    snap.targetGainL_Q14[0] = 12000;
    snap.targetGainR_Q14[0] = 12000;
    snap.targetPitchQ12[0]  = 4096;
    snap.snapshotSeq        = 1;

    std::array<f32, kMixBlockFrames * 2> outA{};
    std::array<f32, kMixBlockFrames * 2> outB{};

    for (int i = 0; i < 12; ++i) {
        mixerA.MixBlock(snap, outA);
        mixerB.MixBlock(snap, outB);
    }

    EN_CHECK(mixerA.ComputeAuditHash() == mixerB.ComputeAuditHash());
    EN_CHECK(std::memcmp(outA.data(), outB.data(), sizeof(outA)) == 0);
    EN_CHECK((mixerA.Telemetry().activeVoiceMask & 1ULL) != 0ULL);
    EN_CHECK((mixerA.Telemetry().activeScopePinMask & (1u << 3u)) != 0u);
    EN_CHECK(mixerA.Telemetry().masterPeakL > 0.05f);

    // Verify direct steal tail accumulation when spawning onto an already-active voice slot.
    spawn.generation = 2;
    mixerA.ApplyCommand(spawn);
    mixerA.MixBlock(snap, outA);
    EN_CHECK(mixerA.Telemetry().stolenVoicesTotal == 1u);
}

void TestLayer2AudioSystemAndCoalescing() noexcept {
    using namespace en;
    using namespace en::audio;

    assets::AssetBank bank{};
    EN_CHECK(bank.Init({.maxSlots = 16, .maxAudio = 16, .blobArenaBytes = 16384}).has_value());

    std::array<i16, 1024> pcm0{};
    std::array<i16, 2048> stemPcm{};
    FillSineWavePcm(pcm0, 520.0f);
    FillSineWavePcm(stemPcm, 220.0f);

    auto registerAudio = [&](std::string_view name, std::span<const i16> pcm, u16 sampleRate, u8 channels) -> assets::AudioHandle {
        bool needsLoad = false;
        auto slotRes = bank.AcquireSlot(assets::AssetId::Hash(name), assets::AssetKind::Audio, 0, assets::Residency::Sim, needsLoad);
        Check(slotRes.has_value(), "AcquireSlot audio", __LINE__);
        const std::span<const u8> pcmBytes{reinterpret_cast<const u8*>(pcm.data()), pcm.size_bytes()};
        auto commitRes = bank.CommitAudio(*slotRes, pcmBytes, sampleRate, channels, false);
        Check(commitRes.has_value(), "CommitAudio", __LINE__);
        return bank.ToAudioHandle(*slotRes);
    };

    const auto handle1 = registerAudio("audio/sine520", pcm0, 48000, 1);
    const auto handle2 = registerAudio("audio/stem220", stemPcm, 48000, 1);

    AudioSystem sys{};
    AudioSystemConfig cfg{};
    cfg.headlessMode = true;
    EN_CHECK(sys.Init(cfg).has_value());

    SoundCueDef cue{};
    cue.cueId                 = SoundCueId{1001};
    cue.variations[0]         = handle1;
    cue.baseGainQ14           = 12000;
    cue.gainVarQ8             = 0;
    cue.pitchVarSemitonesQ4   = 0;
    cue.packedRouting         = static_cast<u8>(1u | (static_cast<u8>(AudioBusId::SfxCombat) << 3u) |
                                        (static_cast<u8>(StealPolicy::StealQuietest) << 6u));
    cue.packedChannelAndCurve = static_cast<u8>(static_cast<u8>(time::TimeChannel::Player) |
                                                (static_cast<u8>(AttenuationCurve::Linear) << 3u) |
                                                (1u << 5u));
    cue.priority              = 128;
    cue.packedFlagsAndCooldown =
        static_cast<u8>(static_cast<u8>(VoiceFlags::Spatialized2D | VoiceFlags::PauseOnHitstop) | (1u << 6u));

    EN_CHECK(sys.RegisterCueDef(cue, 5).has_value());

    // Verify tick 0 energy-summing coalescing returns the exact same VoiceHandle instead of spawning a second voice.
    const VoiceHandle h1 = sys.PlayCue(SoundCueId{1001}, bank, 2.0f, 0.0f, 42u);
    const VoiceHandle h2 = sys.PlayCue(SoundCueId{1001}, bank, 2.0f, 0.0f, 43u);
    EN_CHECK(h1.IsValid());
    EN_CHECK(h1 == h2);

    std::array<f32, kMixBlockFrames * 2> capture{};
    sys.PumpHeadlessBlocks(1, capture);

    const auto tel = sys.Telemetry();
    EN_CHECK(tel.activeVoiceMask == 1ULL);
    EN_CHECK((tel.activeScopePinMask & (1u << 5u)) != 0u);
    EN_CHECK(tel.masterPeakL > 0.01f);

    // Scope 5 is currently pinned by the active voice; PollReleasableScopeMask must return 0.
    sys.RequestScopeRelease(5);
    EN_CHECK(sys.PollReleasableScopeMask() == 0u);

    std::array<AudioEmitterView, 8> emitters{};
    EN_CHECK(sys.ExtractActiveEmitters(emitters) == 1u);
    EN_CHECK(std::abs(emitters[0].worldX - 2.0f) < 1.0e-5f);

    // Stop voice immediately and pump 1 block; Scope 5 must now become releasable.
    sys.StopVoice(h1, 0);
    sys.PumpHeadlessBlocks(1, capture);
    EN_CHECK(sys.PollReleasableScopeMask() == (1u << 5u));

    // Verify interactive music deck transition & scope pinning.
    MusicTrackDef track{};
    track.trackId             = 77;
    track.stems[0]            = handle2;
    track.loopStartFrame      = 0;
    track.loopEndFrame        = static_cast<u32>(stemPcm.size());
    track.packedStemsAndBeats = static_cast<u8>(1u | (4u << 3u));
    EN_CHECK(sys.RegisterMusicTrackDef(track, 9).has_value());

    sys.TransitionMusic(77, bank, 0);
    sys.PumpHeadlessBlocks(2, capture);
    EN_CHECK((sys.Telemetry().activeScopePinMask & (1u << 9u)) != 0u);

    // Verify consolidated Update() with triggers, bus filters, and occlusion callback.
    sys.SetBusFilter(AudioBusId::SfxCombat, FilterMode::LowPass, 14000.0f);
    bool occlusionCalled = false;
    const std::array<f32, 8> scales{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<AudioTriggerReq, 1> reqs{AudioTriggerReq{SoundCueId{1001}, 6.0f, 0.0f, 99u}};

    AudioUpdateDesc upd{};
    upd.tickIndex        = 8;
    upd.listenerX        = 0.0f;
    upd.listenerY        = 0.0f;
    upd.channelScales    = scales;
    upd.triggers         = reqs;
    upd.assetBank        = &bank;
    upd.occlusionUserCtx = &occlusionCalled;
    upd.occlusionQuery   = [](const void* ctx, f32, f32, f32, f32, f32, u32) noexcept -> bool {
        *const_cast<bool*>(static_cast<const bool*>(ctx)) = true;
        return true;
    };

    sys.Update(upd);
    EN_CHECK(occlusionCalled);
    sys.PumpHeadlessBlocks(1, capture);
    EN_CHECK(sys.Telemetry().masterPeakL > 0.0f);

    // Verify move semantics.
    AudioSystem movedSys = std::move(sys);
    EN_CHECK(movedSys.ComputeAuditHash() != 0ULL);
    movedSys.Shutdown();
}

} // namespace

int main() {
    TestCacheLineSizesAndPanLaw();
    TestLayer1AudioMixerDeterminism();
    TestLayer2AudioSystemAndCoalescing();

    if (g_failures == 0) {
        std::printf("[PASS] All Phase 8 Audio unit tests passed.\n");
        return 0;
    }
    std::printf("[FAIL] %d test assertion(s) failed.\n", g_failures);
    return 1;
}
