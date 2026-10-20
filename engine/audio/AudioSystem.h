#pragma once

#include "assets/AssetBank.h"
#include "audio/AudioMixer.h"
#include "core/Containers.h"

namespace en {

struct AudioEngineConfig {
    usize arenaBytes{256 * 1024};
    f32 panWidthMeters{12.0f};
    bool headless{true};
};

// High-level 2D spatial audio coordinator, cue bank registry, and scope retirement tracker.
// Move-only RAII subsystem owning AudioDspAndMixer and the SoundCueDef Robin Hood table.
// Invariant: PollReleasableScopeMask() only sets bit s when zero active voices reference scope s.
class alignas(64) AudioEngine {
public:
    AudioEngine() noexcept = default;
    ~AudioEngine() noexcept { Shutdown(); }

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] Result<void, AudioErrc> Init(const AudioEngineConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterCueDef(const SoundCueDef& def) noexcept;
    [[nodiscard]] Result<void, AudioErrc> RegisterCueBank(const BlobRecord& cueBlob) noexcept;

    VoiceHandle PlayCue(
        AssetId cueId,
        const AssetBank& assetBank,
        Vec2 worldPos = {},
        u32 emitterId = kNullAudioEmitter,
        ScopeId scope = 0) noexcept;

    void StopVoice(VoiceHandle h) noexcept { m_mixer.StopVoice(h); }
    void SetBusGain(AudioBusId bus, f32 linearGain) noexcept;
    void SetBusParent(AudioBusId child, AudioBusId parent) noexcept;
    [[nodiscard]] f32 EffectiveBusGain(AudioBusId bus) const noexcept;
    void SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept;

    void RequestScopeRelease(ScopeId scope) noexcept {
        if (scope < kMaxAssetScopes) m_pendingReleaseScopesMask |= (1U << scope);
    }
    [[nodiscard]] u32 PollReleasableScopeMask() noexcept;

    void Update(const AudioUpdateDesc& desc) noexcept;
    void PumpBlocks(u32 blockCount, std::span<f32> optionalCaptureOut = {}) noexcept;

    [[nodiscard]] u32 ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept;
    [[nodiscard]] constexpr AudioDspAndMixer& Mixer() noexcept { return m_mixer; }
    [[nodiscard]] constexpr const AudioTelemetrySnapshot& Telemetry() const noexcept { return m_mixer.Telemetry(); }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    AudioFrameSnapshot m_snapshot{};
    std::array<f32, kAudioBusCount> m_rawBusGains{};
    std::array<u8, kAudioBusCount> m_busParents{};
    AudioDspAndMixer m_mixer{};
    RobinHoodMap<SoundCueDef> m_cueTable{};
    AlignedArena m_arena{};
    f32 m_invPanWidth{1.0f / 12.0f};
    u32 m_pendingReleaseScopesMask{0};
    bool m_initialized{false};
};

} // namespace en
