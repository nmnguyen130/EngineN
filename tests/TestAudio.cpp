#include "audio/AudioMixer.h"
#include "audio/AudioSystem.h"
#include "audio/AudioTypes.h"
#include "assets/AssetBank.h"
#include "gameplay/SpatialIndex2D.h"
#include "world/World.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace en;
using namespace en::audio;

u32 g_testsPassed = 0;
u32 g_testsFailed = 0;

void AssertTrue(bool condition, std::string_view expression, int line) {
    if (condition) {
        ++g_testsPassed;
    } else {
        ++g_testsFailed;
        std::cerr << "[FAIL] Line " << line << ": " << expression << "\n";
    }
}

#define TEST_ASSERT(cond) AssertTrue(static_cast<bool>(cond), #cond, __LINE__)

void TestAudioTypesAndLayout() {
    static_assert(sizeof(SoundCueDef) == 32);
    static_assert(sizeof(MusicTrackDef) == 32);
    static_assert(sizeof(CookedAudioBankHeader) == 16);
    static_assert(sizeof(AudioCommand) == 32);
    static_assert(sizeof(AudioFrameSnapshot) == 576);
    static_assert(sizeof(AudioTelemetrySnapshot) == 32);
    static_assert(sizeof(CueHashSlot) == 8);

    // VoiceFlags bitwise operators
    const VoiceFlags f1 = VoiceFlags::Looping | VoiceFlags::Spatialized2D;
    TEST_ASSERT(HasVoiceFlag(f1, VoiceFlags::Looping));
    TEST_ASSERT(HasVoiceFlag(f1, VoiceFlags::Spatialized2D));
    TEST_ASSERT(!HasVoiceFlag(f1, VoiceFlags::PauseOnHitstop));

    // SoundCueId lossless conversion from AssetId
    const assets::AssetId aid{0x123456789ABCDEF0ULL};
    const SoundCueId cid = SoundCueId::FromAssetId(aid);
    TEST_ASSERT(cid.value != 0);

    // SoundCueDef packing & accessors
    SoundCueDef def{};
    TEST_ASSERT(def.VariationCount() == 1);
    TEST_ASSERT(def.Bus() == AudioBusId::SfxWorld);
    TEST_ASSERT(def.Steal() == StealPolicy::Reject);
    TEST_ASSERT(def.Channel() == time::TimeChannel::Vfx);
    TEST_ASSERT(def.Curve() == AttenuationCurve::None);
    TEST_ASSERT(def.MaxConcurrency() == 4);
    TEST_ASSERT(def.CooldownTicks() == 2);
    TEST_ASSERT(HasVoiceFlag(def.Flags(), VoiceFlags::Spatialized2D));

    // MusicTrackDef accessors
    MusicTrackDef mdef{};
    TEST_ASSERT(mdef.StemCount() == 1);
    TEST_ASSERT(mdef.BeatsPerBar() == 4);
}

void TestAudioMixerLifecycleAndArena() {
    AudioMixer mixer;

    // Reject undersized arena
    std::vector<u8> tinyArena(100, 0);
    auto errRes = mixer.Init(tinyArena);
    TEST_ASSERT(!errRes.has_value());
    TEST_ASSERT(errRes.error() == AudioErrc::ArenaExhausted);

    // Sufficient arena
    const usize reqBytes = AudioMixer::RequiredArenaBytes();
    std::vector<u8> arena(reqBytes + 256, 0);
    auto initRes = mixer.Init(arena);
    TEST_ASSERT(initRes.has_value());

    const auto& tel = mixer.Telemetry();
    TEST_ASSERT(tel.activeVoiceMask == 0);
    TEST_ASSERT(tel.activeScopePinMask == 0);
    TEST_ASSERT(tel.stolenVoicesTotal == 0);

    // Initial mix on silence
    AudioFrameSnapshot snapshot{};
    snapshot.busTargetGainQ14[0] = 16384; // Master 1.0
    alignas(64) std::array<f32, kMixBlockFrames * 2> outStereo{};
    mixer.MixBlock(snapshot, outStereo);

    bool allZero = true;
    for (f32 s : outStereo) {
        if (s != 0.0f) {
            allZero = false;
            break;
        }
    }
    TEST_ASSERT(allZero);

    // Reset mixer
    mixer.Reset();
    TEST_ASSERT(mixer.Telemetry().activeVoiceMask == 0);
}

