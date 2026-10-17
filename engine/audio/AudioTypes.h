#pragma once

#include "assets/AssetTypes.h"
#include "core/Result.h"
#include "core/Types.h"
#include "core/time/TimeTypes.h"

#include <array>
#include <span>

namespace en::audio {

inline constexpr u32 kSampleRate        = 48000;
inline constexpr u32 kMixBlockFrames    = 256;
inline constexpr u32 kMaxVoices         = 64;
inline constexpr u32 kStealPoolSlots    = 16;
inline constexpr u32 kMaxVirtualVoices  = 128;
inline constexpr u32 kMaxSoundCues      = 512;
inline constexpr u32 kCueHashTableSize  = 1024;
inline constexpr u32 kMaxMusicTracks    = 32;
inline constexpr u32 kMaxMusicStems     = 4;
inline constexpr u32 kCommandRingSize   = 256;
inline constexpr u32 kCookedAudioMagic  = 0x44554145u; // 'EAUD'
inline constexpr u32 kNullEmitter       = 0xFFFFFFFFu;

enum class AudioErrc : u8 {
    InvalidConfig = 0,
    ArenaExhausted,
    DeviceOpenFailed,
    CorruptedBankBlob,
    CueTableFull,
    VoicePoolExhausted
};

enum class AudioBusId : u8 {
    Master = 0,
    Music,
    SfxPlayer,
    SfxCombat,
    SfxWorld,
    Ambience,
    Ui,
    Voice,
    Count
};

inline constexpr usize kAudioBusCount = static_cast<usize>(AudioBusId::Count);

enum class FilterMode : u8 {
    Bypass = 0,
    LowPass,
    HighPass,
    BandPass
};

enum class AttenuationCurve : u8 {
    None = 0,
    Linear,
    InverseClamped,
    Smoothstep
};

enum class StealPolicy : u8 {
    Reject = 0,
    StealOldest,
    StealQuietest,
    Virtualize
};

enum class VoiceFlags : u8 {
    None              = 0,
    Looping           = 1 << 0,
    Spatialized2D     = 1 << 1,
    FollowTimeScale   = 1 << 2,
    PauseOnHitstop    = 1 << 3,
    BypassOcclusion   = 1 << 4,
    OneVoicePerEntity = 1 << 5
};

[[nodiscard]] constexpr VoiceFlags operator|(VoiceFlags a, VoiceFlags b) noexcept {
    return static_cast<VoiceFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}

[[nodiscard]] constexpr bool HasVoiceFlag(VoiceFlags val, VoiceFlags mask) noexcept {
    return (static_cast<u8>(val) & static_cast<u8>(mask)) != 0;
}

struct VoiceHandle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }
    constexpr bool operator==(const VoiceHandle&) const noexcept = default;
};

struct SoundCueId {
    u32 value{0};

    [[nodiscard]] static constexpr SoundCueId FromAssetId(assets::AssetId id) noexcept {
        return SoundCueId{static_cast<u32>(id.value ^ (id.value >> 32u))};
    }

    constexpr bool operator==(const SoundCueId&) const noexcept = default;
};

// Branchless exact-center constant-power stereo panner on panNorm in [-1, 1] (<0.09 dB power error).
// Preserves exact -3.0103 dB (0.70710678) at center (panNorm = 0) and unit power at hard L/R extremes.
constexpr void EvalConstantPowerPan(f32 panNorm, f32& outL, f32& outR) noexcept {
    constexpr f32 kCenterBoost = 0.8284271247461903f; // 2.0f * (sqrt(2.0f) - 1.0f)
    const f32 x  = 0.5f * (panNorm + 1.0f);
    const f32 x1 = 1.0f - x;
    outL = x1 * (1.0f + kCenterBoost * x);
    outR = x  * (1.0f + kCenterBoost * x1);
}

// Decoupled 16-byte trigger payload bridged from simulation cues without exposing gameplay headers.
struct alignas(16) AudioTriggerReq {
    SoundCueId cueId{};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    u32 emitterId{kNullEmitter};
};
static_assert(sizeof(AudioTriggerReq) == 16);

// Decoupled 16-byte active emitter snapshot exported for debug overlays and spatial telemetry.
struct alignas(16) AudioEmitterView {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 maxDist{0.0f};
    f32 occlusionFactor{1.0f};
};
static_assert(sizeof(AudioEmitterView) == 16);

// Main-thread raycast callback for staggered 30 Hz acoustic occlusion.
// Invariant: Invoked strictly inside AudioSystem::Update() on the main thread; never on the audio thread.
using AudioOcclusionQueryFn = bool (*)(
    const void* userCtx,
    f32 startX,
    f32 startY,
    f32 dirX,
    f32 dirY,
    f32 maxDist,
    u32 ignoreEmitterId) noexcept;

