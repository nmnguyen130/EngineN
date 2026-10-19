# EngineN — Audio Subsystem Context (`audio/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()` (single `64 KB` 64-byte aligned arena slab), zero virtual dispatch, locked 5-file footprint (`AudioTypes.h`, `AudioMixer.h/.cpp`, `AudioSystem.h/.cpp`) + `tests/AudioTests.cpp`, two-layer pure C++23 separation (`namespace en::audio`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Header Pollution:**
  - **Layer 1 (`AudioTypes.h`, `AudioMixer.h/.cpp`):** Pure C++23 1-cache-line (`sizeof(AudioMixer) == 64`, `alignas(64)`) data-oriented polyphonic mixer (`kSampleRate = 48000`, `kMixBlockFrames = 256`, `kMaxVoices = 64`), L1-resident single-base-pointer `MixerArenaState` (`sizeof == 20352`, `19.8 KB`: `VoicePoolSoA` `3648 B`, 1-cache-line `MusicDeckRuntime` `64 B`, 3-cache-line `BusDspState` `192 B`, and `16 KB` planar bus scratch), 3-stage decoupled SIMD block voice pipe (Pass A branchless interior 4-point Hermite resampling -> Pass B optional full-range Cytomic SVF -> Pass C stereo gain ramp FMA), direct in-place stolen-voice bus tail accumulation (`AccumulateStolenVoiceTail`), lazy dirty-bus clearing (`EnsureBusInitialized`), combat-to-music sidechain ducking, Padé `[3/2]` $\tanh$ master soft-clipper, and 4-stem sample-locked interactive music decks. Depends solely on `"core/Result.h"`, `"core/Types.h"`, `"core/time/TimeTypes.h"`, and `"assets/AssetTypes.h"` — zero `world/`, `gameplay/`, `platform/`, or `<SDL3/SDL_*.h>` dependencies; 100% deterministic in headless CI via `AudioMixer::ComputeAuditHash()`.
  - **Layer 2 (`AudioSystem.h/.cpp`):** 2-cache-line (`sizeof(AudioSystem) == 128`, `alignas(64)`) move-only RAII coordinator owning the single `64 KB` arena slab (`m_bridge` at offset `0`) and `SDL_AudioStream` hardware binding (`SDL_OpenAudioDeviceStream`), flat 1024-slot Fibonacci Robin Hood `SoundCueDef` index with co-located 8-byte `CueRuntimeMeta`, all-in-one 32-byte `MainVoiceTracker` (`2 per cache line`, zero side arrays), single-pass bitmask voice admission pipeline (tick-0-safe cooldown/energy-summing coalescer, per-cue concurrency cap, entity voice replacement, and single-command `StealOldest`/`StealQuietest` audibility stealing), `m_spatialActiveMask`-filtered 30 Hz staggered raycast occlusion via `AudioOcclusionQueryFn`, and bitmask scope retirement (`RequestScopeRelease` / `PollReleasableScopeMask`) covering both active voices and active music decks. `<SDL3/SDL_audio.h>` is strictly confined to `AudioSystem.cpp`.
- **Direct In-Place Steal Tail Accumulation (`0 B` Steal Pool Overhead):**
  - Eliminates the legacy `16.5 KB` `StealSlotState` buffer, `MixStealPoolPlanar()` pass, `m_nextStealPoolIdx`, and duplicate `StopVoice` command before `SpawnVoice`. When `ApplyCommand(SpawnVoice)` overwrites an active voice bit, `AccumulateStolenVoiceTail(slot)` mixes a 256-sample linear fade-out of the old voice directly into its target bus scratch plane (`m_pendingStealBusMask`), halving SPSC command traffic on voice steals and shrinking `MixerArenaState` to `19.8 KB` (fitting 100% inside a 32 KB L1D cache).
