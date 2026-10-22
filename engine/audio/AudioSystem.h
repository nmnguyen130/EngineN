#pragma once

#include "audio/AudioMixer.h"
#include "core/Memory.h"

namespace en {

class AssetBank;

// 32-byte (2 per 64B cache line, zero padding) main-thread tracker for an active polyphonic voice slot.
struct alignas(32) MainVoiceTracker {
    SoundCueId cueId{};
    u32 emitterId{kNullAudioEmitter};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    i16 emitterVx_q4{0};
    i16 emitterVy_q4{0};
    u16 instanceGain_q14{16384U};
    u16 instancePitch_q12{4096U};
    u16 spawnTick{0U};
    u16 generation{0U};
    u8 cueTableSlot{0U};
    u8 assetScope{0U};
    u8 priority{0U};
    u8 occluded{0U};
};
static_assert(sizeof(MainVoiceTracker) == 32);

// 128-byte (2 cache lines exact, zero padding) 2D spatial audio coordinator, voice admission pipeline, and DSP controller.
// Move-only RAII subsystem owning a single AlignedArena slab; zero cross-subsystem header coupling.
// Invariant: sizeof(AudioSystem) == 128, Cache Line 0 holds the 64B AudioMixer, and PollReleasableScopeMask() returns scopes with zero active voices/stems.
class alignas(64) AudioSystem {
public:
    AudioSystem() noexcept = default;
    ~AudioSystem() noexcept { Shutdown(); }

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    AudioSystem(AudioSystem&& other) noexcept;
    AudioSystem& operator=(AudioSystem&& other) noexcept;

    [[nodiscard]] Result<void, AudioErrc> Init(const AudioSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterCueDef(
        const SoundCueDef& def,
        u8 owningScope = 0U) noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterMusicTrackDef(
        const MusicTrackDef& def,
        u8 owningScope = 0U) noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterCueBank(
        const BlobRecord& cueBlob,
        u8 owningScope = 0U) noexcept;

    void RequestScopeRelease(u8 scope) noexcept {
        if (scope < 32U) m_pendingReleaseScopeMask |= (1U << scope);
    }

    [[nodiscard]] u32 PollReleasableScopeMask() noexcept;

    constexpr void SetFocusMuted(bool muted) noexcept { m_focusMuted = muted ? 1U : 0U; }
    [[nodiscard]] constexpr bool IsFocusMuted() const noexcept { return m_focusMuted != 0U; }

    void SubmitTriggers(
        std::span<const AudioTriggerReq> triggers,
        const AssetBank& assetBank) noexcept;

    VoiceHandle PlayCue(
        SoundCueId cueId,
        const AssetBank& assetBank,
        f32 worldX = 0.0f,
        f32 worldY = 0.0f,
        u32 emitterId = kNullAudioEmitter,
        f32 emitterVx = 0.0f,
        f32 emitterVy = 0.0f) noexcept;

    void StopVoice(VoiceHandle handle, u16 fadeOutBlocks = 1U) noexcept;
    void StopAllVoices(u16 fadeOutBlocks = 1U) noexcept;

    void SetVoicePosition(
        VoiceHandle handle,
        f32 worldX,
        f32 worldY,
        f32 emitterVx = 0.0f,
        f32 emitterVy = 0.0f) noexcept;

    void UpdateEmitterTransform(
        u32 emitterId,
        f32 worldX,
        f32 worldY,
        f32 emitterVx = 0.0f,
        f32 emitterVy = 0.0f) noexcept;

    void SetVoiceGain(VoiceHandle handle, f32 linearGain) noexcept;
    void SetVoicePitch(VoiceHandle handle, f32 pitchRatio) noexcept;

    void SetBusGain(AudioBusId bus, f32 linearGain) noexcept;
    void SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept;

    void SetReverbParams(
        f32 roomSize,
        f32 damping,
        f32 wetGain,
        u8 busSendMask = 0U) noexcept;

    [[nodiscard]] bool TransitionMusic(
        u32 trackId,
        const AssetBank& assetBank,
        u8 crossfadeBlocks = 48U) noexcept;

    void SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept;

    void Update(const AudioUpdateDesc& desc) noexcept;

    void PumpHeadlessBlocks(
        u32 blockCount,
        std::span<f32> optionalCaptureOut = {}) noexcept;

    [[nodiscard]] u32 ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept;

    [[nodiscard]] constexpr const AudioTelemetrySnapshot& Telemetry() const noexcept {
        return m_mixer.Telemetry();
    }

    [[nodiscard]] constexpr const AudioMixer& Mixer() const noexcept { return m_mixer; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    struct AudioBridgeState;

    void EnqueueCommand(const AudioCommand& cmd) noexcept;
    void DrainCommandsToMixer() noexcept;
    void ComputeSpatialVoiceParams(
        u8 slot,
        const SoundCueDef& def,
        f32 listenerX,
        f32 listenerY,
        f32 listenerVx,
        f32 listenerVy,
        f32 dopplerScale,
        u16& outGainL_q14,
        u16& outGainR_q14,
        u16& outPitch_q12) const noexcept;

    // Cache Line 0 (64 bytes): polyphonic SoA DSP mixer
    AudioMixer m_mixer{};

    // Cache Line 1 (64 bytes): arena, bridge pointer, active masks, and listener state
    AlignedArena m_arena{};                 // 32B
    AudioBridgeState* m_bridge{nullptr};    // 8B
    u64 m_spatialActiveMask{0ULL};          // 8B
    f32 m_listenerX{0.0f};                  // 4B
    f32 m_listenerY{0.0f};                  // 4B
    u32 m_pendingReleaseScopeMask{0U};      // 4B
    u16 m_currentTick{0U};                  // 2B
    u8 m_focusMuted{0U};                    // 1B
    u8 m_initialized{0U};                   // 1B
};
static_assert(sizeof(AudioSystem) == 128);

} // namespace en
