#pragma once

#include "assets/AssetTypes.h"
#include "core/Math.h"
#include "time/TimeTypes.h"

#include <array>
#include <span>

namespace en {

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

enum class AudioBusId : u8 {
    Master = 0,
    Music,
    Sfx,
    Ambience,
    Ui,
};

enum class AttenuationCurve : u8 {
    None = 0,
    Linear,
    InverseClamped,
    Smoothstep,
};

enum class VoiceFlags : u8 {
    None              = 0,
    Looping           = 1U << 0U,
    Spatialized2D     = 1U << 1U,
    FollowTimeScale   = 1U << 2U,
    PauseOnFreeze     = 1U << 3U,
    BypassOcclusion   = 1U << 4U,
    OneVoicePerEntity = 1U << 5U,
};
consteval bool enable_bitmask_operators(VoiceFlags) noexcept { return true; }

enum class AudioCmdType : u8 {
    Spawn = 0,
    Stop,
    BindStem,
    CommitDeck,
    Panic,
};

struct VoiceTag {};
using VoiceHandle = Handle<VoiceTag>;
inline constexpr VoiceHandle kNullVoiceHandle{};
static_assert(sizeof(VoiceHandle) == 4);

struct alignas(4) SoundCueId {
    u32 value{0U};
    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0U; }
    [[nodiscard]] static constexpr SoundCueId FromAssetId(AssetId id) noexcept {
        return SoundCueId{static_cast<u32>(id.value ^ (id.value >> 32U))};
    }
    [[nodiscard]] static constexpr SoundCueId Hash(std::string_view s) noexcept {
        return FromAssetId(AssetId::Hash(s));
    }
    [[nodiscard]] constexpr bool operator==(const SoundCueId&) const noexcept = default;
};
static_assert(sizeof(SoundCueId) == 4);

struct alignas(16) AudioTriggerReq {
    SoundCueId cueId{};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    u32 emitterId{kNullAudioEmitter};
};
static_assert(sizeof(AudioTriggerReq) == 16);

struct alignas(16) AudioEmitterView {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 maxDist{1.0f};
    f32 occlusion{0.0f};
};
static_assert(sizeof(AudioEmitterView) == 16);

struct alignas(16) CookedAudioBankHeader {
    static constexpr u32 kMagic   = 0x44554145U;
    static constexpr u16 kVersion = 1U;
    u32 magic{kMagic};
    u16 version{kVersion};
    u16 cueCount{0U};
    u16 musicTrackCount{0U};
    u16 flags{0U};
    u32 checksum{0U};
};
static_assert(sizeof(CookedAudioBankHeader) == 16);

struct alignas(16) SoundCueDef {
    SoundCueId       id{};
    AudioHandle      variants[2]{};
    f32              minDist{2.0f};
    f32              maxDist{24.0f};
    f32              baseGain{1.0f};
    f32              basePitch{1.0f};
    f32              gainJitter{0.0f};
    f32              pitchJitter{0.0f};
    u32              loopStart{0U};
    u16              cooldownTicks{2U};
    u8               priority{128U};
    u8               maxConcurrency{4U};
    VoiceFlags       flags{VoiceFlags::Spatialized2D | VoiceFlags::FollowTimeScale};
    AudioBusId       bus{AudioBusId::Sfx};
    TimeChannel      channel{TimeChannel::World};
    AttenuationCurve atten{AttenuationCurve::Smoothstep};
};
static_assert(sizeof(SoundCueDef) == 48);

struct alignas(16) MusicTrackDef {
    u32         trackId{0U};
    u32         loopStart{0U};
    AudioHandle stems[kMaxMusicStems]{};
    f32         weights[kMaxMusicStems]{1.0f, 0.0f, 0.0f, 0.0f};
    u8          stemCount{1U};
    bool        loop{true};
};
static_assert(sizeof(MusicTrackDef) == 48);

struct alignas(8) AudioCommand {
    const f32*   pcm{nullptr};
    u32          frames{0U};
    u32          loopStart{0U};
    f32          gainL{1.0f};
    f32          gainR{1.0f};
    f32          pitch{1.0f};
    u16          sampleRate{48000U};
    u16          generation{1U};
    u16          fadeBlocks{1U};
    AudioCmdType type{AudioCmdType::Spawn};
    AudioBusId   bus{AudioBusId::Sfx};
    u8           slot{0U};
    VoiceFlags   flags{VoiceFlags::None};
    u8           channels{1U};
    u8           scope{0U};
};
static_assert(sizeof(AudioCommand) == 40);

struct alignas(64) AudioFrameSnapshot {
    std::array<f32, kMaxAudioVoices> gainL{};
    std::array<f32, kMaxAudioVoices> gainR{};
    std::array<f32, kMaxAudioVoices> pitch{};
    std::array<f32, kAudioBusCount>  busGain{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::array<f32, kMaxMusicStems>  stemWeights{1.0f, 0.0f, 0.0f, 0.0f};
    u64 pausedMask{0ULL};
    u64 occludedMask{0ULL};
    f32 occCutoffHz{kDefaultOccHz};
    f32 duckStrength{0.7f};
    bool muted{false};
};

struct alignas(8) AudioTelemetry {
    u64 blocks{0ULL};
    f32 peakL{0.0f};
    f32 peakR{0.0f};
    f32 duckEnv{0.0f};
    u16 voices{0U};
    u16 stolen{0U};
    u16 stems{0U};
    u16 pad{0U};
};
static_assert(sizeof(AudioTelemetry) == 32);

struct alignas(8) AudioUpdateDesc {
    std::span<const AudioTriggerReq> triggers{};
    const AssetBank* assets{nullptr};
    bool (*occlusion)(f32 lx, f32 ly, f32 ex, f32 ey, void* ctx) noexcept{nullptr};
    void*  occlusionCtx{nullptr};
    const f32* channelScales{nullptr};
    u32 tick{0U};
    f32 listenerX{0.0f};
    f32 listenerY{0.0f};
    u8  frozenMask{0U};
    i8  focusMute{-1};
};

struct alignas(8) AudioSystemConfig {
    u32  arenaBytes{65536U};
    f32  panWidth{16.0f};
    f32  occCutoffHz{kDefaultOccHz};
    bool headless{false};
};
static_assert(sizeof(AudioSystemConfig) == 16);

} // namespace en