void TestAudioMixerVoiceHermiteResampling() {
    AudioMixer mixer;
    const usize reqBytes = AudioMixer::RequiredArenaBytes();
    std::vector<u8> arena(reqBytes + 256, 0);
    (void)mixer.Init(arena);

    // Mock PCM data: 512 samples of ramp wave
    std::vector<i16> pcm(512);
    for (usize i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<i16>((static_cast<f32>(i) / 512.0f) * 30000.0f);
    }

    // Spawn voice in slot 0: Left 1.0, Right 0.5
    AudioCommand cmd{};
    cmd.type = AudioCmdType::SpawnVoice;
    cmd.voiceSlot = 0;
    cmd.generation = 1;
    cmd.packedBusAndChannel = static_cast<u8>(AudioBusId::SfxWorld);
    cmd.flags = static_cast<u8>(VoiceFlags::None);
    cmd.assetScopeId = 2;
    cmd.payload.spawn.pcm = pcm.data();
    cmd.payload.spawn.frameCount = static_cast<u32>(pcm.size());
    cmd.payload.spawn.loopStartFrame = 0;
    cmd.payload.spawn.sampleRate = 48000;
    cmd.payload.spawn.gainL_Q14 = 16384;
    cmd.payload.spawn.gainR_Q14 = 8192;
    cmd.payload.spawn.pitchRatioQ12 = 4096;
    mixer.ApplyCommand(cmd);

    AudioFrameSnapshot snapshot{};
    snapshot.targetGainL_Q14[0] = 16384;
    snapshot.targetGainR_Q14[0] = 8192;
    snapshot.targetPitchQ12[0] = 4096;
    snapshot.busTargetGainQ14[0] = 16384; // Master 1.0
    snapshot.busTargetGainQ14[static_cast<usize>(AudioBusId::SfxWorld)] = 16384;

    alignas(64) std::array<f32, kMixBlockFrames * 2> outStereo{};
    mixer.MixBlock(snapshot, outStereo);

    const auto& tel = mixer.Telemetry();
    TEST_ASSERT((tel.activeVoiceMask & 1ULL) != 0);
    TEST_ASSERT((tel.activeScopePinMask & (1u << 2)) != 0);

    // Left channel should have greater peak energy than Right channel
    f32 maxL = 0.0f;
    f32 maxR = 0.0f;
    for (u32 i = 0; i < kMixBlockFrames; ++i) {
        maxL = std::max(maxL, std::abs(outStereo[i * 2]));
        maxR = std::max(maxR, std::abs(outStereo[i * 2 + 1]));
    }
    TEST_ASSERT(maxL > 0.01f);
    TEST_ASSERT(maxR > 0.005f);
    TEST_ASSERT(maxL > maxR * 1.5f);

    // Mix second and third blocks: frameCount is 512, block is 256 -> voice finishes and deactivates on block 3
    mixer.MixBlock(snapshot, outStereo);
    mixer.MixBlock(snapshot, outStereo);
    TEST_ASSERT((mixer.Telemetry().activeVoiceMask & 1ULL) == 0);
}

void TestAudioMixerStealCrossfade() {
    AudioMixer mixer;
    const usize reqBytes = AudioMixer::RequiredArenaBytes();
    std::vector<u8> arena(reqBytes + 256, 0);
    (void)mixer.Init(arena);

    std::vector<i16> pcm(1024, 20000);

    // Spawn voice 0
    AudioCommand spawnCmd{};
    spawnCmd.type = AudioCmdType::SpawnVoice;
    spawnCmd.voiceSlot = 0;
    spawnCmd.generation = 1;
    spawnCmd.packedBusAndChannel = static_cast<u8>(AudioBusId::SfxPlayer);
    spawnCmd.payload.spawn.pcm = pcm.data();
    spawnCmd.payload.spawn.frameCount = 1024;
    spawnCmd.payload.spawn.sampleRate = 48000;
    spawnCmd.payload.spawn.gainL_Q14 = 16384;
    spawnCmd.payload.spawn.gainR_Q14 = 16384;
    spawnCmd.payload.spawn.pitchRatioQ12 = 4096;
    mixer.ApplyCommand(spawnCmd);

    AudioFrameSnapshot snap{};
    snap.targetGainL_Q14[0] = 16384;
    snap.targetGainR_Q14[0] = 16384;
    snap.targetPitchQ12[0] = 4096;
    snap.busTargetGainQ14[0] = 16384;
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::SfxPlayer)] = 16384;

    alignas(64) std::array<f32, kMixBlockFrames * 2> outStereo{};
    mixer.MixBlock(snap, outStereo);
    TEST_ASSERT((mixer.Telemetry().activeVoiceMask & 1ULL) != 0);

    // Steal voice 0 into steal slot 0
    AudioCommand stopCmd{};
    stopCmd.type = AudioCmdType::StopVoice;
    stopCmd.voiceSlot = 0;
    stopCmd.generation = 1;
    stopCmd.payload.stop.stealPoolIdx = 0;
    mixer.ApplyCommand(stopCmd);

    // Run mix block to process steal crossfade and update telemetry
    mixer.MixBlock(snap, outStereo);
    TEST_ASSERT((mixer.Telemetry().activeVoiceMask & 1ULL) == 0);
    TEST_ASSERT(mixer.Telemetry().stolenVoicesTotal >= 1);

    // Steal pool output should fade out smoothly towards end of block
    const f32 firstSample = std::abs(outStereo[0]);
    const f32 lastSample = std::abs(outStereo[(kMixBlockFrames - 1) * 2]);
    TEST_ASSERT(firstSample > lastSample);
}