// Consolidated per-frame synchronization payload passed once to AudioSystem::Update().
struct AudioUpdateDesc {
    u64 tickIndex{0};
    f32 listenerX{0.0f};
    f32 listenerY{0.0f};
    std::span<const f32> channelScales{};
    u8 frozenChannelMask{0};
    AudioOcclusionQueryFn occlusionQuery{nullptr};
    const void* occlusionUserCtx{nullptr};
};

// Cooked 32-byte acoustic definition (2 records per 64B cache line, zero padding).
// Quantized fixed-point fields halve L1 cache pressure during high-concurrency combat ticks.
struct alignas(32) SoundCueDef {
    SoundCueId cueId{};                              // 4B  [0..4)
    std::array<assets::AudioHandle, 4> variations{}; // 16B [4..20)
    u16 baseGainQ14{16384};                          // 2B  [20..22) 16384 == 1.0f
    i16 basePitchCents{0};                           // 2B  [22..24) -2400..+2400 cents
    u8  gainVarQ8{12};                               // 1B  [24..25) symmetric linear variance
    u8  pitchVarSemitonesQ4{16};                     // 1B  [25..26) 16 == +/- 1.0 semitone
    u8  minDistQ2{10};                               // 1B  [26..27) 0..63.75m in 0.25m steps
    u8  maxDistQ1{44};                               // 1B  [27..28) 0..127.5m in 0.5m steps
    u8  packedRouting{0x21};                         // 1B  [28..29) [2:0]=variationCount, [5:3]=bus, [7:6]=stealPolicy
    u8  packedChannelAndCurve{0x64};                 // 1B  [29..30) [2:0]=timeChannel, [4:3]=curve, [7:5]=maxConcurrency-1
    u8  priority{128};                               // 1B  [30..31) 0..255
    u8  packedFlagsAndCooldown{0x4A};                // 1B  [31..32) [5:0]=VoiceFlags, [7:6]=cooldownShift

    [[nodiscard]] constexpr u8 VariationCount() const noexcept { return static_cast<u8>(packedRouting & 0x07u); }
    [[nodiscard]] constexpr AudioBusId Bus() const noexcept { return static_cast<AudioBusId>((packedRouting >> 3u) & 0x07u); }
    [[nodiscard]] constexpr StealPolicy Steal() const noexcept { return static_cast<StealPolicy>((packedRouting >> 6u) & 0x03u); }
    [[nodiscard]] constexpr time::TimeChannel Channel() const noexcept {
        return static_cast<time::TimeChannel>(packedChannelAndCurve & 0x07u);
    }
    [[nodiscard]] constexpr AttenuationCurve Curve() const noexcept {
        return static_cast<AttenuationCurve>((packedChannelAndCurve >> 3u) & 0x03u);
    }
    [[nodiscard]] constexpr u8 MaxConcurrency() const noexcept {
        return static_cast<u8>(((packedChannelAndCurve >> 5u) & 0x07u) + 1u);
    }
    [[nodiscard]] constexpr VoiceFlags Flags() const noexcept {
        return static_cast<VoiceFlags>(packedFlagsAndCooldown & 0x3Fu);
    }
    [[nodiscard]] constexpr u32 CooldownTicks() const noexcept {
        const u8 shift = static_cast<u8>((packedFlagsAndCooldown >> 6u) & 0x03u);
        return shift == 0u ? 0u : (1u << shift);
    }
};
static_assert(sizeof(SoundCueDef) == 32);

// Cooked 32-byte interactive music track definition with beat-grid metadata and zero padding.
struct alignas(32) MusicTrackDef {
    u32 trackId{0};                                          // 4B  [0..4)
    std::array<assets::AudioHandle, kMaxMusicStems> stems{}; // 16B [4..20)
    u32 loopStartFrame{0};                                   // 4B  [20..24)
    u32 loopEndFrame{0};                                     // 4B  [24..28)
    u16 tempoBpmQ8{120 << 8};                                // 2B  [28..30) Q8.8 BPM
    u8  packedStemsAndBeats{0x21};                           // 1B  [30..31) [2:0]=stemCount, [7:3]=beatsPerBar
    u8  defaultCrossfadeBlocks{94};                          // 1B  [31..32) in 256-sample blocks (~501 ms)

    [[nodiscard]] constexpr u8 StemCount() const noexcept { return static_cast<u8>(packedStemsAndBeats & 0x07u); }
    [[nodiscard]] constexpr u8 BeatsPerBar() const noexcept { return static_cast<u8>((packedStemsAndBeats >> 3u) & 0x1Fu); }
};
static_assert(sizeof(MusicTrackDef) == 32);

struct alignas(16) CookedAudioBankHeader {
    u32 magic{kCookedAudioMagic};
    u16 version{1};
    u16 cueCount{0};
    u16 musicTrackCount{0};
    u16 reserved{0};
    u32 contentHash{0};
};
static_assert(sizeof(CookedAudioBankHeader) == 16);

