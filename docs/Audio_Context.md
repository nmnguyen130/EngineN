# EngineN — Audio Subsystem Context (`engine/audio/`)

## 1. Architecture & Invariants
- **Core:** C++23 (`-fno-exceptions`, `-fno-rtti`), zero heap allocation after `Init()` (single 192 KB 64B-aligned arena slab), zero virtual dispatch, 5-file footprint (`AudioTypes.h`, `AudioMixer.h/.cpp`, `AudioSystem.h/.cpp`), decoupled pure C++ design (zero OS, SDL3, World, or Gameplay dependencies in headers).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`AudioMixer`):** Structure-of-Arrays (`VoicePoolSoA`) 64-voice mixer (`kSampleRate = 48000`, `kMixBlockFrames = 256`), Q32.32 phase accumulator with 4-point 3rd-order Hermite resampling, Cytomic SVF filters, 16-slot steal crossfade pool, planar SIMD bus accumulation (8 buses), combat sidechain ducking, Padé [3/2] $\tanh$ soft-clipper, and 4-stem interactive music decks. 100% testable headless via `ComputeAuditHash()`.
  - **Layer 2 (`AudioSystem`):** Move-only RAII coordinator owning `SDL_AudioStream`, flat 1024-slot Robin Hood `SoundCueDef` index, 3-stage voice admission (cooldown/energy coalescing, concurrency cap, priority stealing), 30 Hz staggered raycast occlusion callback, timescale synchronization, and bitmask scope retirement. `<SDL3/SDL_audio.h>` is strictly confined to `AudioSystem.cpp`.
- **Hybrid Lock-Free Thread Synchronization:**
  - **Continuous State (`AudioFrameSnapshot`, 576B):** Precomputed on main thread in `Update()` (Q14 L/R gains, Q12 pitches, Q14 SVF cutoffs, bus gains, stem weights) and published via wait-free 3-slot atomic Triple Buffer (`tripleFlags`), hoisting all transcendental math out of the audio thread.
  - **Discrete Edges (`AudioCommand`, 32B):** 256-slot cache-isolated SPSC ring buffer (`cmdRing`) transfers lifecycle commands (`SpawnVoice`, `StopVoice`, `BindMusicStem`, etc.).
- **Zero-Queue Bitmask Scope Retirement:** Voices record `assets::ScopeId` (0..31). Active scope bits are OR-reduced in `AudioTelemetrySnapshot`. Releasable scopes are polled via `PollReleasableScopeMask()`.

## 2. Core Layouts & Types (`AudioTypes.h`)
- `AudioErrc`: `InvalidConfig`, `ArenaExhausted`, `DeviceOpenFailed`, `CorruptedBankBlob`, `CueTableFull`, `VoicePoolExhausted`.
- `AudioBusId`: `Master = 0`, `Music`, `SfxPlayer`, `SfxCombat`, `SfxWorld`, `Ambience`, `Ui`, `Voice` (`kAudioBusCount = 8`).
- `VoiceHandle`: `{u16 index, u16 generation}` (4B).
- `SoundCueId`: 32-bit ID constructed directly or via `SoundCueId::FromAssetId(AssetId)`.
- `SoundCueDef` (`alignas(32)`, 32B): `cueId`, `variations[4]`, `baseGainQ14`, `basePitchCents`, `gainVarQ8`, `pitchVarSemitonesQ4`, `minDistQ2`, `maxDistQ1`, `packedRouting`, `packedChannelAndCurve`, `priority`, `packedFlagsAndCooldown`.
- `MusicTrackDef` (`alignas(32)`, 32B): `trackId`, `stems[4]`, `loopStartFrame`, `loopEndFrame`, `tempoBpmQ8`, `packedStemsAndBeats`, `defaultCrossfadeBlocks`.
- `AudioTriggerReq` (24B): Input trigger struct `{cueId, worldX, worldY, emitterId}`.
- `AudioUpdateDesc`: `{tickIndex, listenerX, listenerY, channelScales, frozenChannelMask, occlusionQuery, occlusionUserCtx}`.
- `AudioFrameSnapshot` (`alignas(64)`, 576B): Triple-buffered continuous mix frame.
- `AudioTelemetrySnapshot` (`alignas(32)`, 32B): `activeVoiceMask`, `activeScopePinMask`, `masterPeakL/R`, `callbackCpuLoadRatio`.

## 3. Subsystem APIs
### `AudioMixer` (`engine/audio/AudioMixer.h` — SoA Mixer & DSP Engine)
- `Init(span<u8> arenaSlice)` $\to$ `Result<void, AudioErrc>`, `Reset()`
- `ApplyCommand(AudioCommand)`, `MixBlock(AudioFrameSnapshot, span<f32, 512> outStereo)`
- `Telemetry()` $\to$ `const AudioTelemetrySnapshot&`, `ComputeAuditHash()` $\to$ `u64`.

### `AudioSystem` (`engine/audio/AudioSystem.h` — Hardware Coordinator & Mixer Host)
- **Lifecycle & Registrations:**
  - `Init(AudioSystemConfig)` $\to$ `Result<void, AudioErrc>`, `Shutdown()`
  - `RegisterCueDef(SoundCueDef, ScopeId)` $\to$ `Result<void, AudioErrc>`
  - `RegisterMusicTrackDef(MusicTrackDef)` $\to$ `Result<void, AudioErrc>`
  - `RegisterCueBank(BlobRecord, ScopeId)` $\to$ `Result<void, AudioErrc>`
  - `RequestScopeRelease(ScopeId)`, `PollReleasableScopeMask()` $\to$ `u32`
  - `SetFocusMuted(bool muted)`
- **Playback & Control:**
  - `PlayCue(cueId, assetBank, worldX, worldY, emitterId)` $\to$ `VoiceHandle`
  - `SubmitTriggers(span<const AudioTriggerReq>, assetBank)`: Bulk ingestion of cues.
  - `StopVoice(handle, fadeOutBlocks)`, `SetBusGain(bus, linearGain)`, `SetBusFilter(bus, mode, cutoffHz)`
  - `TransitionMusic(trackId, assetBank, crossfadeBlocks)`, `SetMusicStemWeights(span<const f32, 4>)`
  - `Update(AudioUpdateDesc)`: Staggered raycast occlusion & triple-buffer publish.
  - `ExtractActiveEmitters(span<AudioEmitterView>)` $\to$ `u32` (for debug visualization).
  - `Telemetry()` $\to$ `AudioTelemetrySnapshot`, `ComputeAuditHash()` $\to$ `u64`.

## 4. Main Loop Integration
```cpp
// 1. Drain gameplay cues and submit to audio
std::array<audio::AudioTriggerReq, 64> triggers{};
usize triggerCount = 0;
for (const auto& cue : m_gameplay.DrainCues()) {
    if (cue.kind == gameplay::SimCueKind::AudioOneShot && triggerCount < triggers.size()) {
        triggers[triggerCount++] = {
            .cueId = audio::SoundCueId::FromAssetId(cue.assetId),
            .worldX = cue.worldX,
            .worldY = cue.worldY,
            .emitterId = cue.emitter.index
        };
    }
}
m_audio.SubmitTriggers({triggers.data(), triggerCount}, m_assets.Bank());

// 2. Update audio listener & spatial occlusion
m_audio.Update({
    .tickIndex = step.TickIndex(),
    .listenerX = m_camera.worldCenterX,
    .listenerY = m_camera.worldCenterY,
    .channelScales = m_time.ChannelScales(),
    .frozenChannelMask = m_time.FrozenMask()
});
```