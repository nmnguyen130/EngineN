#pragma once

#include "audio/AudioMixer.h"
#include "core/Memory.h"

namespace en {

class AssetBank;

// 2D spatial audio + music stems. Owns arena and SDL stream.
// Sim ↔ audio via SPSC command ring + double-buffered snapshot.
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

    [[nodiscard]] Result<void, AudioErrc> RegisterCue(
        const SoundCueDef& def, u8 scope = 0U) noexcept;
    [[nodiscard]] Result<void, AudioErrc> RegisterMusic(
        const MusicTrackDef& def, u8 scope = 0U) noexcept;
    [[nodiscard]] Result<void, AudioErrc> RegisterBank(
        const BlobRecord& blob, u8 scope = 0U) noexcept;

    constexpr void RequestScopeRelease(u8 scope) noexcept {
        if (scope < 32U) m_releaseMask |= (1U << scope);
    }
    [[nodiscard]] u32 PollReleasableScopes() noexcept;

    void SetFocusMuted(bool muted) noexcept;
    [[nodiscard]] constexpr bool IsFocusMuted() const noexcept { return m_muted; }

    VoiceHandle Play(SoundCueId id, const AssetBank& assets,
                     f32 x = 0.0f, f32 y = 0.0f,
                     u32 emitter = kNullAudioEmitter) noexcept;
    void Stop(VoiceHandle h, u16 fade = 1U) noexcept;
    void StopAll(u16 fade = 1U) noexcept;

    void SetBusGain(AudioBusId bus, f32 gain) noexcept;
    [[nodiscard]] bool SetMusic(u32 trackId, const AssetBank& assets, u8 fade = 48U) noexcept;
    void SetStemWeights(std::span<const f32, kMaxMusicStems> w) noexcept;

    void Update(const AudioUpdateDesc& desc) noexcept;
    void Pull(i32 bytes) noexcept;
    void Pump(u32 blocks, std::span<f32> capture = {}) noexcept;

    [[nodiscard]] bool HasDevice() const noexcept;
    [[nodiscard]] u32 ExtractEmitters(std::span<AudioEmitterView> out) const noexcept;
    [[nodiscard]] constexpr const AudioTelemetry& Telemetry() const noexcept {
        return m_mixer.Telemetry();
    }
    [[nodiscard]] u64 AuditHash() const noexcept;

private:
    struct Bridge;

    void Enqueue(const AudioCommand& cmd) noexcept;
    void Publish() noexcept;
    void SyncDone() noexcept;
    void Run(u32 blocks, std::span<f32> capture, bool device) noexcept;
    void Spatial(f32 gain, f32 wx, f32 wy, const SoundCueDef& def,
                 f32 lx, f32 ly, f32& oL, f32& oR) const noexcept;

    AudioMixer m_mixer{};
    Arena      m_arena{};
    Bridge*    m_bridge{nullptr};
    u64  m_spatial{0ULL};
    f32  m_lx{0.0f};
    f32  m_ly{0.0f};
    u32  m_releaseMask{0U};
    u16  m_tick{0U};
    bool m_muted{false};
    bool m_dirty{false};
};
static_assert(sizeof(AudioSystem) == 128);

} // namespace en
