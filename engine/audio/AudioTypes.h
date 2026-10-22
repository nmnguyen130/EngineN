#pragma once

#include "assets/AssetTypes.h"
#include "core/Math.h"
#include "core/Types.h"
#include "time/TimeTypes.h"

#include <array>
#include <span>
#include <type_traits>

namespace en {

class AssetBank;

inline constexpr u32 kAudioSampleRate = 48000U;
inline constexpr u32 kMixBlockFrames = 256U;
inline constexpr u32 kMaxAudioVoices = 64U;
inline constexpr u32 kAudioBusCount = 8U;
inline constexpr u32 kMaxMusicStems = 4U;
inline constexpr u32 kNullAudioEmitter = 0xFFFFFFFFU;

enum class AudioErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    ArenaExhausted,
    DeviceOpenFailed,
    CorruptedBankBlob,
    CueTableFull,
    VoicePoolExhausted,
};

enum class AudioBusId : u8 {
    Master    = 0,
    Music     = 1,
    SfxPlayer = 2,
    SfxCombat = 3,
    SfxWorld  = 4,
    Ambience  = 5,
    Ui        = 6,
    Voice     = 7,
};

enum class FilterMode : u8 {
    Bypass   = 0,
    LowPass  = 1,
    HighPass = 2,
    BandPass = 3,
};

enum class AttenuationCurve : u8 {
    None           = 0,
    Linear         = 1,
    InverseClamped = 2,
    Smoothstep     = 3,
};

enum class StealPolicy : u8 {
    Reject        = 0,
    StealOldest   = 1,
    StealQuietest = 2,
};

enum class VoiceFlags : u8 {
    None              = 0,
    Looping           = 1U << 0U,
    Spatialized2D     = 1U << 1U,
    FollowTimeScale   = 1U << 2U,
    PauseOnFreeze     = 1U << 3U,
    BypassOcclusion   = 1U << 4U,
    OneVoicePerEntity = 1U << 5U,
    EnableDoppler     = 1U << 6U,
    ReverbSend        = 1U << 7U,
};

[[nodiscard]] constexpr VoiceFlags operator|(VoiceFlags a, VoiceFlags b) noexcept {
    return static_cast<VoiceFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr VoiceFlags operator&(VoiceFlags a, VoiceFlags b) noexcept {
    return static_cast<VoiceFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}
[[nodiscard]] constexpr VoiceFlags operator~(VoiceFlags a) noexcept {
    return static_cast<VoiceFlags>(~static_cast<u8>(a));
}
constexpr VoiceFlags& operator|=(VoiceFlags& a, VoiceFlags b) noexcept {
    a = a | b;
    return a;
}
constexpr VoiceFlags& operator&=(VoiceFlags& a, VoiceFlags b) noexcept {
    a = a & b;
    return a;
}
[[nodiscard]] constexpr bool HasVoiceFlag(VoiceFlags flags, VoiceFlags test) noexcept {
    return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0U;
}

enum class AudioCmdType : u8 {
    SpawnVoice      = 0,
    StopVoice       = 1,
    BindMusicStem   = 2,
    CommitMusicDeck = 3,
    PanicReset      = 4,
};

// 4-byte generational handle to an active polyphonic voice slot in [0..63].
struct alignas(4) VoiceHandle {
    u16 index{0U};
    u16 generation{0U};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0U && index < kMaxAudioVoices; }
    [[nodiscard]] constexpr bool operator==(const VoiceHandle&) const noexcept = default;
};
static_assert(sizeof(VoiceHandle) == 4);

inline constexpr VoiceHandle kNullVoiceHandle{0U, 0U};

// 4-byte hashed sound cue identifier (folded 32-bit FNV-1a from AssetId).
struct alignas(4) SoundCueId {
    u32 value{0U};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0U; }
    [[nodiscard]] static constexpr SoundCueId FromAssetId(AssetId id) noexcept {
        return SoundCueId{static_cast<u32>(id.value ^ (id.value >> 32U))};
    }
    [[nodiscard]] static constexpr SoundCueId Hash(std::string_view str) noexcept {
        return FromAssetId(AssetId::Hash(str));
    }
    [[nodiscard]] constexpr bool operator==(const SoundCueId&) const noexcept = default;
};
static_assert(sizeof(SoundCueId) == 4);

// 16-byte (4 per 64B cache line, zero padding) sound trigger request submitted from simulation/gameplay cues.
struct alignas(16) AudioTriggerReq {
    SoundCueId cueId{};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    u32 emitterId{kNullAudioEmitter};
};
static_assert(sizeof(AudioTriggerReq) == 16);

// 16-byte (4 per 64B cache line, zero padding) debug/HUD view of an active spatialized emitter.
struct alignas(16) AudioEmitterView {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 maxDist{1.0f};
    f32 occlusionFactor{0.0f};
};
static_assert(sizeof(AudioEmitterView) == 16);

