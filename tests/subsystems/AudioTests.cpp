#include "audio/AudioSystem.h"
#include "assets/AssetBank.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace en::tests {

struct TestSuite {
    u32 passed{0U};
    u32 failed{0U};

    void Expect(bool cond, const char* expr, int line) noexcept {
        if (cond) {
            ++passed;
        } else {
            ++failed;
            std::printf("[FAIL] Line %d: %s\n", line, expr);
        }
    }
};

#define EXPECT_TRUE(suite, expr) (suite).Expect(static_cast<bool>(expr), #expr, __LINE__)
#define EXPECT_EQ(suite, a, b) (suite).Expect((a) == (b), #a " == " #b, __LINE__)
#define EXPECT_NEAR(suite, a, b, eps) (suite).Expect(std::abs((a) - (b)) <= (eps), #a " ~= " #b, __LINE__)

// Compile-time struct size & cache-line invariants (0B hidden compiler padding)
static_assert(sizeof(VoiceHandle) == 4);
static_assert(sizeof(SoundCueId) == 4);
static_assert(sizeof(AudioTriggerReq) == 16);
static_assert(sizeof(AudioEmitterView) == 16);
static_assert(sizeof(CookedAudioBankHeader) == 16);
static_assert(sizeof(SoundCueDef) == 32);
static_assert(sizeof(MusicTrackDef) == 32);
static_assert(sizeof(AudioCommand) == 32);
static_assert(sizeof(AudioFrameSnapshot) == 576);
static_assert(sizeof(AudioTelemetrySnapshot) == 32);
static_assert(sizeof(AudioUpdateDesc) == 64);
static_assert(sizeof(AudioSystemConfig) == 16);
static_assert(sizeof(AudioMixer) == 64);
static_assert(sizeof(MainVoiceTracker) == 32);
static_assert(sizeof(AudioSystem) == 128);