enum class AudioCmdType : u8 {
    SpawnVoice = 0,
    StopVoice,
    BindMusicStem,
    CommitMusicDeck,
    TriggerStinger,
    PanicReset
};

// Exact 32-byte discrete lifecycle command passed via the SPSC ring buffer.
// Continuous spatial, filter, and timescale updates use the triple-buffered AudioFrameSnapshot.
struct alignas(32) AudioCommand {
    AudioCmdType type{AudioCmdType::SpawnVoice};     // 1B  [0..1)
    u8  packedBusAndChannel{0};                      // 1B  [1..2) [2:0]=AudioBusId, [5:3]=TimeChannel
    u16 voiceSlot{0};                                // 2B  [2..4) physical slot [0..63]
    u16 generation{0};                               // 2B  [4..6) handle generation
    u8  flags{0};                                    // 1B  [6..7) VoiceFlags bitmask
    u8  assetScopeId{0};                             // 1B  [7..8) ScopeId [0..31]

    union Payload {
        struct Spawn {
            const i16* pcm;                          // 8B  [8..16)
            u32 frameCount;                          // 4B  [16..20)
            u32 loopStartFrame;                      // 4B  [20..24)
            u16 sampleRate;                          // 2B  [24..26)
            u16 gainL_Q14;                           // 2B  [26..28)
            u16 gainR_Q14;                           // 2B  [28..30)
            u16 pitchRatioQ12;                       // 2B  [30..32) 4096 == 1.0x
        } spawn;
        struct Stop {
            u16 fadeOutBlocks;                       // 2B  [8..10)
            u8  stealPoolIdx;                        // 1B  [10..11) [0..15] or 0xFF
            u8  pad[21];                             // 21B [11..32)
        } stop;
        struct MusicStem {
            const i16* pcm;                          // 8B  [8..16)
            u32 frameCount;                          // 4B  [16..20)
            u32 loopStartFrame;                      // 4B  [20..24)
            u32 loopEndFrame;                        // 4B  [24..28)
            u16 tempoBpmQ8;                          // 2B  [28..30)
            u8  deckAndStemIdx;                      // 1B  [30..31) [7]=Deck B/A, [1:0]=stemIdx 0..3
            u8  crossfadeBlocks;                     // 1B  [31..32)
        } music;
    } payload{};
    static_assert(sizeof(Payload) == 24);
};
static_assert(sizeof(AudioCommand) == 32);

// Wait-free triple-buffered continuous parameter snapshot (576 Bytes = 9 x 64B cache lines).
// Written once per frame by the main thread; swapped in O(1) by the audio callback.
struct alignas(64) AudioFrameSnapshot {
    std::array<u16, kMaxVoices> targetGainL_Q14{};      // 128B [0..128)   Cache Lines 0..1
    std::array<u16, kMaxVoices> targetGainR_Q14{};      // 128B [128..256) Cache Lines 2..3
    std::array<u16, kMaxVoices> targetPitchQ12{};       // 128B [256..384) Cache Lines 4..5 (4096 == 1.0x)
    std::array<u16, kMaxVoices> targetSvfG_Q14{};       // 128B [384..512) Cache Lines 6..7 (pre-warped tan(pi*fc/fs))
    std::array<u16, kAudioBusCount> busTargetGainQ14{}; // 16B  [512..528) Cache Line 8
    std::array<u16, kAudioBusCount> busSvfG_Q14{};      // 16B  [528..544)
    std::array<u16, 8> channelTimeScaleQ12{};           // 16B  [544..560)
    std::array<u16, kMaxMusicStems> stemWeightsQ14{};   // 8B   [560..568)
    u16 busFilterModesBits{0};                          // 2B   [568..570) 2 bits per bus
    u8  frozenChannelMask{0};                           // 1B   [570..571)
    u8  masterMuteFlag{0};                              // 1B   [571..572)
    u32 snapshotSeq{0};                                 // 4B   [572..576)
};
static_assert(sizeof(AudioFrameSnapshot) == 576);

struct alignas(32) AudioTelemetrySnapshot {
    u64 activeVoiceMask{0};             // 8B [0..8)   Bit i == 1 if physical voice i is active
    u32 activeScopePinMask{0};          // 4B [8..12)  Bit s == 1 if any active voice reads ScopeId s
    u32 stolenVoicesTotal{0};           // 4B [12..16)
    f32 masterPeakL{0.0f};              // 4B [16..20)
    f32 masterPeakR{0.0f};              // 4B [20..24)
    f32 callbackCpuLoadRatio{0.0f};     // 4B [24..28)
    u16 activeVirtualVoices{0};         // 2B [28..30)
    u16 underrunCount{0};               // 2B [30..32)
};
static_assert(sizeof(AudioTelemetrySnapshot) == 32);

} // namespace en::audio
