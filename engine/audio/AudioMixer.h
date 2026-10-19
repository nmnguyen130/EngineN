#pragma once

#include "audio/AudioTypes.h"

#include <array>
#include <span>

namespace en::audio {

// SIMD-aligned Structure-of-Arrays (SoA) hot state for all 64 physical voices (57 cache lines exact).
// Cache Line 0 packs all 64-bit control masks so idle blocks touch only 1 cache line.
struct alignas(64) VoicePoolSoA {
    u64 activeMask{0};
    u64 loopMask{0};
    u64 pauseOnHitstopMask{0};
    u64 followTimeScaleMask{0};
    u64 stoppingMask{0};
    u64 freshSpawnMask{0};
    u32 lastSnapshotSeq{0};

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
    alignas(64) std::array<u16, kMaxVoices> spawnPitchQ12{};
    alignas(64) std::array<u8,  kMaxVoices> busAndChannel{};
    alignas(64) std::array<u8,  kMaxVoices> scopeId{};
};
static_assert(sizeof(VoicePoolSoA) == 3648);

// 1-cache-line (64B exact) sample-locked interactive music deck state.
struct alignas(64) MusicDeckRuntime {
    std::array<const i16*, kMaxMusicStems> stemPcm{}; // 32B [0..32)
    std::array<f32, kMaxMusicStems> curStemWeight{};  // 16B [32..48)
    u32 loopStartFrame{0};                            // 4B  [48..52)
    u32 loopEndFrame{0};                              // 4B  [52..56)
    u32 cursorFrame{0};                               // 4B  [56..60)
    u16 tempoBpmQ8{120 << 8};                         // 2B  [60..62)
    u8  stemCount{0};                                 // 1B  [62..63)
    u8  scopeAndActive{0};                            // 1B  [63..64) [7]=active, [4:0]=scopeId

    [[nodiscard]] constexpr bool IsActive() const noexcept { return (scopeAndActive & 0x80u) != 0u; }
    [[nodiscard]] constexpr u8 ScopeId() const noexcept { return static_cast<u8>(scopeAndActive & 0x1Fu); }
    constexpr void SetActive(bool active, u8 scopeId) noexcept {
        scopeAndActive = static_cast<u8>((active ? 0x80u : 0x00u) | (scopeId & 0x1Fu));
    }
};
static_assert(sizeof(MusicDeckRuntime) == 64);

// 3-cache-line (192B exact) per-bus Cytomic SVF and sidechain compressor state.
struct alignas(64) BusDspState {
    std::array<f32, kAudioBusCount> curGain{};
    std::array<f32, kAudioBusCount> svfIc1eqL{};
    std::array<f32, kAudioBusCount> svfIc2eqL{};
    std::array<f32, kAudioBusCount> svfIc1eqR{};
    std::array<f32, kAudioBusCount> svfIc2eqR{};
    f32 sidechainEnvelope{0.0f};
};
static_assert(sizeof(BusDspState) == 192);

// L1-resident (20,352B = 19.8 KB) contiguous arena layout holding all hot mixer state.
struct alignas(64) MixerArenaState {
    VoicePoolSoA voices{};
    std::array<MusicDeckRuntime, 2> decks{};
    BusDspState busDsp{};
    alignas(64) std::array<f32, kAudioBusCount * kMixBlockFrames> busScratchL{};
    alignas(64) std::array<f32, kAudioBusCount * kMixBlockFrames> busScratchR{};
};
static_assert(sizeof(MixerArenaState) == 20352);

// Pure C++23 1-cache-line (64B exact) data-oriented audio mixer, Hermite resampler, and Cytomic SVF processor.
// Non-owning view over a caller-supplied arena slice; zero OS or SDL3 dependencies.
// Invariant: MixBlock() executes in bounded O(popcount(activeMask) + popcount(dirtyBusMask)) time with zero allocations.
class alignas(64) AudioMixer {
public:
    [[nodiscard]] static constexpr usize RequiredArenaBytes() noexcept {
        return sizeof(MixerArenaState) + 64u;
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
    void AccumulateStolenVoiceTail(u32 voiceSlot) noexcept;
    [[nodiscard]] u8 MixActiveVoicesPlanar(const AudioFrameSnapshot& snapshot, u8 dirtyBusMask, u32& outScopeMask) noexcept;
    [[nodiscard]] u8 MixMusicDecksPlanar(const AudioFrameSnapshot& snapshot, u8 dirtyBusMask, u32& ioScopeMask) noexcept;
    void ProcessBusesAndMaster(
        const AudioFrameSnapshot& snapshot,
        u8 dirtyBusMask,
        std::span<f32, kMixBlockFrames * 2> outStereo) noexcept;

    MixerArenaState* m_state{nullptr};              // 8B  [0..8)
    u64 m_sampleCounter{0};                         // 8B  [8..16)
    f32 m_deckCrossfadeAlpha{0.0f};                 // 4B  [16..20)
    f32 m_deckCrossfadeStep{0.0f};                  // 4B  [20..24)
    u8  m_pendingStealBusMask{0};                   // 1B  [24..25)
    alignas(32) AudioTelemetrySnapshot m_telemetry{}; // 32B [32..64)
};
static_assert(sizeof(AudioMixer) == 64);

} // namespace en::audio