- **Hybrid Lock-Free Thread Synchronization (Triple-Buffer + Discrete SPSC Ring + `freshSpawnMask`):**
  - **Continuous State (`AudioFrameSnapshot`, `576 B` = 9 cache lines):** Pre-computed on the main thread inside `AudioSystem::Update()` and published via a wait-free 3-slot atomic Triple Buffer (`tripleFlags`). Non-spatial 2D voices compute their targets once at `PlayCue()`, while `Update()` iterates strictly over `m_spatialActiveMask` for distance attenuation, constant-power panning (`m_invPanWidth`), and occlusion filtering.
  - **Discrete Lifecycle Edges (`AudioCommand`, `32 B` exact):** A 256-slot cache-isolated SPSC ring buffer (`cmdRing`) carries only discrete state transitions (`SpawnVoice`, `StopVoice`, `BindMusicStem`, `CommitMusicDeck`, `PanicReset`). `VoicePoolSoA::freshSpawnMask` latches spawn gains and pitch until `snapshotSeq` advances, preventing first-block volume dips when `SpawnVoice` is drained on the audio thread prior to the next `Update()` snapshot.

## 2. Core Types (`audio/AudioTypes.h`)
- **Primitives, Enums & Bitmasks:**
  - `enum class AudioErrc : u8`: `InvalidConfig`, `ArenaExhausted`, `DeviceOpenFailed`, `CorruptedBankBlob`, `CueTableFull`, `VoicePoolExhausted`.
  - `enum class AudioBusId : u8`: `Master = 0`, `Music`, `SfxPlayer`, `SfxCombat`, `SfxWorld`, `Ambience`, `Ui`, `Voice`, `Count` (`kAudioBusCount = 8`).
  - `enum class FilterMode : u8`: `Bypass = 0`, `LowPass`, `HighPass`, `BandPass`.
  - `enum class AttenuationCurve : u8`: `None = 0`, `Linear`, `InverseClamped`, `Smoothstep`.
  - `enum class StealPolicy : u8`: `Reject = 0`, `StealOldest`, `StealQuietest`.
  - `enum class VoiceFlags : u8`: `None`, `Looping`, `Spatialized2D`, `FollowTimeScale`, `PauseOnHitstop`, `BypassOcclusion`, `OneVoicePerEntity`. Queried via `constexpr bool HasVoiceFlag(VoiceFlags val, VoiceFlags mask) noexcept`.
- **Identifiers, Bridges & Cooked Binary Formats (`Zero Dummy Padding`):**
  - `struct alignas(4) VoiceHandle { u16 index{0}; u16 generation{0}; };` (`sizeof == 4`).
  - `struct alignas(4) SoundCueId { u32 value{0}; };` (`sizeof == 4`, `SoundCueId::FromAssetId(assets::AssetId)`).
  - `struct alignas(16) AudioTriggerReq` (`sizeof == 16`): `cueId`, `worldX`, `worldY`, `emitterId`.
  - `struct alignas(16) AudioEmitterView` (`sizeof == 16`): `worldX`, `worldY`, `maxDist`, `occlusionFactor`.
  - `struct AudioUpdateDesc`: `tickIndex`, `listenerX`, `listenerY`, `channelScales`, `triggers`, `assetBank`, `occlusionQuery`, `occlusionUserCtx`, `frozenChannelMask`, `focusMuteOverride`.
  - `struct alignas(16) CookedAudioBankHeader` (`sizeof == 16`, magic `'EAUD'`).
  - `struct alignas(32) SoundCueDef` (`sizeof == 32`, 2 cues per 64B cache line).
  - `struct alignas(32) MusicTrackDef` (`sizeof == 32`, 2 tracks per 64B cache line).
  - `struct alignas(32) AudioCommand` (`sizeof == 32`): `type` (`AudioCmdType`), `packedBusAndChannel`, `voiceSlot`, `generation`, `flags`, `assetScopeId`, `payload` (`24 B` union: `Spawn`, `Stop`, `MusicStem`).
  - `struct alignas(64) AudioFrameSnapshot` (`sizeof == 576`, 9 cache lines).
  - `struct alignas(32) AudioTelemetrySnapshot` (`sizeof == 32`).

