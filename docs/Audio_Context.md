# EngineN — Audio Subsystem Context (`audio/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()` (single `192 KB` 64-byte aligned arena slab), zero virtual dispatch, minimal 5-file footprint (`AudioTypes.h`, `AudioMixer.h/.cpp`, `AudioSystem.h/.cpp`), two-layer pure C++23 separation (`namespace en::audio`).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`AudioTypes.h`, `AudioMixer.h/.cpp`):** Pure C++23 data-oriented Structure-of-Arrays (`VoicePoolSoA`) 64-voice polyphonic mixer (`kSampleRate = 48000`, `kMixBlockFrames = 256`), Q32.32 fixed-point phase accumulator with 4-point 3rd-order Hermite vari-speed resampling and pre-cached 4-sample loop guards, Cytomic Linear Trap-Integrated State Variable Filter (SVF), 16-slot steal crossfade pool (`StealSlotState`), planar SIMD bus accumulation (`kAudioBusCount = 8`), combat-to-music sidechain ducking, Padé `[3/2]` $\tanh$ master soft-clipper, and 4-stem sample-locked interactive music decks. Zero OS or `<SDL3/SDL_*.h>` dependencies; 100% deterministic in headless CI via `AudioMixer::ComputeAuditHash()`.
  - **Layer 2 (`AudioSystem.h/.cpp`):** Move-only RAII coordinator owning the `SDL_AudioStream` hardware binding (`SDL_OpenAudioDeviceStream`), flat 1024-slot Fibonacci Robin Hood `SoundCueDef` index, 3-stage voice admission pipeline (cooldown/energy-summing coalescer, per-cue concurrency cap, priority/audibility stealing), 30 Hz staggered `gameplay::SpatialIndex2D::Raycast()` tile occlusion, `core::time::TimeChannel` hitstop/timescale synchronization, and `assets::ScopeId` bitmask retirement protection. `<SDL3/SDL_audio.h>` is strictly confined to `AudioSystem.cpp`.
- **Hybrid Lock-Free Thread Synchronization (Triple-Buffer + Discrete SPSC Ring):**
  - **Continuous State (`AudioFrameSnapshot`, `576 B` = 9 cache lines):** All per-frame continuous parameters (64 voice Q14 L/R gains, Q12 pitch ratios, pre-warped Q14 SVF cutoffs, 8 bus gains/cutoffs, 8 `TimeChannel` time scales, `frozenChannelMask`, and 4 music stem weights) are pre-computed on the main thread inside `AudioSystem::Update()` and published via a wait-free 3-slot atomic Triple Buffer (`tripleFlags`). Eliminates per-frame positional/filter command queue spam and hoists all transcendental math (`std::sqrt`, `std::cos`, `std::sin`, Padé $\tan$) out of the real-time audio callback.
  - **Discrete Lifecycle Edges (`AudioCommand`, `32 B` exact):** A 256-slot cache-isolated SPSC ring buffer (`cmdRing`) carries only discrete state transitions (`SpawnVoice`, `StopVoice`, `BindMusicStem`, `CommitMusicDeck`, `TriggerStinger`, `PanicReset`).
- **Zero-Queue Bitmask Scope Retirement (`activeScopePinMask`):**
  - Each active physical voice records its `assets::ScopeId` (`0..31`). At the end of `AudioMixer::MixBlock()`, active scope bits are OR-reduced into `AudioTelemetrySnapshot::activeScopePinMask` (`u32`). When unloading a room bank via `AudioSystem::RequestScopeRelease()`, `AssetSystem::EndScope()` is invoked immediately if the scope bit is `0`, or deferred cleanly until active fade-outs complete.