// 16-byte cooked audio bank binary header ("EAUD" v1).
struct alignas(16) CookedAudioBankHeader {
    static constexpr u32 kMagic = 0x44554145U; // "EAUD"
    static constexpr u16 kVersion = 1U;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 cueCount{0U};
    u16 musicTrackCount{0U};
    u16 flags{0U};
    u32 checksum{0U};
};
static_assert(sizeof(CookedAudioBankHeader) == 16);

// 32-byte (2 per 64B cache line, zero padding) sound cue descriptor with loop-start point, randomized jitter, and routing.
struct alignas(32) SoundCueDef {
    SoundCueId id{};
    AudioHandle clipVariants[2]{};
    f32 minDist{2.0f};
    f32 maxDist{24.0f};
    u16 baseGain_q14{16384U};       // 16384 == 1.0f linear gain
    u16 basePitch_q12{4096U};       // 4096 == 1.0f pitch ratio
    u16 loopStartFrame{0U};         // Sample frame index for seamless sustain loops [loopStartFrame .. frameCount)
    u8 priority{128U};              // [0..255], higher priority steals lower priority
    u8 maxConcurrency{4U};          // Max simultaneous voices for this cue (0 == unlimited)
    u8 cooldownTicks{2U};           // Same-cue energy-summing coalescer window in ticks
    VoiceFlags flags{VoiceFlags::Spatialized2D | VoiceFlags::FollowTimeScale};
    u8 packedRouting{static_cast<u8>(AudioBusId::SfxWorld) |
                     (static_cast<u8>(StealPolicy::StealQuietest) << 6U)}; // bits 0..2: AudioBusId, bits 3..5: TimeChannel, bits 6..7: StealPolicy
    u8 packedCurveAndJitter{static_cast<u8>(AttenuationCurve::Smoothstep)}; // bits 0..1: AttenuationCurve, bits 2..4: gainJitter, bits 5..7: pitchJitter

    [[nodiscard]] constexpr AudioBusId Bus() const noexcept {
        return static_cast<AudioBusId>(packedRouting & 0x07U);
    }
    constexpr void SetBus(AudioBusId bus) noexcept {
        packedRouting = static_cast<u8>((packedRouting & 0xF8U) | (static_cast<u8>(bus) & 0x07U));
    }

    [[nodiscard]] constexpr TimeChannel Channel() const noexcept {
        return static_cast<TimeChannel>((packedRouting >> 3U) & 0x07U);
    }
    constexpr void SetChannel(TimeChannel ch) noexcept {
        packedRouting = static_cast<u8>((packedRouting & 0xC7U) | ((static_cast<u8>(ch) & 0x07U) << 3U));
    }

    [[nodiscard]] constexpr StealPolicy Steal() const noexcept {
        return static_cast<StealPolicy>((packedRouting >> 6U) & 0x03U);
    }
    constexpr void SetStealPolicy(StealPolicy s) noexcept {
        packedRouting = static_cast<u8>((packedRouting & 0x3FU) | ((static_cast<u8>(s) & 0x03U) << 6U));
    }

    [[nodiscard]] constexpr AttenuationCurve Attenuation() const noexcept {
        return static_cast<AttenuationCurve>(packedCurveAndJitter & 0x03U);
    }
    constexpr void SetAttenuation(AttenuationCurve c) noexcept {
        packedCurveAndJitter = static_cast<u8>((packedCurveAndJitter & 0xFCU) | (static_cast<u8>(c) & 0x03U));
    }

    [[nodiscard]] constexpr f32 GainJitter() const noexcept {
        return static_cast<f32>((packedCurveAndJitter >> 2U) & 0x07U) * (1.0f / 32.0f);
    }
    constexpr void SetGainJitter(f32 jitterRatio) noexcept {
        const u8 steps = static_cast<u8>(std::clamp(jitterRatio, 0.0f, 0.21875f) * 32.0f + 0.5f) & 0x07U;
        packedCurveAndJitter = static_cast<u8>((packedCurveAndJitter & 0xE3U) | (steps << 2U));
    }

    [[nodiscard]] constexpr f32 PitchJitter() const noexcept {
        return static_cast<f32>((packedCurveAndJitter >> 5U) & 0x07U) * (1.0f / 32.0f);
    }
    constexpr void SetPitchJitter(f32 jitterRatio) noexcept {
        const u8 steps = static_cast<u8>(std::clamp(jitterRatio, 0.0f, 0.21875f) * 32.0f + 0.5f) & 0x07U;
        packedCurveAndJitter = static_cast<u8>((packedCurveAndJitter & 0x1FU) | (steps << 5U));
    }

    [[nodiscard]] constexpr f32 BaseGain() const noexcept {
        return static_cast<f32>(baseGain_q14) * (1.0f / 16384.0f);
    }
    constexpr void SetBaseGain(f32 g) noexcept {
        baseGain_q14 = static_cast<u16>(std::clamp(g, 0.0f, 4.0f) * 16384.0f + 0.5f);
    }

