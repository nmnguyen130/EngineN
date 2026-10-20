#pragma once

#include "audio/AudioTypes.h"
#include "core/Memory.h"

namespace en {

// Pure C++23 64-voice SoA polyphonic mixer with Hermite4 resampler, Cytomic SVF, reverb, and compressor.
// Non-owning view over arena memory; deterministic across headless and hardware playback.
// Invariant: MixBlock outputs finite [-1, 1] soft-clipped stereo samples with zero allocations.
class alignas(64) AudioDspAndMixer {
public:
    struct VoiceSlot {
        const f32* pcm{nullptr};
        u32 frameCount{0};
        f64 playhead{0.0};
        f32 pitchStep{1.0f};
        f32 curGainL{0.0f};
        f32 curGainR{0.0f};
        f32 svfIc1EqL{0.0f};
        f32 svfIc2EqL{0.0f};
        f32 cutoffRatio{0.45f};
        u32 emitterId{kNullAudioEmitter};
        Vec2 worldPos{};
        f32 maxDist{18.0f};
        f32 baseGain{0.8f};
        AudioBusId bus{AudioBusId::SfxCombat};
        TimeChannel channel{TimeChannel::World};
        VoiceFlags flags{VoiceFlags::None};
        ScopeId scopeId{0};
        u16 generation{1};
        bool active{false};
    };

    [[nodiscard]] bool Init(AlignedArena& arena) noexcept;
    void Reset() noexcept;

    [[nodiscard]] VoiceHandle SpawnVoice(
        const SoundCueDef& def,
        const AudioRecord& clip,
        Vec2 worldPos,
        u32 emitterId,
        ScopeId scope) noexcept;

    void StopVoice(VoiceHandle h) noexcept;
    void MixBlock(
        const AudioFrameSnapshot& snapshot,
        std::span<f32, kAudioMixBlockFrames * 2> outStereo) noexcept;

    constexpr void SetReverbConfig(const ReverbConfig& cfg) noexcept { m_reverbCfg = cfg; }
    constexpr void SetCompressorConfig(const CompressorConfig& cfg) noexcept { m_compCfg = cfg; }

    [[nodiscard]] constexpr std::span<VoiceSlot> Voices() noexcept { return m_voices; }
    [[nodiscard]] constexpr std::span<const VoiceSlot> Voices() const noexcept { return m_voices; }
    [[nodiscard]] constexpr const AudioTelemetrySnapshot& Telemetry() const noexcept { return m_telemetry; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    [[nodiscard]] static constexpr f32 SampleHermite4(
        f32 xm1, f32 x0, f32 x1, f32 x2, f32 frac) noexcept {
        const f32 c = (x1 - xm1) * 0.5f;
        const f32 v = x0 - x1;
        const f32 w = c + v;
        const f32 a = w + v + (x2 - x0) * 0.5f;
        const f32 bNeg = w + a;
        return (((a * frac) - bNeg) * frac + c) * frac + x0;
    }

    [[nodiscard]] static f32 ComputeDopplerPitch(Vec2 listenerPos, Vec2 listenerVel, Vec2 emitterPos, Vec2 emitterVel, f32 speedOfSound = 343.0f) noexcept {
        const Vec2 toEmitter = emitterPos - listenerPos;
        const f32 distSq = toEmitter.LengthSq();
        if (distSq < 1e-4f || speedOfSound <= 1.0f) return 1.0f;
        const f32 invDist = 1.0f / std::sqrt(distSq);
        const Vec2 dir = toEmitter * invDist;
        const f32 vL = std::clamp(listenerVel.Dot(dir), -speedOfSound * 0.5f, speedOfSound * 0.5f);
        const f32 vS = std::clamp(emitterVel.Dot(dir), -speedOfSound * 0.5f, speedOfSound * 0.5f);
        return std::clamp((speedOfSound + vL) / (speedOfSound + vS), 0.5f, 2.0f);
    }

    [[nodiscard]] static constexpr f32 PadeSoftClip(f32 x) noexcept {
        const f32 c = std::clamp(x, -3.0f, 3.0f);
        const f32 c2 = c * c;
        return c * (27.0f + c2) / (27.0f + 9.0f * c2);
    }

private:
    std::span<VoiceSlot> m_voices{};
    std::span<f32> m_reverbCombL{};
    std::span<f32> m_reverbCombR{};
    u32 m_reverbCursor{0};
    f32 m_compEnvelope{0.0f};
    ReverbConfig m_reverbCfg{};
    CompressorConfig m_compCfg{};
    AudioTelemetrySnapshot m_telemetry{};
};

} // namespace en
