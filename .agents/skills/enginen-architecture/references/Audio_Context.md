# EngineN Phase 10: `audio/` (v2) Subsystem Architecture & Reference

## 1. Zero-Coupling Boundary & Header Hygiene
- `audio/` includes ONLY `"core/Types.h"`, `"core/Error.h"`, `"core/Memory.h"`, `"core/Math.h"`, `"time/TimeTypes.h"`, and `"assets/AssetTypes.h"` (`AudioSystem.cpp` includes `"assets/AssetBank.h"`).
- **Zero Cross-Subsystem Header Leaks:** Zero `world/`, `physics/`, `gameplay/`, `platform/`, `renderer/`, or `input/` headers.
- **Zero Type Aliases & Zero Hidden Padding:** `0` `using` type aliases across `audio/*.h` and `audio/*.cpp`; `0` dummy `reserved` padding fields; `0B` hidden compiler padding verified via `static_assert`.

## 2. Exact Struct Layouts (`audio/AudioTypes.h`)
- `VoiceHandle` (`4B`): `u16 index{0}`, `u16 generation{0}` (`generation == 0` is invalid).
- `SoundCueId` (`4B`): `u32 value{0}` with `constexpr SoundCueId::Hash(std::string_view)`.
- `AudioTriggerReq` (`16B`, `alignas(16)`): `SoundCueId cueId`, `f32 worldX`, `f32 worldY`, `u32 emitterId`.
- `AudioEmitterView` (`16B`, `alignas(16)`): `f32 worldX`, `f32 worldY`, `f32 maxDist`, `f32 occlusionFactor`.
- `CookedAudioBankHeader` (`16B`, `alignas(16)`): `u32 magic{0x44554145U}` (`"EAUD"`), `u16 version{1}`, `u16 cueCount`, `u16 musicTrackCount`, `u16 flags`, `u32 checksum`.
- `SoundCueDef` (`32B`, `alignas(32)`):
  - `SoundCueId id` (`4B`), `AudioHandle clipVariants[2]` (`8B`), `f32 minDist{2.0f}` (`4B`), `f32 maxDist{24.0f}` (`4B`), `u16 baseGain_q14{16384}` (`2B`), `u16 basePitch_q12{4096}` (`2B`), `u16 loopStartFrame{0}` (`2B`), `u8 priority{128}` (`1B`), `u8 maxConcurrency{4}` (`1B`), `u8 cooldownTicks{2}` (`1B`), `VoiceFlags flags` (`1B`), `u8 packedRouting` (`1B`: `AudioBusId`, `TimeChannel`, `StealPolicy`), `u8 packedCurveAndJitter` (`1B`: `AttenuationCurve`, `GainJitter`, `PitchJitter`).
- `MusicTrackDef` (`32B`, `alignas(32)`): `u32 trackId`, `AudioHandle stems[4]` (`16B`), `u16 defaultWeights_q14[4]` (`8B`), `u16 loopStartFrame`, `u8 stemCount`, `u8 loop`.
- `AudioCommand` (`32B`, `alignas(32)`, descending alignment): `const f32* pcmData` (`8B` at offset 0), `u32 frameCount` (`4B`), `u16 loopStartFrame` (`2B`), `u16 sampleRate` (`2B`), `u16 initialGainL_q14` (`2B`), `u16 initialGainR_q14` (`2B`), `u16 initialPitch_q12` (`2B`), `u16 generation` (`2B`), `u16 fadeBlocks` (`2B`), `AudioCmdType type` (`1B`), `AudioBusId bus` (`1B`), `u8 voiceSlot` (`1B`), `VoiceFlags flags` (`1B`), `u8 channels` (`1B`), `u8 assetScope` (`1B`).
- `AudioFrameSnapshot` (`576B`, `alignas(64)`, 9 cache lines): `targetGainL_q14[64]`, `targetGainR_q14[64]`, `targetPitch_q12[64]`, `voiceCutoffHz[64]`, `busGain_q14[8]`, `busCutoffHz[8]`, `busFilterMode[8]`, `musicStemWeights_q14[4]`, `pausedVoicesMask`, `sidechainDuckStrength_q14`, `reverbWetGain_q14`, `reverbRoomSize_q8`, `reverbDamping_q8`, `masterMuted`, `reverbBusSendMask`.
- `AudioTelemetrySnapshot` (`32B`, `alignas(32)`): `u64 mixedBlocksCount`, `f32 masterPeakL`, `f32 masterPeakR`, `f32 sidechainEnvelope`, `f32 reverbPeak`, `u16 activeVoiceCount`, `u16 virtualVoiceCount`, `u16 stolenVoiceCount`, `u16 activeMusicStems`.
- `AudioUpdateDesc` (`64B`, `alignas(64)`): Single-call per-tick orchestration descriptor.

