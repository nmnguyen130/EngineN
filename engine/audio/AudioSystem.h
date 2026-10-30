#pragma once

#include "audio/AudioMixer.h"
#include "core/Containers.h"

namespace en {

class AssetBank;

// 2D spatial audio and multi-stem adaptive music system with lock-free SPSC mixer command bridge.
// Move-disabled owner of an Arena and optional SDL_AudioStream; zero heap allocations after Init.
// Invariant: sizeof(AudioSystem) == 96.
class AudioSystem {
public:
    AudioSystem() noexcept = default;
    ~AudioSystem() noexcept { Shutdown(); }

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    [[nodiscard]] Result<void, AudioErrc> Init(const AudioConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterCue(const SoundCueDef& def) noexcept;
    [[nodiscard]] Result<void, AudioErrc> RegisterMusic(const MusicTrackDef& def) noexcept;

    void SetFocusMuted(bool muted) noexcept;
    [[nodiscard]] constexpr bool IsFocusMuted() const noexcept { return m_muted; }

    VoiceHandle Play(
        AssetId cueId,
        const AssetBank& assets,
        Vec2 position = {0.0f, 0.0f},
        u32 emitter = kNullAudioEmitter,
        f32 gainScale = 1.0f) noexcept;
    void Stop(VoiceHandle handle, u16 fadeBlocks = 1U) noexcept;
    void StopAll(u16 fadeBlocks = 1U) noexcept;

    void SetBusGain(AudioBus bus, f32 gain) noexcept;
    [[nodiscard]] f32 GetBusGain(AudioBus bus) const noexcept;

    [[nodiscard]] bool PlayMusic(AssetId trackId, const AssetBank& assets, u8 fadeBlocks = 48U) noexcept;
    void SetStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept;

    void Update(const AudioUpdateDesc& desc) noexcept;
    void PullDeviceBytes(i32 bytes) noexcept;
    void Pump(u32 blocks, std::span<f32> captureOut = {}) noexcept;

    [[nodiscard]] bool HasDevice() const noexcept;
    [[nodiscard]] u32 ExtractEmitters(std::span<AudioEmitterView> out) const noexcept;
    [[nodiscard]] constexpr const AudioTelemetry& Telemetry() const noexcept {
        return m_mixer.Telemetry();
    }
    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_bridge != nullptr; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    struct Bridge;

    void Enqueue(const AudioCommand& cmd) noexcept;
    void PublishSnapshot() noexcept;
    void SyncFinishedVoices() noexcept;
    void RunBlocks(u32 blocks, std::span<f32> captureOut, bool pushToDevice) noexcept;
    void ComputeSpatialGains(
        f32 gain,
        Vec2 pos,
        const SoundCueDef& def,
        Vec2 listener,
        f32& outL,
        f32& outR) const noexcept;

    AudioMixer m_mixer{};
    Arena      m_arena{};
    Bridge*    m_bridge{nullptr};
    u64        m_spatialMask{0};
    Vec2       m_listener{0.0f, 0.0f};
    u16        m_tick{0};
    bool       m_muted{false};
    bool       m_dirty{false};
    u32        m_pad{0};
};
static_assert(sizeof(AudioSystem) == 96);

} // namespace en