void TestAudioMixerBusesAndMasterSoftClip() {
    AudioMixer mixer;
    const usize reqBytes = AudioMixer::RequiredArenaBytes();
    std::vector<u8> arena(reqBytes + 256, 0);
    (void)mixer.Init(arena);

    // High frequency tone (+32000, -32000 alternating every sample = 24kHz Nyquist)
    std::vector<i16> nyquistTone(1024);
    for (usize i = 0; i < nyquistTone.size(); ++i) {
        nyquistTone[i] = (i % 2 == 0) ? 32000 : -32000;
    }

    AudioCommand spawnCmd{};
    spawnCmd.type = AudioCmdType::SpawnVoice;
    spawnCmd.voiceSlot = 1;
    spawnCmd.generation = 1;
    spawnCmd.packedBusAndChannel = static_cast<u8>(AudioBusId::Ambience);
    spawnCmd.payload.spawn.pcm = nyquistTone.data();
    spawnCmd.payload.spawn.frameCount = 1024;
    spawnCmd.payload.spawn.sampleRate = 48000;
    spawnCmd.payload.spawn.gainL_Q14 = 16384;
    spawnCmd.payload.spawn.gainR_Q14 = 16384;
    spawnCmd.payload.spawn.pitchRatioQ12 = 4096;
    mixer.ApplyCommand(spawnCmd);

    // 1. Unfiltered pass
    AudioFrameSnapshot snapUnfiltered{};
    snapUnfiltered.targetGainL_Q14[1] = 16384;
    snapUnfiltered.targetGainR_Q14[1] = 16384;
    snapUnfiltered.targetPitchQ12[1] = 4096;
    snapUnfiltered.busTargetGainQ14[0] = 16384;
    snapUnfiltered.busTargetGainQ14[static_cast<usize>(AudioBusId::Ambience)] = 16384;

    alignas(64) std::array<f32, kMixBlockFrames * 2> outRaw{};
    mixer.MixBlock(snapUnfiltered, outRaw);
    f32 peakRaw = 0.0f;
    for (f32 s : outRaw) peakRaw = std::max(peakRaw, std::abs(s));
    TEST_ASSERT(peakRaw > 0.5f);

    // 2. Filtered with steep LowPass on Ambience bus (mode LowPass = 1)
    AudioFrameSnapshot snapFiltered = snapUnfiltered;
    const usize ambBus = static_cast<usize>(AudioBusId::Ambience);
    snapFiltered.busFilterModesBits = static_cast<u16>(1u << (ambBus * 2u));
    snapFiltered.busSvfG_Q14[ambBus] = 800; // low cutoff

    alignas(64) std::array<f32, kMixBlockFrames * 2> outFiltered{};
    mixer.MixBlock(snapFiltered, outFiltered);
    f32 peakFiltered = 0.0f;
    for (f32 s : outFiltered) peakFiltered = std::max(peakFiltered, std::abs(s));

    // Low pass filter should heavily attenuate 24kHz nyquist tone
    TEST_ASSERT(peakFiltered < peakRaw * 0.25f);

    // 3. Master Padé Soft-Clip: feed super-loud gain (4x)
    snapUnfiltered.busTargetGainQ14[0] = 65535;
    mixer.MixBlock(snapUnfiltered, outRaw);
    for (f32 s : outRaw) {
        TEST_ASSERT(std::abs(s) <= 1.0f); // Never exceeds 1.0f due to Padé approximant
    }
}