## 2. Core Types (`audio/AudioTypes.h`)
- **Primitives, Enums & Bitmasks:**
  - `enum class AudioErrc : u8`: `InvalidConfig`, `ArenaExhausted`, `DeviceOpenFailed`, `CorruptedBankBlob`, `CueTableFull`, `VoicePoolExhausted`.
  - `enum class AudioBusId : u8`: `Master = 0`, `Music`, `SfxPlayer`, `SfxCombat`, `SfxWorld`, `Ambience`, `Ui`, `Voice`, `Count` (`kAudioBusCount = 8`).
  - `enum class FilterMode : u8`: `Bypass = 0`, `LowPass`, `HighPass`, `BandPass`.
  - `enum class AttenuationCurve : u8`: `None = 0`, `Linear`, `InverseClamped`, `Smoothstep`.
  - `enum class StealPolicy : u8`: `Reject = 0`, `StealOldest`, `StealQuietest`, `Virtualize`.
  - `enum class VoiceFlags : u8`: `None`, `Looping`, `Spatialized2D`, `FollowTimeScale`, `PauseOnHitstop`, `BypassOcclusion`, `OneVoicePerEntity`. Queried via `constexpr bool HasVoiceFlag(VoiceFlags val, VoiceFlags mask) noexcept`.
- **Identifiers & Generational Handles:**
  - `struct VoiceHandle { u16 index{0}; u16 generation{0}; };` (`sizeof == 4`).
  - `struct SoundCueId { u32 value{0}; };` — Supports lossless 32-bit construction and 64-bit `assets::AssetId` XOR-folding via `SoundCueId::FromAssetId(assets::AssetId)`.
- **Cooked Binary Formats (`'EAUD'` Blobs) & Synchronization Records (Zero Padding):**
  - `struct alignas(16) CookedAudioBankHeader` (`sizeof == 16`, magic `'EAUD'`).
  - `struct alignas(32) SoundCueDef` (`sizeof == 32`, 2 cues per 64B cache line): `cueId`, `variations[4]` (`assets::AudioHandle`), `baseGainQ14`, `basePitchCents`, `gainVarQ8`, `pitchVarSemitonesQ4`, `minDistQ2`, `maxDistQ1`, `packedRouting`, `packedChannelAndCurve`, `priority`, `packedFlagsAndCooldown`.
  - `struct alignas(32) MusicTrackDef` (`sizeof == 32`): `trackId`, `stems[4]` (`assets::AudioHandle`), `loopStartFrame`, `loopEndFrame`, `tempoBpmQ8`, `packedStemsAndBeats`, `defaultCrossfadeBlocks`.
  - `struct alignas(32) AudioCommand` (`sizeof == 32`): `type` (`AudioCmdType`), `packedBusAndChannel`, `voiceSlot`, `generation`, `flags`, `assetScopeId`, `payload` (`24 B` union: `Spawn`, `Stop`, `MusicStem`).
  - `struct alignas(64) AudioFrameSnapshot` (`sizeof == 576`, 9 cache lines): `targetGainL_Q14[64]`, `targetGainR_Q14[64]`, `targetPitchQ12[64]`, `targetSvfG_Q14[64]`, `busTargetGainQ14[8]`, `busSvfG_Q14[8]`, `channelTimeScaleQ12[8]`, `stemWeightsQ14[4]`, `busFilterModesBits`, `frozenChannelMask`, `masterMuteFlag`, `snapshotSeq`.
  - `struct alignas(32) AudioTelemetrySnapshot` (`sizeof == 32`): `activeVoiceMask` (`u64`), `activeScopePinMask` (`u32`), `stolenVoicesTotal`, `masterPeakL`, `masterPeakR`, `callbackCpuLoadRatio`, `activeVirtualVoices`, `underrunCount`.

## 3. Public Interfaces & Signatures
### `AudioMixer` (`audio/AudioMixer.h` — Pure C++23 SoA Mixer & DSP Engine)
- Lifecycle & Sizing:
  - `[[nodiscard]] static constexpr usize RequiredArenaBytes() noexcept`
  - `[[nodiscard]] std::expected<void, AudioErrc> Init(std::span<u8> arenaSlice) noexcept`
  - `void Reset() noexcept`