    [[nodiscard]] constexpr f32 BasePitch() const noexcept {
        return static_cast<f32>(basePitch_q12) * (1.0f / 4096.0f);
    }
    constexpr void SetBasePitch(f32 p) noexcept {
        basePitch_q12 = static_cast<u16>(std::clamp(p, 0.125f, 8.0f) * 4096.0f + 0.5f);
    }
};
static_assert(sizeof(SoundCueDef) == 32);

// 32-byte (2 per 64B cache line, zero padding) 4-stem sample-locked interactive music track descriptor.
struct alignas(32) MusicTrackDef {
    u32 trackId{0U};
    AudioHandle stems[kMaxMusicStems]{};
    u16 defaultWeights_q14[kMaxMusicStems]{16384U, 0U, 0U, 0U};
    u16 loopStartFrame{0U};
    u8 stemCount{1U};
    u8 loop{1U};
};
static_assert(sizeof(MusicTrackDef) == 32);

// 32-byte (2 per 64B cache line, zero padding) discrete lifecycle command packed in descending alignment order.
struct alignas(32) AudioCommand {
    const f32* pcmData{nullptr};
    u32 frameCount{0U};
    u16 loopStartFrame{0U};
    u16 sampleRate{48000U};
    u16 initialGainL_q14{16384U};
    u16 initialGainR_q14{16384U};
    u16 initialPitch_q12{4096U};
    u16 generation{1U};
    u16 fadeBlocks{1U};
    AudioCmdType type{AudioCmdType::SpawnVoice};
    AudioBusId bus{AudioBusId::SfxWorld};
    u8 voiceSlot{0U};
    VoiceFlags flags{VoiceFlags::None};
    u8 channels{1U};
    u8 assetScope{0U};
};
static_assert(sizeof(AudioCommand) == 32);

// 576-byte (9 cache lines exact, zero padding) continuous per-frame parameter snapshot.
struct alignas(64) AudioFrameSnapshot {
    std::array<u16, kMaxAudioVoices> targetGainL_q14{};
    std::array<u16, kMaxAudioVoices> targetGainR_q14{};
    std::array<u16, kMaxAudioVoices> targetPitch_q12{};
    std::array<u16, kMaxAudioVoices> voiceCutoffHz{};
    std::array<u16, kAudioBusCount> busGain_q14{16384U, 16384U, 16384U, 16384U, 16384U, 16384U, 16384U, 16384U};
    std::array<u16, kAudioBusCount> busCutoffHz{20000U, 20000U, 20000U, 20000U, 20000U, 20000U, 20000U, 20000U};
    std::array<FilterMode, kAudioBusCount> busFilterMode{};
    std::array<u16, kMaxMusicStems> musicStemWeights_q14{16384U, 0U, 0U, 0U};
    u64 pausedVoicesMask{0ULL};
    u16 sidechainDuckStrength_q14{11468U};
    u16 reverbWetGain_q14{0U};
    u8 reverbRoomSize_q8{180U};
    u8 reverbDamping_q8{100U};
    u8 masterMuted{0U};
    u8 reverbBusSendMask{0U};
};
static_assert(sizeof(AudioFrameSnapshot) == 576);

// 32-byte (2 per 64B cache line, zero padding) real-time mixer telemetry snapshot.
struct alignas(32) AudioTelemetrySnapshot {
    u64 mixedBlocksCount{0ULL};
    f32 masterPeakL{0.0f};
    f32 masterPeakR{0.0f};
    f32 sidechainEnvelope{0.0f};
    f32 reverbPeak{0.0f};
    u16 activeVoiceCount{0U};
    u16 virtualVoiceCount{0U};
    u16 stolenVoiceCount{0U};
    u16 activeMusicStems{0U};
};
static_assert(sizeof(AudioTelemetrySnapshot) == 32);

// 64-byte (1 cache line exact, zero padding) consolidated per-frame update descriptor for AudioSystem::Update().
struct alignas(64) AudioUpdateDesc {
    std::span<const AudioTriggerReq> triggers{};
    const AssetBank* assetBank{nullptr};
    bool (*occlusionQuery)(f32 listenerX, f32 listenerY, f32 emitterX, f32 emitterY, void* userCtx) noexcept{nullptr};
    void* occlusionUserCtx{nullptr};
    const f32* channelScales{nullptr};
    u32 tickIndex{0U};
    f32 listenerX{0.0f};
    f32 listenerY{0.0f};
    u8 frozenChannelMask{0U};
    i8 focusMuteOverride{-1};
    u16 dopplerScale_q8{256U};
};
static_assert(sizeof(AudioUpdateDesc) == 64);

// 16-byte configuration descriptor for AudioSystem::Init().
struct alignas(8) AudioSystemConfig {
    u32 arenaBytes{131072U};
    f32 panWidthMeters{16.0f};
    f32 speedOfSoundMetersPerSec{343.0f};
    u16 occludedLowPassHz{900U};
    u8 defaultMusicCrossfadeBlocks{48U};
    u8 enableAuxReverb{1U};
};
static_assert(sizeof(AudioSystemConfig) == 16);

} // namespace en