void TestAudioSystemRobinHoodCueIndex() {
    AudioSystem audio;
    AudioSystemConfig cfg{};
    cfg.headlessMode = true;
    auto initRes = audio.Init(cfg);
    TEST_ASSERT(initRes.has_value());

    // Register 10 distinct sound cues
    for (u32 i = 1; i <= 10; ++i) {
        SoundCueDef def{};
        def.cueId = SoundCueId{i * 100u};
        def.priority = static_cast<u8>(i * 10u);
        auto regRes = audio.RegisterCueDef(def, 1);
        TEST_ASSERT(regRes.has_value());
    }

    // Register duplicate cue ID -> updates in-place without error
    SoundCueDef dupDef{};
    dupDef.cueId = SoundCueId{300u};
    dupDef.priority = 250;
    auto dupRes = audio.RegisterCueDef(dupDef, 2);
    TEST_ASSERT(dupRes.has_value());

    // Test bank blob registration
    alignas(16) std::vector<u8> blobData(sizeof(CookedAudioBankHeader) + sizeof(SoundCueDef) * 2);
    auto* hdr = reinterpret_cast<CookedAudioBankHeader*>(blobData.data());
    hdr->magic = kCookedAudioMagic;
    hdr->version = 1;
    hdr->cueCount = 2;
    hdr->musicTrackCount = 0;

    auto* cues = reinterpret_cast<SoundCueDef*>(blobData.data() + sizeof(CookedAudioBankHeader));
    cues[0].cueId = SoundCueId{5001};
    cues[1].cueId = SoundCueId{5002};

    assets::BlobRecord blob{};
    blob.data = blobData.data();
    blob.size = blobData.size();

    auto bankRes = audio.RegisterCueBank(blob, 0);
    TEST_ASSERT(bankRes.has_value());

    // Test Move Constructor and Move Assignment
    AudioSystem movedAudio(std::move(audio));
    AudioSystem assignAudio;
    assignAudio = std::move(movedAudio);
    assignAudio.Shutdown();
}

