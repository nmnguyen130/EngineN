#pragma once

#include "audio/AudioMixer.h"
#include "core/Memory.h"

namespace en {

class AssetBank;

// 2D spatial audio coordinator, voice admission pipeline, and DSP controller.
// Non-copyable, non-movable owner of the audio arena, SDL audio stream, and double-buffered snapshot bridge.
// Invariant: sim-thread state and audio-thread mixer state communicate only via atomic SPSC ring and double-buffered snapshots.
class alignas(64) AudioSystem {
public:
    AudioSystem() noexcept = default;
    ~AudioSystem() noexcept { Shutdown(); }

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;

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

    constexpr void RequestScopeRelease(u8 scope) noexcept {
        if (scope < 32U) m_pendingReleaseScopeMask |= (1U << scope);
    }

    [[nodiscard]] u32 PollReleasableScopeMask() noexcept;

    void SetFocusMuted(bool muted) noexcept;
    [[nodiscard]] constexpr bool IsFocusMuted() const noexcept { return m_focusMuted; }

    [[nodiscard]] bool HasAudioDeviceStream() const noexcept;
    void PullDeviceStreamBytes(i32 additionalBytes) noexcept;

    void SubmitTriggers(
        std::span<const AudioTriggerReq> triggers,
        const AssetBank& assetBank) noexcept;

    VoiceHandle PlayCue(
        SoundCueId cueId,
        const AssetBank& assetBank,
        f32 worldX = 0.0f,
        f32 worldY = 0.0f,
        u32 emitterId = kNullAudioEmitter) noexcept;

    void StopVoice(VoiceHandle handle, u16 fadeOutBlocks = 1U) noexcept;
    void StopAllVoices(u16 fadeOutBlocks = 1U) noexcept;

    void SetVoicePosition(VoiceHandle handle, f32 worldX, f32 worldY) noexcept;
    void UpdateEmitterTransform(u32 emitterId, f32 worldX, f32 worldY) noexcept;
    void SetVoiceGain(VoiceHandle handle, f32 linearGain) noexcept;
    void SetVoicePitch(VoiceHandle handle, f32 pitchRatio) noexcept;

    void SetBusGain(AudioBusId bus, f32 linearGain) noexcept;
    void SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept;

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
    [[nodiscard]] constexpr const AudioTelemetrySnapshot& Telemetry() const noexcept { return m_mixer.Telemetry(); }
    [[nodiscard]] constexpr const AudioMixer& Mixer() const noexcept { return m_mixer; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    struct AudioBridgeState;

    void EnqueueCommand(const AudioCommand& cmd) noexcept;
    void PublishSnapshot() noexcept;
    void SyncFinishedVoicesFromMixer() noexcept;
    void RunMixerBlocks(u32 blockCount, std::span<f32> optionalCaptureOut, bool pushToDevice) noexcept;
    void ComputeSpatialVoiceParams(
        f32 instanceGain,
        f32 worldX,
        f32 worldY,
        const SoundCueDef& def,
        f32 listenerX,
        f32 listenerY,
        f32& outGainL,
        f32& outGainR) const noexcept;

    AudioMixer m_mixer{};
    AlignedArena m_arena{};
    AudioBridgeState* m_bridge{nullptr};
    u64 m_spatialActiveMask{0ULL};
    f32 m_listenerX{0.0f};
    f32 m_listenerY{0.0f};
    u32 m_pendingReleaseScopeMask{0U};
    u16 m_currentTick{0U};
    bool m_focusMuted{false};
    bool m_snapshotDirty{false};
};
static_assert(sizeof(AudioSystem) == 128);

} // namespace en
