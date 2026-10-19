#pragma once

#include "audio/AudioMixer.h"
#include "audio/AudioTypes.h"

#include <span>

struct SDL_AudioStream;

namespace en::audio {

struct AudioSystemConfig {
    u32 arenaBytes{64 * 1024};
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
// 2-cache-line (128B exact) move-only RAII owner of the arena slab and SDL_AudioStream; zero world/gameplay headers.
// Invariant: Continuous parameters sync via wait-free triple buffering; occlusion queries run strictly on main thread.
class alignas(64) AudioSystem {
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
        const MusicTrackDef& def,
        assets::ScopeId owningScope = 0) noexcept;

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

    // Consolidates trigger ingestion, focus mute, 30 Hz staggered raycast occlusion, and snapshot publication.
    void Update(const AudioUpdateDesc& desc) noexcept;

    void PumpHeadlessBlocks(u32 blockCount, std::span<f32> optionalCaptureOut = {}) noexcept;

    [[nodiscard]] u32 ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept;
    [[nodiscard]] AudioTelemetrySnapshot Telemetry() const noexcept;
    [[nodiscard]] u64 ComputeAuditHash() const noexcept { return m_mixer.ComputeAuditHash(); }

private:
    // All-in-one 32-byte (2 per cache line, zero padding) main-thread voice tracker.
    struct alignas(32) MainVoiceTracker {
        u32 cueId{0};                // 4B [0..4)
        u32 emitterId{kNullEmitter}; // 4B [4..8)
        u32 spawnTick{0};            // 4B [8..12)
        f32 worldX{0.0f};            // 4B [12..16)
        f32 worldY{0.0f};            // 4B [16..20)
        u16 baseGainQ14{16384};      // 2B [20..22)
        u16 occlusionQ14{16384};     // 2B [22..24)
        u16 generation{0};           // 2B [24..26)
        u8  minDistQ2{10};           // 1B [26..27)
        u8  maxDistQ1{44};           // 1B [27..28)
        u8  priority{128};           // 1B [28..29)
        u8  curve{0};                // 1B [29..30)
        u8  flags{0};                // 1B [30..31)
        u8  scopeId{0};              // 1B [31..32)

        [[nodiscard]] constexpr f32 MinDist() const noexcept { return static_cast<f32>(minDistQ2) * 0.25f; }
        [[nodiscard]] constexpr f32 MaxDist() const noexcept {
            const f32 minD = MinDist();
            const f32 maxD = static_cast<f32>(maxDistQ1) * 0.5f;
            return maxD > minD + 0.5f ? maxD : (minD + 0.5f);
        }
        [[nodiscard]] constexpr f32 BaseGain() const noexcept { return static_cast<f32>(baseGainQ14) * (1.0f / 16384.0f); }
        [[nodiscard]] constexpr f32 Occlusion() const noexcept { return static_cast<f32>(occlusionQ14) * (1.0f / 16384.0f); }
        [[nodiscard]] constexpr AttenuationCurve Curve() const noexcept { return static_cast<AttenuationCurve>(curve); }
        [[nodiscard]] constexpr VoiceFlags Flags() const noexcept { return static_cast<VoiceFlags>(flags); }
    };
    static_assert(sizeof(MainVoiceTracker) == 32);

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

    // Cache Line 0 (offset 0..63): Hot main-thread control and tracking state.
    SharedBridgeState* m_bridge{nullptr}; // 8B [0..8)   Also base pointer of the 64B-aligned arena slab
    SDL_AudioStream* m_stream{nullptr};   // 8B [8..16)
    u64 m_trackedActiveMask{0};           // 8B [16..24)
    u64 m_spatialActiveMask{0};           // 8B [24..32) Subset of m_trackedActiveMask with Spatialized2D
    u64 m_currentTick{0};                 // 8B [32..40)
    f32 m_listenerX{0.0f};                // 4B [40..44)
    f32 m_listenerY{0.0f};                // 4B [44..48)
    f32 m_invPanWidth{1.0f / 14.0f};      // 4B [48..52)
    u32 m_rngState{0xA341316Cu};          // 4B [52..56)
    u32 m_pendingUnpinScopeMask{0};       // 4B [56..60)
    bool m_muteOnFocusLoss{true};         // 1B [60..61)
    bool m_stagingDirty{false};           // 1B [61..62)

    // Cache Line 1 (offset 64..127, 64B exact): Pure C++23 DSP mixer.
    AudioMixer m_mixer{};                 // 64B [64..128)
};
static_assert(sizeof(AudioSystem) == 128);

} // namespace en::audio