## 3. DSP & Mixer Engine (`audio/AudioMixer.h/.cpp` v2)
- `sizeof(AudioMixer) == 64` (1 cache line exact) operating over a contiguous 56 KB `MixerArenaState` with zero dead fields (`m_freshSpawnMask`, `m_lastSnapshotSeq`, `m_activeVoiceCount` removed).
- **True Stereo & Mono 4-Point Catmull-Rom Hermite Resampler (`InterpolateHermite4`):** Natively synthesizes both 1-channel and 2-channel stereo clips using `baseRateStep32_32` (`(sampleRate << 32) / kAudioSampleRate`) scaled by `pitch_q12` without cumulative pitch drift.
- **Seamless Sample-Accurate Loop Points (`[loopStartFrame .. frameCount)`):** Preserves sub-sample fractional phase across intro-to-sustain loop boundaries for both polyphonic voices and 4-stem music tracks.
- **Virtual Voice Fast-Path (`m_virtualVoicesMask`):** Looping spatial emitters outside `maxDist` (`gain == 0`) advance `phase32_32` in O(1) (`step * kMixBlockFrames` with loop wrapping) without sample synthesis, and promote seamlessly to real voices when entering `maxDist`.
- **Unified Cytomic Linear Trapezoidal SVF (`StepCytomicSvf`) & In-Place Stolen-Voice / Music Deck Tail Crossfader:** Zero-delay feedback (`LowPass`, `HighPass`, `BandPass`) shared between voices and buses; click-free 256-sample tail rendering on voice steals, hard stops, and music track transitions.
- **Integrated Stereo Schroeder / Freeverb Aux Reverb:** 4 parallel lowpass-feedback comb filters + 2 series allpass diffusers per channel (`SetReverbParams(roomSize, damping, wetGain, busSendMask)` + `VoiceFlags::ReverbSend`), bypassed in O(1) when `reverbWetGain_q14 == 0`.
- **Sidechain Envelope Ducking & Padé `[3/2]` Soft-Clipper:** Automatic `Music` + `Ambience` ducking driven by `SfxCombat + Voice` peak envelope, followed by rational Padé $\tanh$ master limiting.

## 4. Main-Thread Coordinator (`audio/AudioSystem.h/.cpp` v2)
- `sizeof(AudioSystem) == 128` (2 cache lines exact), `sizeof(MainVoiceTracker) == 32` (stores `instanceGain_q14` and `instancePitch_q12` so randomized spawn jitter and same-tick energy-summing boosts persist across `Update()` ticks).
- **Near-Field Spatial Spread (`dist / minDist`), Radial 2D Doppler & Staggered 30 Hz Occlusion:** Smoothly collapses stereo panning toward center inside `minDist` to prevent speaker-flip artifacts when crossing `listenerX`.
- **Dynamic Emitter & Voice Controls:** `SetVoicePosition`, `UpdateEmitterTransform`, `SetVoiceGain`, `SetVoicePitch`, `StopVoice` (with smooth multi-block decay that retires looping voices cleanly), `StopAllVoices`, and `RequestScopeRelease` / `PollReleasableScopeMask`.