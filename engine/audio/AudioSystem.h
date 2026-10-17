#pragma once

#include "audio/AudioMixer.h"
#include "audio/AudioTypes.h"

#include <span>

struct SDL_AudioStream;

namespace en::assets { class AssetBank; }

namespace en::audio {

struct AudioSystemConfig {
    u32 arenaBytes{192 * 1024};
    f32 initialMasterGain{1.0f};
    f32 initialMusicGain{0.8f};
    f32 initialSfxGain{1.0f};
    f32 panWidthMeters{14.0f};
    bool muteOnFocusLoss{true};
    bool headlessMode{false};
};

struct alignas(8) CueHashSlot {
    u32 key{0};
    u16 cueIdx{0};
    u16 psl{0}; // 0 == empty slot; probe sequence length + 1 when occupied
};
static_assert(sizeof(CueHashSlot) == 8);

// Coordinates Robin Hood cue lookup, 3-stage voice admission, main-thread occlusion, and SDL3 output.
// Move-only RAII owner of the contiguous arena slab and SDL_AudioStream handle; zero world/gameplay headers.
// Invariant: Continuous parameters sync via wait-free triple buffering; occlusion queries run strictly on main thread.
class AudioSystem {
public:
    AudioSystem() = default;
    ~AudioSystem() noexcept;

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&& other) noexcept;
    AudioSystem& operator=(AudioSystem&& other) noexcept;

    [[nodiscard]] Result<void, AudioErrc> Init(const AudioSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterCueDef(
        const SoundCueDef& def,
        assets::ScopeId owningScope = 0) noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterMusicTrackDef(
        const MusicTrackDef& def) noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterCueBank(
        const assets::BlobRecord& cueBlob,
        assets::ScopeId owningScope) noexcept;

    // Marks an asset scope for retirement and returns the bitmask of all scopes now safe to release.
    void RequestScopeRelease(assets::ScopeId scope) noexcept;
    [[nodiscard]] u32 PollReleasableScopeMask() noexcept;

    void SetFocusMuted(bool muted) noexcept;

    void SubmitTriggers(
        std::span<const AudioTriggerReq> triggers,
        const assets::AssetBank& assetBank) noexcept;

    VoiceHandle PlayCue(
        SoundCueId cueId,
        const assets::AssetBank& assetBank,
        f32 worldX = 0.0f,
        f32 worldY = 0.0f,
        u32 emitterId = kNullEmitter) noexcept;

    void StopVoice(VoiceHandle handle, u16 fadeOutBlocks = 1) noexcept;
    void SetBusGain(AudioBusId bus, f32 linearGain) noexcept;
    void SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept;

    void TransitionMusic(u32 trackId, const assets::AssetBank& assetBank, u8 crossfadeBlocks = 94) noexcept;
    void SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept;

    // Evaluates staggered 30 Hz main-thread raycast occlusion and publishes the triple-buffered frame snapshot.
    void Update(const AudioUpdateDesc& desc) noexcept;

    void PumpHeadlessBlocks(u32 blockCount, std::span<f32> optionalCaptureOut = {}) noexcept;

    [[nodiscard]] u32 ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept;
    [[nodiscard]] AudioTelemetrySnapshot Telemetry() const noexcept;
    [[nodiscard]] u64 ComputeAuditHash() const noexcept { return m_mixer.ComputeAuditHash(); }

private:
    struct MainVoiceTracker {
        u32 cueId{0};
        u32 emitterId{kNullEmitter};
        u64 spawnTick{0};
        f32 worldX{0.0f};
        f32 worldY{0.0f};
        f32 baseGain{1.0f};
        f32 basePitch{1.0f};
        f32 minDist{2.5f};
        f32 maxDist{22.0f};
        f32 occlusionFactor{1.0f};
        u16 generation{0};
        u8  priority{0};
        u8  curve{0};
        u8  flags{0};
    };

    struct SharedBridgeState;

    [[nodiscard]] const SoundCueDef* FindCueDef(SoundCueId id, u16& outIndex) const noexcept;
    bool EnqueueCommand(const AudioCommand& cmd) noexcept;
    void ReconcileFinishedVoices() noexcept;
    void ComputeVoiceSpatialTargets(u32 slot, AudioFrameSnapshot& snap) const noexcept;
    void PublishFrameSnapshot(const AudioFrameSnapshot& snap) noexcept;
    void ConsumeAndMixBlocks(u32 blockCount, SDL_AudioStream* stream, std::span<f32> captureOut) noexcept;

    static void AudioStreamCallback(
        void* userdata,
        SDL_AudioStream* stream,
        int additionalAmount,
        int totalAmount) noexcept;

    AudioSystemConfig m_cfg{};
    SDL_AudioStream* m_stream{nullptr};
    u8* m_arenaSlab{nullptr};
    SharedBridgeState* m_bridge{nullptr};
    AudioMixer m_mixer{};

    u64 m_trackedActiveMask{0};
    f32 m_listenerX{0.0f};
    f32 m_listenerY{0.0f};
    u64 m_currentTick{0};
    u32 m_rngState{0xA341316Cu};
    u32 m_pendingUnpinScopeMask{0};
    u8  m_nextStealPoolIdx{0};
};

} // namespace en::audio
