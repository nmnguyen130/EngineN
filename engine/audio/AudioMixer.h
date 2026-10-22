#pragma once

#include "audio/AudioTypes.h"

namespace en {

// 64-byte (1 cache line exact, zero padding) pure C++23 polyphonic SoA synthesizer, virtual-voice tracker, and 8-bus DSP mixer.
// Non-owning view over a single contiguous MixerArenaState block; zero heap allocations and zero OS/SDL headers.
// Invariant: sizeof(AudioMixer) == 64, out-of-earshot looping voices advance phase in O(1), and output samples are bounded within [-1.0, +1.0].
class alignas(64) AudioMixer {
public:
    [[nodiscard]] static constexpr usize RequiredArenaBytes() noexcept { return 65536U; }

    [[nodiscard]] Result<void, AudioErrc> Init(std::span<u8> arenaSlice) noexcept;
    void Reset() noexcept;

    void ApplyCommand(const AudioCommand& cmd) noexcept;

    void MixBlock(
        const AudioFrameSnapshot& snapshot,
        std::span<f32, kMixBlockFrames * 2U> outInterleavedStereo) noexcept;

    [[nodiscard]] constexpr const AudioTelemetrySnapshot& Telemetry() const noexcept { return m_telemetry; }
    [[nodiscard]] constexpr u64 ActiveVoiceMask() const noexcept { return m_activeVoicesMask; }
    [[nodiscard]] constexpr u64 VirtualVoiceMask() const noexcept { return m_virtualVoicesMask; }
    [[nodiscard]] u32 ActiveAssetScopesMask() const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    // 4-point 3rd-order Catmull-Rom Hermite cubic interpolation kernel.
    [[nodiscard]] static constexpr f32 InterpolateHermite4(
        f32 xm1,
        f32 x0,
        f32 x1,
        f32 x2,
        f32 frac) noexcept {
        const f32 c0 = x0;
        const f32 c1 = 0.5f * (x1 - xm1);
        const f32 c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const f32 c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    // Single-sample Cytomic linear trapezoidal zero-delay feedback state variable filter step.
    [[nodiscard]] static constexpr f32 StepCytomicSvf(
        f32 input,
        f32& ic1,
        f32& ic2,
        f32 a1,
        f32 a2,
        f32 a3,
        FilterMode mode = FilterMode::LowPass) noexcept {
        const f32 v3 = input - ic2;
        const f32 v1 = a1 * ic1 + a2 * v3;
        const f32 v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        if (mode == FilterMode::LowPass) return v2;
        if (mode == FilterMode::BandPass) return v1;
        if (mode == FilterMode::HighPass) return input - 1.41421356f * v1 - v2;
        return input;
    }

    // Rational Padé [3/2] hyperbolic tangent soft-clipping limiter bounded strictly to [-1.0, +1.0].
    [[nodiscard]] static constexpr f32 SoftClipTanhPade(f32 x) noexcept {
        if (x <= -3.0f) return -1.0f;
        if (x >=  3.0f) return  1.0f;
        const f32 x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

private:
    struct MixerArenaState;

    void EnsureBusInitialized(u8 busIdx) noexcept;
    void EnsureReverbSendInitialized() noexcept;
    void AccumulateStolenVoiceTail(u8 slot) noexcept;
    void AccumulateMusicDeckTail() noexcept;

    MixerArenaState* m_state{nullptr};          // 8B
    u64 m_activeVoicesMask{0ULL};               // 8B
    u64 m_virtualVoicesMask{0ULL};              // 8B
    u32 m_activeScopesCache{0U};                // 4B
    u16 m_dirtyBusMask{0U};                     // 2B
    u16 m_pendingStealBusMask{0U};              // 2B
    AudioTelemetrySnapshot m_telemetry{};       // 32B
};
static_assert(sizeof(AudioMixer) == 64);

} // namespace en
