#pragma once

#include "audio/AudioTypes.h"

#include <array>
#include <span>

namespace en::audio {

// SIMD-aligned Structure-of-Arrays (SoA) hot state for all 64 physical voices.
struct alignas(64) VoicePoolSoA {
    u64 activeMask{0};
    u64 loopMask{0};
    u64 pauseOnHitstopMask{0};
    u64 followTimeScaleMask{0};
    u64 stoppingMask{0};

    alignas(64) std::array<const i16*, kMaxVoices> pcmPtr{};
    alignas(64) std::array<u64, kMaxVoices> phaseQ32{};
    alignas(64) std::array<u64, kMaxVoices> baseStepQ32{};
    alignas(64) std::array<u32, kMaxVoices> frameCount{};
    alignas(64) std::array<u32, kMaxVoices> loopStart{};
    alignas(64) std::array<f32, kMaxVoices> curGainL{};
    alignas(64) std::array<f32, kMaxVoices> curGainR{};
    alignas(64) std::array<f32, kMaxVoices> svfIc1eq{};
    alignas(64) std::array<f32, kMaxVoices> svfIc2eq{};
    alignas(64) std::array<u16, kMaxVoices> generation{};
    alignas(64) std::array<u16, kMaxVoices> stopBlocksRemaining{};
    alignas(64) std::array<u8,  kMaxVoices> busAndChannel{};
    alignas(64) std::array<u8,  kMaxVoices> scopeId{};
    alignas(64) std::array<i16, kMaxVoices * 4> loopGuardSamples{};
};

struct alignas(64) StealSlotState {
    u16 activeMask{0};
    std::array<u8,  kStealPoolSlots> busIdx{};
    std::array<f32, kStealPoolSlots> gainL{};
    std::array<f32, kStealPoolSlots> gainR{};
    alignas(64) std::array<f32, kStealPoolSlots * kMixBlockFrames> samples{};
};

struct alignas(64) MusicDeckRuntime {
    std::array<const i16*, kMaxMusicStems> stemPcm{};
    std::array<f32, kMaxMusicStems> curStemWeight{};
    u32 frameCount{0};
    u32 loopStartFrame{0};
    u32 loopEndFrame{0};
    u32 cursorFrame{0};
    u16 tempoBpmQ8{120 << 8};
    u8  stemCount{0};
    bool active{false};
};

struct alignas(64) BusDspState {
    std::array<f32, kAudioBusCount> curGain{};
    std::array<f32, kAudioBusCount> svfIc1eqL{};
    std::array<f32, kAudioBusCount> svfIc2eqL{};
    std::array<f32, kAudioBusCount> svfIc1eqR{};
    std::array<f32, kAudioBusCount> svfIc2eqR{};
    f32 sidechainEnvelope{0.0f};
    f32 masterFadeGain{1.0f};
};

// Pure C++23 data-oriented audio mixer, vari-speed Hermite resampler, and Cytomic SVF processor.
// Non-owning view over a caller-supplied arena slice; zero OS or SDL3 dependencies.
// Invariant: MixBlock() executes in bounded O(popcount(activeMask)) time with zero allocations or locks.
class AudioMixer {
public:
    [[nodiscard]] static constexpr usize RequiredArenaBytes() noexcept {
        return sizeof(VoicePoolSoA) + sizeof(StealSlotState) + (sizeof(MusicDeckRuntime) * 2u) +
               sizeof(BusDspState) + (sizeof(f32) * kAudioBusCount * kMixBlockFrames * 2u) + 256u;
    }

    [[nodiscard]] Result<void, AudioErrc> Init(std::span<u8> arenaSlice) noexcept;
    void Reset() noexcept;

    void ApplyCommand(const AudioCommand& cmd) noexcept;
    void MixBlock(
        const AudioFrameSnapshot& snapshot,
        std::span<f32, kMixBlockFrames * 2> outInterleavedStereo) noexcept;

    [[nodiscard]] constexpr const AudioTelemetrySnapshot& Telemetry() const noexcept { return m_telemetry; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    void CaptureStealTail(u32 voiceSlot, u8 stealPoolIdx) noexcept;
    void RefreshLoopGuard(u32 voiceSlot) noexcept;
    void MixActiveVoicesPlanar(const AudioFrameSnapshot& snapshot) noexcept;
    void MixStealPoolPlanar() noexcept;
    void MixMusicDecksPlanar(const AudioFrameSnapshot& snapshot) noexcept;
    void ProcessBusesAndMaster(
        const AudioFrameSnapshot& snapshot,
        std::span<f32, kMixBlockFrames * 2> outStereo) noexcept;

    VoicePoolSoA* m_voices{nullptr};
    StealSlotState* m_stealState{nullptr};
    MusicDeckRuntime* m_decks{nullptr}; // [2]: Deck A (0) and Deck B (1)
    BusDspState* m_busDsp{nullptr};
    f32* m_busScratchL{nullptr};        // [kAudioBusCount * kMixBlockFrames]
    f32* m_busScratchR{nullptr};        // [kAudioBusCount * kMixBlockFrames]

    f32 m_deckCrossfadeAlpha{0.0f};
    f32 m_deckCrossfadeStep{0.0f};
    AudioTelemetrySnapshot m_telemetry{};
    u64 m_sampleCounter{0};
};

} // namespace en::audio