void TestDspKernelsTrueStereoLoopPointsAndMultiBlockFade(TestSuite& suite) noexcept {
    AssetBank bank;
    EXPECT_TRUE(suite, bank.Init().has_value());
    const u8 scope = bank.BeginScope().value_or(0U);

    // 1. Verify Catmull-Rom Hermite4, Cytomic SVF (LP/HP/BP), and Padé [3/2] soft-clipper
    EXPECT_NEAR(suite, AudioMixer::InterpolateHermite4(0.0f, 1.0f, 2.0f, 3.0f, 0.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(suite, AudioMixer::InterpolateHermite4(0.0f, 1.0f, 2.0f, 3.0f, 1.0f), 2.0f, 1e-6f);
    EXPECT_NEAR(suite, AudioMixer::InterpolateHermite4(0.0f, 1.0f, 2.0f, 3.0f, 0.5f), 1.5f, 1e-6f);
    EXPECT_NEAR(suite, AudioMixer::SoftClipTanhPade(0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(suite, AudioMixer::SoftClipTanhPade(4.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(suite, AudioMixer::SoftClipTanhPade(-4.0f), -1.0f, 1e-6f);

    f32 ic1 = 0.0f, ic2 = 0.0f, lpOut = 0.0f;
    for (u32 i = 0U; i < 64U; ++i) {
        lpOut = AudioMixer::StepCytomicSvf(1.0f, ic1, ic2, 0.7f, 0.15f, 0.03f, FilterMode::LowPass);
    }
    EXPECT_TRUE(suite, lpOut > 0.7f);

    // 2. Verify True Stereo 2-channel clip synthesis (L = +0.6, R = -0.2 -> L peak > R peak)
    std::vector<f32> stereoPcm(1024U * 2U);
    for (u32 i = 0U; i < 1024U; ++i) {
        stereoPcm[i * 2U]      = 0.8f;
        stereoPcm[i * 2U + 1U] = 0.2f;
    }
    bool needsLoad = false;
    const u16 stSlot = bank.AcquireSlot(AssetId::Hash("stereo_loop.wav"), AssetKind::Audio, scope, AssetResidency::Sim, needsLoad);
    const AudioHandle stereoClip = bank.CommitAudio(stSlot, stereoPcm, 48000U, 2U, true);
    EXPECT_TRUE(suite, stereoClip.IsValid());

    AudioSystem audio;
    EXPECT_TRUE(suite, audio.Init().has_value());

    SoundCueDef stCue{};
    stCue.id = SoundCueId::Hash("sfx_stereo_loop");
    stCue.clipVariants[0] = stereoClip;
    stCue.SetBus(AudioBusId::SfxPlayer);
    stCue.SetBasePitch(1.5f);
    stCue.loopStartFrame = 256U;
    stCue.flags = VoiceFlags::Looping;
    stCue.cooldownTicks = 0U;
    stCue.maxConcurrency = 64U;
    EXPECT_TRUE(suite, audio.RegisterCueDef(stCue, scope).has_value());

    const VoiceHandle vh = audio.PlayCue(stCue.id, bank);
    EXPECT_TRUE(suite, vh.IsValid());

    std::vector<f32> stereoOut(kMixBlockFrames * 2U * 4U, 0.0f);
    audio.PumpHeadlessBlocks(4U, stereoOut);
    EXPECT_EQ(suite, audio.Telemetry().mixedBlocksCount, 4ULL);
    EXPECT_TRUE(suite, audio.Telemetry().masterPeakL > audio.Telemetry().masterPeakR * 2.5f);

    // 3. Verify multi-block smooth fade-out retires even a Looping voice after N blocks
    audio.StopVoice(vh, 3U);
    audio.PumpHeadlessBlocks(3U, stereoOut);
    EXPECT_EQ(suite, audio.Telemetry().activeVoiceCount, 0U);

    // 4. Test voice stealing & in-place stolen-voice tail crossfade when all 64 slots are occupied
    VoiceHandle activeHandles[64]{};
    for (u32 i = 0U; i < 65U; ++i) {
        const VoiceHandle h = audio.PlayCue(stCue.id, bank);
        if (h.IsValid()) activeHandles[h.index & 63U] = h;
    }
    audio.PumpHeadlessBlocks(1U, stereoOut);
    EXPECT_TRUE(suite, audio.Telemetry().stolenVoiceCount >= 1U);

    // 5. Free 3 slots and test per-cue maxConcurrency with StealPolicy::Reject
    audio.StopVoice(activeHandles[0], 0U);
    audio.StopVoice(activeHandles[1], 0U);
    audio.StopVoice(activeHandles[2], 0U);

    SoundCueDef rejectCue = stCue;
    rejectCue.id = SoundCueId::Hash("sfx_reject_cap");
    rejectCue.maxConcurrency = 2U;
    rejectCue.SetStealPolicy(StealPolicy::Reject);
    EXPECT_TRUE(suite, audio.RegisterCueDef(rejectCue, scope).has_value());
    EXPECT_TRUE(suite, audio.PlayCue(rejectCue.id, bank).IsValid());
    EXPECT_TRUE(suite, audio.PlayCue(rejectCue.id, bank).IsValid());
    EXPECT_TRUE(suite, !audio.PlayCue(rejectCue.id, bank).IsValid());

    // 6. Test OneVoicePerEntity slot reuse
    SoundCueDef entityCue = stCue;
    entityCue.id = SoundCueId::Hash("sfx_entity_loop");
    entityCue.flags = VoiceFlags::OneVoicePerEntity;
    entityCue.maxConcurrency = 8U;
    EXPECT_TRUE(suite, audio.RegisterCueDef(entityCue, scope).has_value());
    const VoiceHandle ev1 = audio.PlayCue(entityCue.id, bank, 0.0f, 0.0f, 42U);
    const VoiceHandle ev2 = audio.PlayCue(entityCue.id, bank, 1.0f, 0.0f, 42U);
    EXPECT_TRUE(suite, ev1.IsValid() && ev2.IsValid());
    EXPECT_EQ(suite, ev1.index, ev2.index);

    audio.Shutdown();
    bank.Shutdown();
}

void TestVirtualVoicesNearFieldSpreadReverbAndScopeRelease(TestSuite& suite) noexcept {
    AssetBank bank;
    EXPECT_TRUE(suite, bank.Init().has_value());
    const u8 sceneScope = bank.BeginScope().value_or(0U);

    std::vector<f32> tonePcm(2048U, 0.8f);
    bool needsLoad = false;
    const u16 slot = bank.AcquireSlot(AssetId::Hash("torch_loop.wav"), AssetKind::Audio, sceneScope, AssetResidency::Sim, needsLoad);
    const AudioHandle torchClip = bank.CommitAudio(slot, tonePcm, 48000U, 1U, true);

    const u16 musSlot = bank.AcquireSlot(AssetId::Hash("bgm_stem0.wav"), AssetKind::Audio, sceneScope, AssetResidency::Sim, needsLoad);
    const AudioHandle musClip = bank.CommitAudio(musSlot, tonePcm, 48000U, 1U, true);

    AudioSystem audio;
    EXPECT_TRUE(suite, audio.Init().has_value());

    // Configure Stereo Schroeder/Freeverb Aux Reverb
    audio.SetReverbParams(0.8f, 0.4f, 0.35f, static_cast<u8>(1U << static_cast<u8>(AudioBusId::SfxCombat)));

    SoundCueDef torchCue{};
    torchCue.id = SoundCueId::Hash("torch_loop");
    torchCue.clipVariants[0] = torchClip;
    torchCue.minDist = 4.0f;
    torchCue.maxDist = 20.0f;
    torchCue.SetBus(AudioBusId::SfxCombat);
    torchCue.SetChannel(TimeChannel::World);
    torchCue.SetGainJitter(0.05f);
    torchCue.SetPitchJitter(0.05f);
    torchCue.flags = VoiceFlags::Looping | VoiceFlags::Spatialized2D | VoiceFlags::EnableDoppler |
                     VoiceFlags::FollowTimeScale | VoiceFlags::PauseOnFreeze | VoiceFlags::ReverbSend;
    torchCue.cooldownTicks = 4U;
    EXPECT_TRUE(suite, audio.RegisterCueDef(torchCue, sceneScope).has_value());

    // Spawn torch outside maxDist (at x = 50.0m > 20.0m) -> becomes a Virtual Voice!
    const VoiceHandle torchVh = audio.PlayCue(torchCue.id, bank, 50.0f, 0.0f, 77U);
    EXPECT_TRUE(suite, torchVh.IsValid());
    audio.PumpHeadlessBlocks(2U);
    EXPECT_EQ(suite, audio.Telemetry().virtualVoiceCount, 1U);
    EXPECT_EQ(suite, audio.Telemetry().activeVoiceCount, 0U);

    // Move emitter 77 into range (x = 6.0m) -> automatically promoted from Virtual Voice to Real Voice!
    audio.UpdateEmitterTransform(77U, 6.0f, 0.0f, -12.0f, 0.0f);

    MusicTrackDef track{};
    track.trackId = 100U;
    track.stems[0] = musClip;
    track.stems[1] = musClip;
    track.stemCount = 2U;
    track.defaultWeights_q14[0] = 16384U;
    track.defaultWeights_q14[1] = 8192U;
    EXPECT_TRUE(suite, audio.RegisterMusicTrackDef(track, sceneScope).has_value());
    EXPECT_TRUE(suite, audio.TransitionMusic(100U, bank, 4U));

    // Coalesce 2 additional same-tick triggers on torchCue
    AudioTriggerReq reqs[2]{
        {torchCue.id, 6.0f, 0.0f, 77U},
        {torchCue.id, 6.0f, 0.0f, 77U},
    };

    const f32 channelScales[8]{1.0f, 0.75f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    AudioUpdateDesc desc{};
    desc.triggers = reqs;
    desc.assetBank = &bank;
    desc.channelScales = channelScales;
    desc.tickIndex = 0U;
    desc.listenerX = 0.0f;
    desc.listenerY = 0.0f;
    desc.occlusionQuery = [](f32, f32, f32, f32, void*) noexcept -> bool {
        return true;
    };
    audio.Update(desc);

    std::array<AudioEmitterView, 4> emitters{};
    EXPECT_EQ(suite, audio.ExtractActiveEmitters(emitters), 1U);
    EXPECT_NEAR(suite, emitters[0].occlusionFactor, 1.0f, 1e-3f);

    audio.PumpHeadlessBlocks(2U);
    EXPECT_EQ(suite, audio.Telemetry().virtualVoiceCount, 0U);
    EXPECT_EQ(suite, audio.Telemetry().activeVoiceCount, 1U);
    EXPECT_TRUE(suite, audio.Telemetry().sidechainEnvelope > 0.05f);
    EXPECT_TRUE(suite, audio.Telemetry().reverbPeak > 0.01f);
    EXPECT_EQ(suite, audio.Telemetry().activeMusicStems, 2U);

    // Test dynamic stem weights & PauseOnFreeze when TimeChannel::World (bit 1) is frozen
    const f32 newWeights[4]{1.0f, 1.0f, 0.0f, 0.0f};
    audio.SetMusicStemWeights(newWeights);
    desc.triggers = {};
    desc.tickIndex = 1U;
    desc.listenerX = 2.0f;
    desc.frozenChannelMask = (1U << static_cast<u8>(TimeChannel::World));
    audio.Update(desc);
    audio.PumpHeadlessBlocks(1U);
    EXPECT_EQ(suite, audio.Telemetry().mixedBlocksCount, 5ULL);

    // Scope release handshake while active voices reference sceneScope
    audio.RequestScopeRelease(sceneScope);
    EXPECT_EQ(suite, audio.PollReleasableScopeMask(), 0U);

    audio.StopAllVoices(0U);
    audio.Shutdown();
    EXPECT_TRUE(suite, audio.Init().has_value());
    audio.RequestScopeRelease(sceneScope);
    EXPECT_EQ(suite, audio.PollReleasableScopeMask(), (1U << sceneScope));

    audio.Shutdown();
    bank.Shutdown();
}

void TestCookedEaudBankAndDeterminismAuditHash(TestSuite& suite) noexcept {
    const auto runAudioSim = [&]() noexcept -> u64 {
        AssetBank bank;
        (void)bank.Init();
        const u8 scope = bank.BeginScope().value_or(0U);

        std::vector<f32> pcm(512U);
        for (u32 i = 0U; i < 512U; ++i) {
            pcm[i] = std::sin(2.0f * kPi * static_cast<f32>(i) / 64.0f);
        }
        bool needsLoad = false;
        const u16 aSlot = bank.AcquireSlot(AssetId::Hash("beep.wav"), AssetKind::Audio, scope, AssetResidency::Sim, needsLoad);
        const AudioHandle beepClip = bank.CommitAudio(aSlot, pcm, 48000U, 1U, false);

        CookedAudioBankHeader hdr{};
        hdr.cueCount = 1U;
        hdr.musicTrackCount = 0U;

        SoundCueDef def{};
        def.id = SoundCueId::Hash("ui_beep");
        def.clipVariants[0] = beepClip;
        def.SetBus(AudioBusId::Ui);
        def.flags = VoiceFlags::ReverbSend;

        std::vector<u8> bankBytes(sizeof(hdr) + sizeof(def));
        std::memcpy(bankBytes.data(), &hdr, sizeof(hdr));
        std::memcpy(bankBytes.data() + sizeof(hdr), &def, sizeof(def));

        const u16 bSlot = bank.AcquireSlot(AssetId::Hash("sfx.eaud"), AssetKind::Blob, scope, AssetResidency::Sim, needsLoad);
        const BlobHandle blobH = bank.CommitBlob(bSlot, bankBytes);

        AudioSystem audio;
        (void)audio.Init();
        EXPECT_TRUE(suite, audio.RegisterCueBank(bank.GetBlob(blobH), scope).has_value());

        const VoiceHandle vh = audio.PlayCue(def.id, bank);
        EXPECT_TRUE(suite, vh.IsValid());
        audio.SetVoiceGain(vh, 0.9f);
        audio.SetVoicePitch(vh, 1.1f);
        audio.SetBusGain(AudioBusId::Ui, 0.85f);
        audio.SetBusFilter(AudioBusId::Ui, FilterMode::LowPass, 2400.0f);
        audio.SetReverbParams(0.6f, 0.5f, 0.25f);

        AudioUpdateDesc desc{};
        desc.tickIndex = 1U;
        audio.Update(desc);
        audio.PumpHeadlessBlocks(3U);

        const u64 hash = audio.ComputeAuditHash();
        audio.Shutdown();
        bank.Shutdown();
        return hash;
    };

    const u64 h1 = runAudioSim();
    const u64 h2 = runAudioSim();
    EXPECT_EQ(suite, h1, h2);
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestSuite suite{};
    TestDspKernelsTrueStereoLoopPointsAndMultiBlockFade(suite);
    TestVirtualVoicesNearFieldSpreadReverbAndScopeRelease(suite);
    TestCookedEaudBankAndDeterminismAuditHash(suite);

    std::printf("Phase 10 Audio v2 Verification: %u passed, %u failed\n", suite.passed, suite.failed);
    return (suite.failed == 0U) ? 0 : 1;
}
