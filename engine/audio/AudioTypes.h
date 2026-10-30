#pragma once

#include "assets/AssetTypes.h"
#include "time/TimeTypes.h"

namespace en {

class AssetBank;

inline constexpr u32 kAudioSampleRate  = 48000U;
inline constexpr u32 kMixBlockFrames   = 256U;
inline constexpr u32 kMaxAudioVoices   = 48U;
inline constexpr u32 kAudioBusCount    = 5U;
inline constexpr u32 kMaxMusicStems    = 4U;
inline constexpr u32 kNullAudioEmitter = 0xFFFFFFFFU;

inline constexpr u64 kPhaseOne          = 1ULL << 32U;
inline constexpr u64 kPhaseFracMask     = 0xFFFFFFFFULL;
inline constexpr f32 kInvPhaseOne       = 1.0f / 4294967296.0f;
inline constexpr f32 kInvMixBlockFrames = 1.0f / static_cast<f32>(kMixBlockFrames);
inline constexpr f32 kInvSampleRate     = 1.0f / static_cast<f32>(kAudioSampleRate);

inline constexpr f32 kMinCutoffHz       = 40.0f;
inline constexpr f32 kMaxCutoffHz       = 20000.0f;
inline constexpr f32 kSvfBypassHz       = 18500.0f;
inline constexpr f32 kDefaultOccHz      = 900.0f;
inline constexpr f32 kSvfDamp           = 1.41421356f;
inline constexpr f32 kEqPower           = 0.70710678f;
inline constexpr f32 kSilence           = 1e-6f;
inline constexpr f32 kMaxGain           = 4.0f;
inline constexpr f32 kMinPitch          = 0.125f;
inline constexpr f32 kMaxPitch          = 8.0f;

enum class AudioErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    CorruptedBankBlob,
    CueTableFull,
};

enum class AudioBus : u8 {
    Master   = 0,
    Music    = 1,
    Sfx      = 2,
    Ambience = 3,
    Ui       = 4,
};

enum class AttenuationCurve : u8 {
    None           = 0,
    Linear         = 1,
    InverseClamped = 2,
    Smoothstep     = 3,
};

enum class VoiceFlags : u8 {
    None              = 0,
    Looping           = 1U << 0,
    Spatial2D         = 1U << 1,
    FollowTimeScale   = 1U << 2,
    PauseOnFreeze     = 1U << 3,
    BypassOcclusion   = 1U << 4,
    OneVoicePerEntity = 1U << 5,
};
consteval bool enable_bitmask_operators(VoiceFlags) noexcept { return true; }

enum class AudioCmdType : u8 {
    Spawn      = 0,
    Stop       = 1,
    BindStem   = 2,
    CommitDeck = 3,
    Panic      = 4,
};

struct VoiceTag {};
using VoiceHandle = Handle<VoiceTag>;
inline constexpr VoiceHandle kNullVoice{};
static_assert(sizeof(VoiceHandle) == 4);

// 24-byte one-shot spatial audio trigger request queued from gameplay simulation.
struct AudioTrigger {
    AssetId cueId{};
    Vec2    position{0.0f, 0.0f};
    u32     emitter{kNullAudioEmitter};
    f32     gainScale{1.0f};
};
static_assert(sizeof(AudioTrigger) == 24);

// 16-byte active spatial emitter diagnostic snapshot for debug overlay rendering.
struct AudioEmitterView {
    Vec2 position{0.0f, 0.0f};
    f32  maxDist{1.0f};
    f32  occlusion{0.0f};
};
static_assert(sizeof(AudioEmitterView) == 16);

// 48-byte sound cue descriptor with variant selection, 2D spatial falloff, and voice-stealing priority.
struct SoundCueDef {
    AssetId          id{};
    AudioHandle      variants[2]{};
    f32              minDist{2.0f};
    f32              maxDist{24.0f};
    f32              baseGain{1.0f};
    f32              basePitch{1.0f};
    f32              gainJitter{0.0f};
    f32              pitchJitter{0.0f};
    u32              loopStart{0};
    u16              cooldownTicks{2};
    u16              pad{0};
    u8               priority{128};
    u8               maxConcurrency{4};
    VoiceFlags       flags{VoiceFlags::Spatial2D | VoiceFlags::FollowTimeScale};
    AudioBus         bus{AudioBus::Sfx};
    TimeChannel      channel{TimeChannel::World};
    AttenuationCurve atten{AttenuationCurve::Smoothstep};
};
static_assert(sizeof(SoundCueDef) == 56);

// 48-byte multi-stem adaptive music track definition supporting crossfades and loop points.
struct MusicTrackDef {
    AssetId     id{};
    AudioHandle stems[kMaxMusicStems]{};
    f32         weights[kMaxMusicStems]{1.0f, 0.0f, 0.0f, 0.0f};
    u32         loopStart{0};
    u8          stemCount{1};
    bool        loop{true};
    u16         pad{0};
};
static_assert(sizeof(MusicTrackDef) == 48);

// 40-byte command packet passed from AudioSystem to AudioMixer.
struct AudioCommand {
    const f32*   pcm{nullptr};
    u32          frames{0};
    u32          loopStart{0};
    f32          gainL{1.0f};
    f32          gainR{1.0f};
    f32          pitch{1.0f};
    u16          sampleRate{48000};
    u16          generation{1};
    u16          fadeBlocks{1};
    AudioCmdType type{AudioCmdType::Spawn};
    AudioBus     bus{AudioBus::Sfx};
    u8           slot{0};
    VoiceFlags   flags{VoiceFlags::None};
    u8           channels{1};
    u8           scope{0};
};
static_assert(sizeof(AudioCommand) == 40);

// Double-buffered per-frame modulation snapshot read by AudioMixer.
struct AudioFrameSnapshot {
    std::array<f32, kMaxAudioVoices> gainL{};
    std::array<f32, kMaxAudioVoices> gainR{};
    std::array<f32, kMaxAudioVoices> pitch{};
    std::array<f32, kAudioBusCount>  busGain{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::array<f32, kMaxMusicStems>  stemWeights{1.0f, 0.0f, 0.0f, 0.0f};
    u64                              pausedMask{0};
    u64                              occludedMask{0};
    f32                              occCutoffHz{kDefaultOccHz};
    f32                              duckStrength{0.7f};
    bool                             muted{false};
};

// 32-byte mixer telemetry counters and peak meters.
struct AudioTelemetry {
    u64 blocks{0};
    f32 peakL{0.0f};
    f32 peakR{0.0f};
    f32 duckEnv{0.0f};
    u16 voices{0};
    u16 stolen{0};
    u16 stems{0};
    u16 pad{0};
};
static_assert(sizeof(AudioTelemetry) == 32);

struct AudioUpdateDesc {
    std::span<const AudioTrigger> triggers{};
    const AssetBank*              assets{nullptr};
    bool (*occlusionFn)(Vec2 listener, Vec2 emitter, void* ctx) noexcept{nullptr};
    void*                         occlusionCtx{nullptr};
    const f32*                    channelScales{nullptr};
    Vec2                          listenerPos{0.0f, 0.0f};
    u32                           tick{0};
    u8                            frozenMask{0};
    i8                            focusMute{-1};
};

struct AudioConfig {
    u32  arenaBytes{128U * 1024U};
    f32  panWidth{16.0f};
    f32  occCutoffHz{kDefaultOccHz};
    bool headless{false};
};
static_assert(sizeof(AudioConfig) == 16);

} // namespace en