- Block Mixing & Determinism Audit:
  - `void ApplyCommand(const AudioCommand& cmd) noexcept`
  - `void MixBlock(const AudioFrameSnapshot& snapshot, std::span<f32, kMixBlockFrames * 2> outInterleavedStereo) noexcept`
  - `[[nodiscard]] constexpr const AudioTelemetrySnapshot& Telemetry() const noexcept`
  - `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `AudioSystem` (`audio/AudioSystem.h` — Move-Only RAII Coordinator & SDL3 Stream Adapter)
- Lifecycle & Bank Registration:
  - `[[nodiscard]] std::expected<void, AudioErrc> Init(const AudioSystemConfig& cfg = {}) noexcept`
  - `void Shutdown() noexcept`
  - `[[nodiscard]] std::expected<void, AudioErrc> RegisterCueDef(const SoundCueDef& def, assets::ScopeId owningScope = 0) noexcept`
  - `[[nodiscard]] std::expected<void, AudioErrc> RegisterMusicTrackDef(const MusicTrackDef& def) noexcept`
  - `[[nodiscard]] std::expected<void, AudioErrc> RegisterCueBank(const assets::BlobRecord& cueBlob, assets::ScopeId owningScope) noexcept`
  - `void RequestScopeRelease(assets::ScopeId scope, assets::AssetSystem& assetSystem) noexcept`
  - `void OnWindowEvent(platform::WindowFlags flags, const platform::WindowState& winState) noexcept`
- Playback, Routing & Frame Synchronization:
  - `void SetListenerPosition(f32 worldX, f32 worldY) noexcept`
  - `void ConsumeSimCues(std::span<const gameplay::SimCue> cues, const world::World& world, const assets::AssetBank& assetBank) noexcept` (filters `SimCueKind::AudioOneShot` and resolves `SoundCueId::FromAssetId(cue.assetId)` at `(cue.worldX, cue.worldY)`)
  - `VoiceHandle PlayCue(SoundCueId cueId, const assets::AssetBank& assetBank, f32 worldX = 0.0f, f32 worldY = 0.0f, world::EntityId emitter = world::kNullEntity) noexcept`
  - `void StopVoice(VoiceHandle handle, u16 fadeOutBlocks = 1) noexcept`
  - `void SetBusGain(AudioBusId bus, f32 linearGain) noexcept`, `void SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept`
  - `void TransitionMusic(u32 trackId, const assets::AssetBank& assetBank, u8 crossfadeBlocks = 94) noexcept`
  - `void SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept`
  - `void Update(const world::World& world, const gameplay::SpatialIndex2D& spatial, const assets::AssetBank& assetBank, core::time::RenderTimeView timeView, u8 frozenChannelMask, std::span<const f32, 8> channelScales) noexcept`
  - `void PumpHeadlessBlocks(u32 blockCount, std::span<f32> optionalCaptureOut = {}) noexcept`
  - `[[nodiscard]] AudioTelemetrySnapshot Telemetry() const noexcept`, `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

## 4. Canonical Main Loop Usage
```cpp
const auto winChanges = m_platform.Pump(m_window, m_input, m_devToolsTap);
m_audio.OnWindowEvent(winChanges, m_window.State());
m_assets.Pump(m_renderer);

u8 frozenMask = 0;
std::array<f32, 8> channelScales{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const auto tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(en::core::time::TimeChannel::Player));

    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
    m_world.EndTick(step);

    for (u8 ch = 0; ch < 8; ++ch) {
        if (step.IsFrozen(ch)) {
            frozenMask |= static_cast<u8>(1u << ch);
        }
        channelScales[ch] = step.EffectiveScale(ch);
    }
}

m_audio.ConsumeSimCues(m_gameplay.DrainCues(), m_world, m_assets.Bank());
m_audio.Update(m_world, m_gameplay.Spatial(), m_assets.Bank(), m_time.GetRenderView(), frozenMask, channelScales);