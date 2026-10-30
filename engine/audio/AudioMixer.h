#pragma once

#include "audio/AudioTypes.h"

namespace en {

// Fixed-pool SoA 48 kHz stereo software mixer with 4-point Hermite resampling, SVF occlusion, and bus ducking.
// Non-owning view over an Arena slice; performs zero heap allocations during mixing.
// Invariant: sizeof(AudioMixer) == 48.
class AudioMixer {
public:
    [[nodiscard]] static usize RequiredArenaBytes() noexcept;

    [[nodiscard]] Result<void, AudioErrc> Init(std::span<u8> arena) noexcept;
    void Reset() noexcept;
    void Apply(const AudioCommand& cmd) noexcept;
    void Mix(const AudioFrameSnapshot& snap, std::span<f32, kMixBlockFrames * 2U> out) noexcept;

    [[nodiscard]] constexpr const AudioTelemetry& Telemetry() const noexcept { return m_tel; }
    [[nodiscard]] constexpr u64 ActiveMask() const noexcept { return m_active; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    [[nodiscard]] static constexpr f32 Hermite4(f32 a, f32 b, f32 c, f32 d, f32 t) noexcept {
        const f32 c1 = 0.5f * (c - a);
        const f32 c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
        const f32 c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * t + c2) * t + c1) * t + b;
    }

    [[nodiscard]] static constexpr f32 SvfLowPass(
        f32 x, f32& ic1, f32& ic2, f32 a1, f32 a2, f32 a3) noexcept {
        const f32 v3 = x - ic2;
        const f32 v1 = a1 * ic1 + a2 * v3;
        const f32 v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }

    [[nodiscard]] static constexpr f32 SoftClip(f32 x) noexcept {
        if (x <= -3.0f) return -1.0f;
        if (x >=  3.0f) return  1.0f;
        const f32 x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

private:
    struct State;

    State*         m_state{nullptr};
    u64            m_active{0};
    AudioTelemetry m_tel{};
};
static_assert(sizeof(AudioMixer) == 48);

} // namespace en