## 3. Public Interfaces & Signatures
### `AudioMixer` (`audio/AudioMixer.h` — 64B 1-Cache-Line Pure C++23 SoA Mixer & DSP Engine)
- `[[nodiscard]] static constexpr usize RequiredArenaBytes() noexcept`
- `[[nodiscard]] Result<void, AudioErrc> Init(std::span<u8> arenaSlice) noexcept`
- `void Reset() noexcept`
- `void ApplyCommand(const AudioCommand& cmd) noexcept`
- `void MixBlock(const AudioFrameSnapshot& snapshot, std::span<f32, kMixBlockFrames * 2> outInterleavedStereo) noexcept`
- `[[nodiscard]] constexpr const AudioTelemetrySnapshot& Telemetry() const noexcept`
- `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `AudioSystem` (`audio/AudioSystem.h` — 128B 2-Cache-Line Move-Only RAII Coordinator & SDL3 Stream Adapter)
- Lifecycle & Bank Registration:
  - `[[nodiscard]] Result<void, AudioErrc> Init(const AudioSystemConfig& cfg = {}) noexcept` (`cfg.arenaBytes = 64 * 1024`)
  - `void Shutdown() noexcept`
  - `[[nodiscard]] Result<void, AudioErrc> RegisterCueDef(const SoundCueDef& def, assets::ScopeId owningScope = 0) noexcept`
  - `[[nodiscard]] Result<void, AudioErrc> RegisterMusicTrackDef(const MusicTrackDef& def, assets::ScopeId owningScope = 0) noexcept`
  - `[[nodiscard]] Result<void, AudioErrc> RegisterCueBank(const assets::BlobRecord& cueBlob, assets::ScopeId owningScope) noexcept`
  - `void RequestScopeRelease(assets::ScopeId scope) noexcept`, `[[nodiscard]] u32 PollReleasableScopeMask() noexcept`
  - `void SetFocusMuted(bool muted) noexcept`
- Playback, Routing & Consolidated Frame Synchronization:
  - `void SubmitTriggers(std::span<const AudioTriggerReq> triggers, const assets::AssetBank& assetBank) noexcept`
  - `VoiceHandle PlayCue(SoundCueId cueId, const assets::AssetBank& assetBank, f32 worldX = 0.0f, f32 worldY = 0.0f, u32 emitterId = kNullEmitter) noexcept`
  - `void StopVoice(VoiceHandle handle, u16 fadeOutBlocks = 1) noexcept`
  - `void SetBusGain(AudioBusId bus, f32 linearGain) noexcept`, `void SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept`
  - `void TransitionMusic(u32 trackId, const assets::AssetBank& assetBank, u8 crossfadeBlocks = 94) noexcept`
  - `void SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept`
  - `void Update(const AudioUpdateDesc& desc) noexcept`
  - `void PumpHeadlessBlocks(u32 blockCount, std::span<f32> optionalCaptureOut = {}) noexcept`
  - `[[nodiscard]] u32 ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept`
  - `[[nodiscard]] AudioTelemetrySnapshot Telemetry() const noexcept`, `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

## 4. Canonical Main Loop Usage
```cpp
for (u32 relMask = m_audio.PollReleasableScopeMask(); relMask != 0u; relMask &= (relMask - 1u)) {
    m_assets.EndScope(static_cast<en::assets::ScopeId>(std::countr_zero(relMask)));
}

m_audio.Update({
    .tickIndex         = m_time.GetRenderView().tickIndex,
    .listenerX         = camX,
    .listenerY         = camY,
    .channelScales     = m_time.EffectiveScales(),
    .triggers          = audioTriggerBatch,
    .assetBank         = &m_assets.Bank(),
    .occlusionQuery    = &QuerySpatialOcclusion,
    .occlusionUserCtx  = &m_gameplay.Spatial(),
    .frozenChannelMask = m_time.FrozenMask(),
    .focusMuteOverride = m_window.State().focused ? i8(0) : i8(1)
});
```