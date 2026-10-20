#pragma once

#include "assets/AssetTypes.h"
#include "time/TimePipeline.h"

namespace en {

class AssetBank;

inline constexpr u32 kAudioSampleRate = 48000;
inline constexpr u32 kAudioMixBlockFrames = 256;
inline constexpr usize kMaxAudioVoices = 64;
inline constexpr usize kAudioBusCount = 8;
inline constexpr usize kMaxMusicStems = 4;
inline constexpr u32 kNullAudioEmitter = 0xFFFFFFFFU;

struct VoiceTag {};
using VoiceHandle = Handle<VoiceTag>;

enum class AudioErrc : u8 {
    None = 0,
    InvalidConfig,
    ArenaExhausted,
    DeviceOpenFailed,
    CorruptedBankBlob,
    CueTableFull,
    VoicePoolExhausted,
};

enum class AudioBusId : u8 {
    Master = 0,
    Music = 1,
    SfxPlayer = 2,
    SfxCombat = 3,
    SfxWorld = 4,
    Ambience = 5,
    Ui = 6,
    Voice = 7,
};

enum class FilterMode : u8 {
    Bypass = 0,
    LowPass = 1,
    HighPass = 2,
    BandPass = 3,
};

enum class AttenuationCurve : u8 {
    None = 0,
    Linear = 1,
    InverseClamped = 2,
    Smoothstep = 3,
    Logarithmic = 4,
    InverseExponential = 5,
};

[[nodiscard]] inline f32 EvaluateDistanceAttenuation(
    AttenuationCurve curve, f32 distance, f32 minDist, f32 maxDist) noexcept {
    if (distance <= minDist || curve == AttenuationCurve::None) return 1.0f;
    if (distance >= maxDist || maxDist <= minDist) return 0.0f;
    const f32 t = std::clamp((distance - minDist) / (maxDist - minDist), 0.0f, 1.0f);
    switch (curve) {
        case AttenuationCurve::Linear:
            return 1.0f - t;
        case AttenuationCurve::InverseClamped:
            return std::clamp(minDist / std::max(minDist, distance), 0.0f, 1.0f);
        case AttenuationCurve::Smoothstep:
            return SmoothStep(maxDist, minDist, distance);
        case AttenuationCurve::Logarithmic:
            return std::clamp(1.0f - std::log1p(t * 9.0f) / std::log1p(9.0f), 0.0f, 1.0f);
        case AttenuationCurve::InverseExponential:
            return std::exp(-4.0f * t) * (1.0f - t);
        default:
            return 1.0f - t;
    }
}

enum class StealPolicy : u8 {
    Reject = 0,
    StealOldest = 1,
    StealQuietest = 2,
};

enum class VoiceFlags : u8 {
    None = 0,
    Looping = 1U << 0,
    Spatialized2D = 1U << 1,
    FollowTimeScale = 1U << 2,
    PauseOnHitstop = 1U << 3,
    BypassOcclusion = 1U << 4,
    OneVoicePerEntity = 1U << 5,
};
consteval bool enable_bitmask_operators(VoiceFlags) { return true; }

struct alignas(32) SoundCueDef {
    AssetId cueId{};
    AudioHandle clipHandle{};
    f32 baseGain{0.8f};
    f32 basePitch{1.0f};
    f32 pitchVariance{0.05f};
    f32 maxDistance{18.0f};
    AudioBusId bus{AudioBusId::SfxCombat};
    TimeChannel channel{TimeChannel::World};
    AttenuationCurve attenuation{AttenuationCurve::Smoothstep};
    StealPolicy stealPolicy{StealPolicy::StealQuietest};
    VoiceFlags flags{VoiceFlags::Spatialized2D};
    u8 maxConcurrency{4};
    u16 cooldownTicks{2};
};
static_assert(sizeof(SoundCueDef) == 64);

struct alignas(8) MusicTrackDef {
    u32 trackId{0};
    std::array<AudioHandle, kMaxMusicStems> stems{};
    f32 baseGain{0.7f};
    u8 stemCount{1};
    bool looping{true};
    u16 crossfadeTicks{60};
};
static_assert(sizeof(MusicTrackDef) == 32);

struct alignas(16) AudioTriggerReq {
    AssetId cueId{};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    u32 emitterId{kNullAudioEmitter};
    u32 flags{0};
};
static_assert(sizeof(AudioTriggerReq) == 32);

struct alignas(16) AudioEmitterView {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 maxDist{15.0f};
    f32 occlusionFactor{0.0f};
};
static_assert(sizeof(AudioEmitterView) == 16);

using AudioOcclusionQueryFn = f32 (*)(f32 lx, f32 ly, f32 ex, f32 ey, void* userCtx) noexcept;

struct AudioUpdateDesc {
    u64 tickIndex{0};
    f32 listenerX{0.0f};
    f32 listenerY{0.0f};
    f32 listenerVx{0.0f};
    f32 listenerVy{0.0f};
    std::span<const f32> channelScales{};
    std::span<const AudioTriggerReq> triggers{};
    const AssetBank* assetBank{nullptr};
    AudioOcclusionQueryFn occlusionQuery{nullptr};
    void* occlusionUserCtx{nullptr};
    u8 frozenChannelMask{0};
    bool focusMuted{false};
};

struct ReverbConfig {
    f32 roomSize{0.65f};
    f32 damping{0.4f};
    f32 wetMix{0.18f};
    bool enabled{false};
};

struct CompressorConfig {
    f32 thresholdLinear{0.6f};
    f32 ratio{4.0f};
    f32 attackCoeff{0.05f};
    f32 releaseCoeff{0.005f};
    bool sidechainFromCombat{true};
};

struct alignas(64) AudioFrameSnapshot {
    std::array<f32, kMaxAudioVoices> targetGainL{};
    std::array<f32, kMaxAudioVoices> targetGainR{};
    std::array<f32, kAudioBusCount> busGains{};
    std::array<f32, kAudioBusCount> busFilterCutoffHz{};
    std::array<FilterMode, kAudioBusCount> busFilterMode{};
    std::array<f32, kMaxMusicStems> musicStemWeights{1.0f, 0.0f, 0.0f, 0.0f};
    u64 activeMask{0};
    u64 frozenVoiceMask{0};
    u64 sequence{0};
};
static_assert(sizeof(AudioFrameSnapshot) == 640);

struct alignas(8) AudioTelemetrySnapshot {
    f32 peakLeft{0.0f};
    f32 peakRight{0.0f};
    f32 cpuLoadRatio{0.02f};
    u16 activeVoices{0};
    u16 stolenVoices{0};
    u32 mixedBlocks{0};
};
static_assert(sizeof(AudioTelemetrySnapshot) == 24);

inline constexpr u32 kCookedAudioBankMagic = 0x44554145U; // 'EAUD'

struct alignas(16) CookedAudioBankHeader {
    u32 magic{kCookedAudioBankMagic};
    u16 version{1};
    u16 cueCount{0};
    u32 bankFlags{0};
    u32 checksumCrc32{0};
};
static_assert(sizeof(CookedAudioBankHeader) == 16);

} // namespace en