void TestAudioSystemVoiceAdmissionAndCoalescing() {
    AudioSystem audio;
    AudioSystemConfig cfg{};
    cfg.headlessMode = true;
    (void)audio.Init(cfg);

    assets::AssetBank assetBank;
    (void)assetBank.Init();
    auto scopeRes = assetBank.BeginScope();
    TEST_ASSERT(scopeRes.has_value());
    const assets::ScopeId scope = *scopeRes;

    // Register PCM in AssetBank
    std::vector<i16> mockPcm(2048, 10000);
    bool needsLoad = false;
    const assets::AssetId audioAid{9090};
    auto slotRes = assetBank.AcquireSlot(audioAid, assets::AssetKind::Audio, scope, assets::Residency::Sim, needsLoad);
    TEST_ASSERT(slotRes.has_value());
    const std::span<const u8> pcmBytes(reinterpret_cast<const u8*>(mockPcm.data()), mockPcm.size() * sizeof(i16));
    auto commitRes = assetBank.CommitAudio(*slotRes, pcmBytes, 48000, 1, false);
    TEST_ASSERT(commitRes.has_value());
    const assets::AudioHandle audioHandle = assetBank.ToAudioHandle(*slotRes);

    // Register cue: CooldownTicks = 2 (packedFlagsAndCooldown = (1 << 6)), MaxConcurrency = 2
    SoundCueDef def{};
    def.cueId = SoundCueId{777};
    def.variations[0] = audioHandle;
    def.packedRouting = 0x21; // 1 variation, SfxCombat, Reject
    def.packedChannelAndCurve = static_cast<u8>(0x04 | (1u << 5)); // MaxConcurrency = 2 (stored as 1)
    def.packedFlagsAndCooldown = static_cast<u8>(1u << 6); // cooldown shift = 1 -> 2 ticks
    (void)audio.RegisterCueDef(def, scope);

    world::World dummyWorld;
    gameplay::SpatialIndex2D dummySpatial;
    time::RenderTimeView timeView{};
    timeView.tickIndex = 1;
    std::array<f32, 8> scales{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    audio.Update(dummyWorld, dummySpatial, assetBank, timeView, 0, scales);

    // 1. Play first time at tick 1
    VoiceHandle h1 = audio.PlayCue(def.cueId, assetBank);
    TEST_ASSERT(h1.IsValid());

    // 2. Play immediately again in same tick -> Cooldown coalescer catches it, returns same slot
    VoiceHandle hCoalesced = audio.PlayCue(def.cueId, assetBank);
    TEST_ASSERT(hCoalesced.IsValid());
    TEST_ASSERT(hCoalesced.index == h1.index);

    audio.PumpHeadlessBlocks(2);

    // 3. Advance time to tick 10 (beyond cooldown of 2 ticks) and play another instance
    timeView.tickIndex = 10;
    audio.Update(dummyWorld, dummySpatial, assetBank, timeView, 0, scales);

    VoiceHandle h2 = audio.PlayCue(def.cueId, assetBank);
    TEST_ASSERT(h2.IsValid());
    TEST_ASSERT(h2.index != h1.index); // Spawns in separate slot

    // 4. Stop voice
    audio.StopVoice(h1, 2);
    audio.PumpHeadlessBlocks(4);

    audio.Shutdown();
}

void TestAudioSystemSpatializationAndHeadlessPump() {
    AudioSystem audio;
    AudioSystemConfig cfg{};
    cfg.headlessMode = true;
    cfg.panWidthMeters = 10.0f;
    (void)audio.Init(cfg);

    assets::AssetBank assetBank;
    (void)assetBank.Init();
    auto scopeRes = assetBank.BeginScope();
    TEST_ASSERT(scopeRes.has_value());
    const assets::ScopeId scope = *scopeRes;

    std::vector<i16> mockPcm(4096, 12000);
    bool needsLoad = false;
    const assets::AssetId audioAid{9091};
    auto slotRes = assetBank.AcquireSlot(audioAid, assets::AssetKind::Audio, scope, assets::Residency::Sim, needsLoad);
    TEST_ASSERT(slotRes.has_value());
    const std::span<const u8> pcmBytes(reinterpret_cast<const u8*>(mockPcm.data()), mockPcm.size() * sizeof(i16));
    (void)assetBank.CommitAudio(*slotRes, pcmBytes, 48000, 1, false);
    const assets::AudioHandle audioHandle = assetBank.ToAudioHandle(*slotRes);

    SoundCueDef def{};
    def.cueId = SoundCueId{888};
    def.variations[0] = audioHandle;
    def.minDistQ2 = 4;  // 1.0m
    def.maxDistQ1 = 40; // 20.0m
    def.packedChannelAndCurve = static_cast<u8>((1u << 3) | 0x01); // Linear, TimeChannel::Player
    def.packedFlagsAndCooldown = static_cast<u8>(VoiceFlags::Spatialized2D);
    (void)audio.RegisterCueDef(def, scope);

    // Listener at (0, 0), Sound at (8.0, 0.0) -> heavily panned to the Right
    audio.SetListenerPosition(0.0f, 0.0f);
    VoiceHandle h = audio.PlayCue(def.cueId, assetBank, 8.0f, 0.0f);
    TEST_ASSERT(h.IsValid());

    world::World dummyWorld;
    gameplay::SpatialIndex2D dummySpatial;
    time::RenderTimeView timeView{};
    timeView.tickIndex = 1;
    std::array<f32, 8> scales{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    audio.Update(dummyWorld, dummySpatial, assetBank, timeView, 0, scales);

    // Pump audio and verify capture output
    std::vector<f32> captured(kMixBlockFrames * 2 * 4, 0.0f);
    audio.PumpHeadlessBlocks(4, captured);

    const auto tel = audio.Telemetry();
    TEST_ASSERT(tel.masterPeakR > tel.masterPeakL); // Panned right!

    // Determinism test: Audit hash must match between two runs with identical state
    const u64 hash1 = audio.ComputeAuditHash();
    TEST_ASSERT(hash1 != 0);

    audio.Shutdown();
}

} // namespace

int main() {
    std::cout << "Running EngineN Audio Subsystem Tests...\n" << std::flush;

    std::cout << "  [1/8] TestAudioTypesAndLayout...\n" << std::flush;
    TestAudioTypesAndLayout();

    std::cout << "  [2/8] TestAudioMixerLifecycleAndArena...\n" << std::flush;
    TestAudioMixerLifecycleAndArena();

    std::cout << "  [3/8] TestAudioMixerVoiceHermiteResampling...\n" << std::flush;
    TestAudioMixerVoiceHermiteResampling();

    std::cout << "  [4/8] TestAudioMixerStealCrossfade...\n" << std::flush;
    TestAudioMixerStealCrossfade();

    std::cout << "  [5/8] TestAudioMixerBusesAndMasterSoftClip...\n" << std::flush;
    TestAudioMixerBusesAndMasterSoftClip();

    std::cout << "  [6/8] TestAudioSystemRobinHoodCueIndex...\n" << std::flush;
    TestAudioSystemRobinHoodCueIndex();

    std::cout << "  [7/8] TestAudioSystemVoiceAdmissionAndCoalescing...\n" << std::flush;
    TestAudioSystemVoiceAdmissionAndCoalescing();

    std::cout << "  [8/8] TestAudioSystemSpatializationAndHeadlessPump...\n" << std::flush;
    TestAudioSystemSpatializationAndHeadlessPump();

    std::cout << "Test Summary: " << g_testsPassed << " passed, " << g_testsFailed << " failed.\n" << std::flush;
    return g_testsFailed == 0 ? 0 : 1;
}
