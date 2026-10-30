#include "tests/subsystems/CtsHarness.h"

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"

#include <vector>

namespace {

using namespace en;
using namespace en::cts;
using namespace en::literals;

bool CtsSpatialPanningAttenuationAndVoiceStealing(CtsRunner& r) noexcept {
    AssetBank bank;
    CTS_CHECK(r, bank.Init().has_value());
    const u8 scope = *bank.BeginScope();

    std::vector<f32> pcm(2048U, 0.5f);
    const AudioHandle tone = bank.RegisterAudio("sfx/tone.wav"_aid, scope, pcm, 48000U, 2U, false);
    CTS_CHECK(r, tone.IsValid());

    AudioSystem audio;
    CTS_CHECK(r, audio.Init({.panWidth = 10.0f, .headless = true}).has_value());

    CTS_CHECK(r, audio.RegisterCue(SoundCueDef{
        .id             = "cue/low"_aid,
        .variants       = {tone, {}},
        .minDist        = 1.0f,
        .maxDist        = 20.0f,
        .baseGain       = 1.0f,
        .cooldownTicks  = 0U,
        .priority       = 50U,
        .maxConcurrency = 64U,
        .flags          = VoiceFlags::Spatial2D,
        .bus            = AudioBus::Sfx,
    }).has_value());

    CTS_CHECK(r, audio.RegisterCue(SoundCueDef{
        .id             = "cue/high"_aid,
        .variants       = {tone, {}},
        .minDist        = 1.0f,
        .maxDist        = 20.0f,
        .baseGain       = 1.0f,
        .cooldownTicks  = 0U,
        .priority       = 200U,
        .maxConcurrency = 64U,
        .flags          = VoiceFlags::Spatial2D,
        .bus            = AudioBus::Sfx,
    }).has_value());

    const VoiceHandle vRight = audio.Play("cue/high"_aid, bank, {10.0f, 0.0f});
    CTS_CHECK(r, vRight.IsValid());

    std::vector<f32> captured(kMixBlockFrames * 2U, 0.0f);
    audio.Pump(1U, captured);
    f32 sumL = 0.0f, sumR = 0.0f;
    for (u32 i = 0U; i < kMixBlockFrames; ++i) {
        sumL += std::abs(captured[i * 2U]);
        sumR += std::abs(captured[i * 2U + 1U]);
    }
    CTS_CHECK(r, sumR > sumL * 2.0f && audio.Telemetry().peakR > 0.1f);
    audio.StopAll(0U);
    audio.Pump(1U);

    for (u32 i = 0U; i < kMaxAudioVoices; ++i) {
        CTS_CHECK(r, audio.Play("cue/low"_aid, bank, {0.0f, 0.0f}).IsValid());
    }
    const VoiceHandle stolen = audio.Play("cue/high"_aid, bank, {0.0f, 0.0f});
    CTS_CHECK(r, stolen.IsValid());
    audio.Pump(1U);
    CTS_CHECK(r, audio.Telemetry().voices == kMaxAudioVoices && audio.Telemetry().stolen >= 1U);
    return true;
}

bool CtsAdaptiveMusicStemsCrossfadeAndDucking(CtsRunner& r) noexcept {
    AssetBank bank;
    CTS_CHECK(r, bank.Init().has_value());
    const u8 scope = *bank.BeginScope();

    std::vector<f32> stem0Pcm(2048U, 0.3f);
    std::vector<f32> stem1Pcm(2048U, 0.6f);
    const AudioHandle stem0 = bank.RegisterAudio("mus/calm.wav"_aid, scope, stem0Pcm, 48000U, 2U, true);
    const AudioHandle stem1 = bank.RegisterAudio("mus/combat.wav"_aid, scope, stem1Pcm, 48000U, 2U, true);

    AudioSystem audio;
    CTS_CHECK(r, audio.Init({.headless = true}).has_value());

    CTS_CHECK(r, audio.RegisterMusic(MusicTrackDef{
        .id        = "mus/boss"_aid,
        .stems     = {stem0, stem1, {}, {}},
        .weights   = {1.0f, 0.0f, 0.0f, 0.0f},
        .stemCount = 2U,
        .loop      = true,
    }).has_value());

    CTS_CHECK(r, audio.PlayMusic("mus/boss"_aid, bank, 4U));
    std::vector<f32> mixOut(kMixBlockFrames * 2U, 0.0f);
    audio.Pump(4U, mixOut);
    CTS_CHECK(r, audio.Telemetry().stems == 2U && audio.Telemetry().peakL > 0.05f);

    const f32 calmPeak = audio.Telemetry().peakL;
    const std::array<f32, kMaxMusicStems> combatWeights{1.0f, 1.0f, 0.0f, 0.0f};
    audio.SetStemWeights(combatWeights);
    audio.Pump(2U, mixOut);
    CTS_CHECK(r, audio.Telemetry().peakL > calmPeak);

    CTS_CHECK(r, audio.RegisterCue(SoundCueDef{
        .id       = "sfx/boom"_aid,
        .variants = {stem1, {}},
        .baseGain = 1.5f,
        .flags    = VoiceFlags::None,
        .bus      = AudioBus::Sfx,
    }).has_value());
    (void)audio.Play("sfx/boom"_aid, bank);
    audio.Pump(2U, mixOut);
    CTS_CHECK(r, audio.Telemetry().duckEnv > 0.01f);
    return true;
}

bool DstDeterministicAudioMixerLockstep(CtsRunner& r) noexcept {
    auto runAudioSim = [](u64& outHash) noexcept -> bool {
        AssetBank bank;
        if (!bank.Init().has_value()) return false;
        const u8 s = *bank.BeginScope();

        std::vector<f32> wave(1024U);
        for (usize i = 0U; i < wave.size(); ++i) {
            f32 c = 1.0f, sn = 0.0f;
            FastSinCos(static_cast<f32>(i) * 0.1f, c, sn);
            wave[i] = sn * 0.4f;
        }
        const AudioHandle clip = bank.RegisterAudio("sfx/synth"_aid, s, wave, 48000U, 1U, false);

        AudioSystem audio;
        if (!audio.Init({.headless = true}).has_value()) return false;
        (void)audio.RegisterCue(SoundCueDef{
            .id             = "cue/synth"_aid,
            .variants       = {clip, {}},
            .gainJitter     = 0.1f,
            .pitchJitter    = 0.1f,
            .cooldownTicks  = 1U,
            .maxConcurrency = 16U,
            .flags          = VoiceFlags::Spatial2D | VoiceFlags::FollowTimeScale,
        });

        for (u32 tick = 1U; tick <= 60U; ++tick) {
            AudioTrigger trig{
                .cueId    = "cue/synth"_aid,
                .position = {static_cast<f32>(tick % 10U) - 5.0f, 1.0f},
            };
            const bool triggerThisTick = (tick % 3U) == 0U;
            AudioUpdateDesc desc{
                .triggers      = triggerThisTick ? std::span<const AudioTrigger>{&trig, 1U} : std::span<const AudioTrigger>{},
                .assets        = &bank,
                .occlusionFn   = [](Vec2 l, Vec2 e, void*) noexcept { return (e.x - l.x) > 2.0f; },
                .listenerPos   = {0.0f, 0.0f},
                .tick          = tick,
            };
            audio.Update(desc);
            audio.Pump(1U);
        }
        outHash = audio.ComputeAuditHash();
        return audio.Telemetry().blocks == 60ULL;
    };

    u64 hashA = 0ULL;
    u64 hashB = 0ULL;
    CTS_CHECK(r, runAudioSim(hashA) && runAudioSim(hashB));
    CTS_CHECK(r, hashA == hashB && hashA != 0ULL);
    r.RecordFuzzOps(60U * kMixBlockFrames);
    r.MixDigest(hashA);
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "AudioTests (CTS+DST)"};
    runner.RunCase("Spatial Panning, Attenuation & Voice Stealing", CtsSpatialPanningAttenuationAndVoiceStealing);
    runner.RunCase("Adaptive Music Stems, Crossfade & Bus Ducking", CtsAdaptiveMusicStemsCrossfadeAndDucking);
    runner.RunCase("DST: 60-Block Lockstep Resampling & SVF Stress", DstDeterministicAudioMixerLockstep);
    return runner.Finish();
}
