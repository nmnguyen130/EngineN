#include "audio/AudioSystem.h"

#include "assets/AssetBank.h"
#include "assets/AssetSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "gameplay/SpatialIndex2D.h"
#include "platform/Window.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#include <SDL3/SDL_audio.h>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace en::audio {

namespace {

inline constexpr u64 kFibonacciHashMult = 11400714819323198485ULL;
inline constexpr f32 kPi                = 3.14159265358979323846f;
inline constexpr f32 kHalfPi            = 1.57079632679489661923f;

[[nodiscard]] constexpr u32 HashCueKey(u32 cueId) noexcept {
    const u64 prod = static_cast<u64>(cueId ^ 0x9E3779B9u) * kFibonacciHashMult;
    return static_cast<u32>(prod >> (64u - 10u)) & (kCueHashTableSize - 1u);
}

[[nodiscard]] constexpr u16 FloatToQ14(f32 val) noexcept {
    const f32 clamped = std::clamp(val, 0.0f, 3.999f);
    return static_cast<u16>(clamped * 16384.0f + 0.5f);
}

[[nodiscard]] constexpr u16 PrewarpCutoffToQ14(f32 cutoffHz) noexcept {
    const f32 clampedHz = std::clamp(cutoffHz, 40.0f, 20000.0f);
    if (clampedHz >= 19500.0f) {
        return 0u; // Bypass sentinel
    }
    const f32 w = kPi * clampedHz / static_cast<f32>(kSampleRate);
    // 3rd-order Padé approximation to tan(w) on [0, pi * 19500 / 48000]
    const f32 w2 = w * w;
    const f32 g  = w * (15.0f - w2) / (15.0f - 6.0f * w2);
    return FloatToQ14(std::clamp(g, 0.001f, 0.95f));
}

[[nodiscard]] constexpr f32 NextSignedUnitFloat(u32& state) noexcept {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return (static_cast<f32>(state & 0x00FFFFFFu) / 8388608.0f) - 1.0f;
}

inline void EnableHardwareFlushToZero() noexcept {
#if defined(__SSE2__) || defined(_M_X64)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
    u64 fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24u);
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

} // namespace

// Cache-line isolated synchronization bridge residing inside the pre-allocated arena slab.
struct alignas(64) AudioSystem::SharedBridgeState {
    // Wait-free Triple Buffer: bits [1:0]=Front, [3:2]=Spare, [5:4]=Back, bit [6]=hasNewSpare
    alignas(64) std::array<AudioFrameSnapshot, 3> snapshots{};
    alignas(64) std::atomic<u8> tripleFlags{0b00'10'01'00u}; // Back=2, Spare=1, Front=0, dirty=0

    // Wait-free SPSC Command Ring
    alignas(64) std::atomic<u32> cmdWriteIdx{0};
    alignas(64) std::atomic<u32> cmdReadIdx{0};
    alignas(64) std::array<AudioCommand, kCommandRingSize> cmdRing{};

    // Atomic Telemetry Return Channel
    alignas(64) std::atomic<u64> activeVoiceMask{0};
    std::atomic<u32> activeScopePinMask{0};
    std::atomic<u32> stolenVoicesTotal{0};
    std::atomic<f32> masterPeakL{0.0f};
    std::atomic<f32> masterPeakR{0.0f};

    // Main-thread Cue & Voice Tables
    alignas(64) std::array<SoundCueDef, kMaxSoundCues> cueDefs{};
    std::array<u8, kMaxSoundCues> cueScopes{};
    std::array<u64, kMaxSoundCues> cueLastTriggerTick{};
    std::array<u16, kMaxSoundCues> cueLastVoiceSlot{};
    alignas(64) std::array<CueHashSlot, kCueHashTableSize> cueHashTable{};
    u16 cueCount{0};

    std::array<MusicTrackDef, kMaxMusicTracks> musicTracks{};
    u16 musicTrackCount{0};

    std::array<MainVoiceTracker, kMaxVoices> voiceTrackers{};
    AudioFrameSnapshot stagingSnapshot{};
};

AudioSystem::~AudioSystem() noexcept {
    Shutdown();
}

AudioSystem::AudioSystem(AudioSystem&& other) noexcept
    : m_cfg(other.m_cfg)
    , m_stream(std::exchange(other.m_stream, nullptr))
    , m_arenaSlab(std::exchange(other.m_arenaSlab, nullptr))
    , m_bridge(std::exchange(other.m_bridge, nullptr))
    , m_mixer(std::exchange(other.m_mixer, AudioMixer{}))
    , m_listenerX(other.m_listenerX)
    , m_listenerY(other.m_listenerY)
    , m_currentTick(other.m_currentTick)
    , m_rngState(other.m_rngState)
    , m_pendingUnpinScopeMask(other.m_pendingUnpinScopeMask)
    , m_nextStealPoolIdx(other.m_nextStealPoolIdx)
    , m_mutedByFocus(other.m_mutedByFocus) {}

AudioSystem& AudioSystem::operator=(AudioSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_cfg                   = other.m_cfg;
        m_stream                = std::exchange(other.m_stream, nullptr);
        m_arenaSlab             = std::exchange(other.m_arenaSlab, nullptr);
        m_bridge                = std::exchange(other.m_bridge, nullptr);
        m_mixer                 = std::exchange(other.m_mixer, AudioMixer{});
        m_listenerX             = other.m_listenerX;
        m_listenerY             = other.m_listenerY;
        m_currentTick           = other.m_currentTick;
        m_rngState              = other.m_rngState;
        m_pendingUnpinScopeMask = other.m_pendingUnpinScopeMask;
        m_nextStealPoolIdx      = other.m_nextStealPoolIdx;
        m_mutedByFocus          = other.m_mutedByFocus;
    }
    return *this;
}

Result<void, AudioErrc> AudioSystem::Init(const AudioSystemConfig& cfg) noexcept {
    Shutdown();
    m_cfg = cfg;

    const usize minBytes = sizeof(SharedBridgeState) + AudioMixer::RequiredArenaBytes() + 128u;
    const usize slabSize = std::max<usize>(cfg.arenaBytes, minBytes);

    void* rawMem = std::aligned_alloc(64u, (slabSize + 63u) & ~usize(63u));
    if (!rawMem) {
        return Err(AudioErrc::ArenaExhausted);
    }
    std::memset(rawMem, 0, slabSize);
    m_arenaSlab = static_cast<u8*>(rawMem);

    m_bridge = std::construct_at(reinterpret_cast<SharedBridgeState*>(m_arenaSlab));
    u8* mixerStart = m_arenaSlab + sizeof(SharedBridgeState);
    const usize mixerBytes = slabSize - sizeof(SharedBridgeState);

    if (auto res = m_mixer.Init(std::span<u8>(mixerStart, mixerBytes)); !res) {
        Shutdown();
        return res;
    }

    auto& snap = m_bridge->stagingSnapshot;
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::Master)]    = FloatToQ14(cfg.initialMasterGain);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::Music)]     = FloatToQ14(cfg.initialMusicGain);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::SfxPlayer)] = FloatToQ14(cfg.initialSfxGain);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::SfxCombat)] = FloatToQ14(cfg.initialSfxGain);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::SfxWorld)]  = FloatToQ14(cfg.initialSfxGain);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::Ambience)]  = FloatToQ14(cfg.initialSfxGain);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::Ui)]        = FloatToQ14(cfg.initialSfxGain);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::Voice)]     = FloatToQ14(cfg.initialSfxGain);

    for (u32 ch = 0; ch < 8u; ++ch) {
        snap.channelTimeScaleQ12[ch] = 4096u;
    }
    for (u32 st = 0; st < kMaxMusicStems; ++st) {
        snap.stemWeightsQ14[st] = 16384u;
    }

    m_bridge->snapshots[0] = snap;
    m_bridge->snapshots[1] = snap;
    m_bridge->snapshots[2] = snap;

    if (!cfg.headlessMode) {
        SDL_AudioSpec spec{};
        spec.format   = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq     = static_cast<int>(kSampleRate);

        m_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
            &spec,
            &AudioSystem::AudioStreamCallback,
            this);
        if (!m_stream) {
            Shutdown();
            return Err(AudioErrc::DeviceOpenFailed);
        }
        SDL_ResumeAudioStreamDevice(m_stream);
    }

    return Ok;
}

void AudioSystem::Shutdown() noexcept {
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_arenaSlab) {
        std::free(m_arenaSlab);
        m_arenaSlab = nullptr;
        m_bridge    = nullptr;
    }
}

Result<void, AudioErrc> AudioSystem::RegisterCueDef(
    const SoundCueDef& def,
    assets::ScopeId owningScope) noexcept {
    if (!m_bridge || def.cueId.value == 0u) {
        return Err(AudioErrc::InvalidConfig);
    }
    if (m_bridge->cueCount >= kMaxSoundCues) {
        return Err(AudioErrc::CueTableFull);
    }

    const u16 newIdx = m_bridge->cueCount++;
    m_bridge->cueDefs[newIdx]            = def;
    m_bridge->cueScopes[newIdx]          = owningScope;
    m_bridge->cueLastTriggerTick[newIdx] = 0ULL;
    m_bridge->cueLastVoiceSlot[newIdx]   = 0xFFFFu;

    // Flat power-of-2 Robin Hood insertion with probe sequence length (PSL) swapping.
    u32 slotIdx = HashCueKey(def.cueId.value);
    CueHashSlot incoming{def.cueId.value, newIdx, 1u};

    for (u32 probe = 0; probe < kCueHashTableSize; ++probe) {
        CueHashSlot& existing = m_bridge->cueHashTable[slotIdx];
        if (existing.psl == 0u) {
            existing = incoming;
            return Ok;
        }
        if (existing.key == incoming.key) {
            m_bridge->cueDefs[existing.cueIdx]   = def;
            m_bridge->cueScopes[existing.cueIdx] = owningScope;
            --m_bridge->cueCount;
            return Ok;
        }
        if (incoming.psl > existing.psl) {
            std::swap(incoming, existing);
        }
        ++incoming.psl;
        slotIdx = (slotIdx + 1u) & (kCueHashTableSize - 1u);
    }
    return Err(AudioErrc::CueTableFull);
}

Result<void, AudioErrc> AudioSystem::RegisterMusicTrackDef(const MusicTrackDef& def) noexcept {
    if (!m_bridge || m_bridge->musicTrackCount >= kMaxMusicTracks) {
        return Err(AudioErrc::CueTableFull);
    }
    m_bridge->musicTracks[m_bridge->musicTrackCount++] = def;
    return Ok;
}

Result<void, AudioErrc> AudioSystem::RegisterCueBank(
    const assets::BlobRecord& cueBlob,
    assets::ScopeId owningScope) noexcept {
    if (!cueBlob.data || cueBlob.size < sizeof(CookedAudioBankHeader)) {
        return Err(AudioErrc::CorruptedBankBlob);
    }
    const auto* hdr = reinterpret_cast<const CookedAudioBankHeader*>(cueBlob.data);
    if (hdr->magic != kCookedAudioMagic || hdr->version != 1u) {
        return Err(AudioErrc::CorruptedBankBlob);
    }

    const usize expectedSize = sizeof(CookedAudioBankHeader) +
                               (static_cast<usize>(hdr->cueCount) * sizeof(SoundCueDef)) +
                               (static_cast<usize>(hdr->musicTrackCount) * sizeof(MusicTrackDef));
    if (cueBlob.size < expectedSize) {
        return Err(AudioErrc::CorruptedBankBlob);
    }

    const u8* ptr = cueBlob.data + sizeof(CookedAudioBankHeader);
    const auto* cues = reinterpret_cast<const SoundCueDef*>(ptr);
    for (u16 i = 0; i < hdr->cueCount; ++i) {
        if (auto res = RegisterCueDef(cues[i], owningScope); !res) {
            return res;
        }
    }

    ptr += static_cast<usize>(hdr->cueCount) * sizeof(SoundCueDef);
    const auto* tracks = reinterpret_cast<const MusicTrackDef*>(ptr);
    for (u16 i = 0; i < hdr->musicTrackCount; ++i) {
        if (auto res = RegisterMusicTrackDef(tracks[i]); !res) {
            return res;
        }
    }
    return Ok;
}

const SoundCueDef* AudioSystem::FindCueDef(SoundCueId id, u16& outIndex) const noexcept {
    if (!m_bridge || id.value == 0u) {
        return nullptr;
    }
    u32 slotIdx = HashCueKey(id.value);
    u16 psl     = 1u;
    for (u32 probe = 0; probe < kCueHashTableSize; ++probe) {
        const CueHashSlot& slot = m_bridge->cueHashTable[slotIdx];
        if (slot.psl == 0u || psl > slot.psl) {
            return nullptr; // Robin Hood early-out invariant
        }
        if (slot.key == id.value) {
            outIndex = slot.cueIdx;
            return &m_bridge->cueDefs[slot.cueIdx];
        }
        ++psl;
        slotIdx = (slotIdx + 1u) & (kCueHashTableSize - 1u);
    }
    return nullptr;
}

bool AudioSystem::EnqueueCommand(const AudioCommand& cmd) noexcept {
    if (!m_bridge) {
        return false;
    }
    const u32 writeIdx = m_bridge->cmdWriteIdx.load(std::memory_order_relaxed);
    const u32 readIdx  = m_bridge->cmdReadIdx.load(std::memory_order_acquire);
    if (writeIdx - readIdx >= kCommandRingSize) {
        return false;
    }
    m_bridge->cmdRing[writeIdx & (kCommandRingSize - 1u)] = cmd;
    m_bridge->cmdWriteIdx.store(writeIdx + 1u, std::memory_order_release);
    return true;
}

void AudioSystem::RequestScopeRelease(assets::ScopeId scope, assets::AssetSystem& assetSystem) noexcept {
    if (scope >= assets::kMaxScopes || !m_bridge) {
        return;
    }
    const u32 bit    = 1u << scope;
    const u32 pinned = m_bridge->activeScopePinMask.load(std::memory_order_acquire);
    if ((pinned & bit) == 0u) {
        assetSystem.EndScope(scope);
    } else {
        m_pendingUnpinScopeMask |= bit;
    }
}

void AudioSystem::OnWindowEvent(platform::WindowFlags flags, const platform::WindowState& winState) noexcept {
    if (!m_bridge || !m_cfg.muteOnFocusLoss) {
        return;
    }
    if (platform::HasFlag(flags, platform::WindowFlags::FocusLost) || winState.suspended) {
        m_mutedByFocus = true;
    } else if (platform::HasFlag(flags, platform::WindowFlags::FocusGained) && !winState.suspended) {
        m_mutedByFocus = false;
    }
    m_bridge->stagingSnapshot.masterMuteFlag = m_mutedByFocus ? 1u : 0u;
}

void AudioSystem::ComputeVoiceSpatialTargets(u32 slot, AudioFrameSnapshot& snap) const noexcept {
    const MainVoiceTracker& tr = m_bridge->voiceTrackers[slot];
    if (!tr.active) {
        snap.targetGainL_Q14[slot] = 0u;
        snap.targetGainR_Q14[slot] = 0u;
        return;
    }

    f32 atten   = 1.0f;
    f32 panL    = 0.70710678f;
    f32 panR    = 0.70710678f;
    f32 airCut  = 20000.0f;

    const VoiceFlags vf = static_cast<VoiceFlags>(tr.flags);
    if (HasVoiceFlag(vf, VoiceFlags::Spatialized2D) &&
        static_cast<AttenuationCurve>(tr.curve) != AttenuationCurve::None) {
        const f32 dx   = tr.worldX - m_listenerX;
        const f32 dy   = tr.worldY - m_listenerY;
        const f32 dist = std::sqrt(dx * dx + dy * dy);

        if (dist >= tr.maxDist) {
            atten = 0.0f;
        } else if (dist > tr.minDist) {
            const f32 t = (dist - tr.minDist) / std::max(0.01f, tr.maxDist - tr.minDist);
            switch (static_cast<AttenuationCurve>(tr.curve)) {
            case AttenuationCurve::Linear:
                atten = 1.0f - t;
                break;
            case AttenuationCurve::InverseClamped:
                atten = (1.0f - t) / (1.0f + 2.5f * t);
                break;
            case AttenuationCurve::Smoothstep:
                atten = 1.0f - (t * t * (3.0f - 2.0f * t));
                break;
            case AttenuationCurve::None:
                break;
            }
            // Air absorption naturally rolls off brittle high frequencies with distance.
            airCut = 20000.0f * (1.0f - 0.65f * t);
        }

        const f32 panNorm  = std::clamp(dx / std::max(1.0f, m_cfg.panWidthMeters), -1.0f, 1.0f);
        const f32 panAngle = 0.25f * kPi * (panNorm + 1.0f);
        panL               = std::cos(panAngle);
        panR               = std::sin(panAngle);
    }

    const f32 occGain   = HasVoiceFlag(vf, VoiceFlags::BypassOcclusion) ? 1.0f : tr.occlusionFactor;
    const f32 totalGain = tr.baseGain * atten * (0.45f + 0.55f * occGain);
    const f32 cutoffHz  = std::min(airCut, 900.0f + 19100.0f * occGain);

    snap.targetGainL_Q14[slot] = FloatToQ14(totalGain * panL);
    snap.targetGainR_Q14[slot] = FloatToQ14(totalGain * panR);
    snap.targetPitchQ12[slot]  = static_cast<u16>(std::clamp(tr.basePitch * 4096.0f, 1024.0f, 8192.0f));
    snap.targetSvfG_Q14[slot]  = PrewarpCutoffToQ14(cutoffHz);
}

VoiceHandle AudioSystem::PlayCue(
    SoundCueId cueId,
    const assets::AssetBank& assetBank,
    f32 worldX,
    f32 worldY,
    world::EntityId emitter) noexcept {
    if (!m_bridge) {
        return {};
    }

    u16 cueIdx = 0;
    const SoundCueDef* def = FindCueDef(cueId, cueIdx);
    if (!def || def->VariationCount() == 0u) {
        return {};
    }

    // Stage 1: Cooldown & Same-Tick Energy-Summing Coalescer
    const u64 lastTick = m_bridge->cueLastTriggerTick[cueIdx];
    const u32 cooldown = def->CooldownTicks();
    if (lastTick != 0ULL && m_currentTick >= lastTick && (m_currentTick - lastTick) <= cooldown) {
        const u16 prevSlot = m_bridge->cueLastVoiceSlot[cueIdx];
        if (prevSlot < kMaxVoices && m_bridge->voiceTrackers[prevSlot].active &&
            m_bridge->voiceTrackers[prevSlot].cueId == cueId.value) {
            auto& prevTr    = m_bridge->voiceTrackers[prevSlot];
            prevTr.baseGain = std::min(prevTr.baseGain * 1.1892f, 1.4142f); // Cap energy sum at +3 dB
            ComputeVoiceSpatialTargets(prevSlot, m_bridge->stagingSnapshot);
            return VoiceHandle{prevSlot, prevTr.generation};
        }
        return {};
    }

    const u8 varCount = def->VariationCount();
    const u8 varSlot  = static_cast<u8>((m_rngState++) % varCount);
    const assets::AudioRecord& rec = assetBank.GetAudio(def->variations[varSlot]);
    if (!rec.pcm || rec.frameCount == 0u) {
        return {};
    }

    // Stage 2: Concurrency & Physical Slot Selection
    const u64 hwActiveMask = m_bridge->activeVoiceMask.load(std::memory_order_acquire);
    u32 chosenSlot         = 0xFFFFFFFFu;
    u8  sameCueCount       = 0u;
    u32 oldestSameCueSlot  = 0xFFFFFFFFu;
    u64 oldestSameCueTick  = 0xFFFFFFFFFFFFFFFFULL;

    for (u32 s = 0; s < kMaxVoices; ++s) {
        auto& tr = m_bridge->voiceTrackers[s];
        if (tr.active && (hwActiveMask & (1ULL << s)) == 0ULL && (m_currentTick > tr.spawnTick + 2u)) {
            tr.active = false;
        }
        if (!tr.active && chosenSlot == 0xFFFFFFFFu) {
            chosenSlot = s;
        } else if (tr.active && tr.cueId == cueId.value) {
            ++sameCueCount;
            if (tr.spawnTick < oldestSameCueTick) {
                oldestSameCueTick = tr.spawnTick;
                oldestSameCueSlot = s;
            }
        }
    }

    bool stoleActiveVoice = false;
    if (sameCueCount >= def->MaxConcurrency() && oldestSameCueSlot < kMaxVoices) {
        chosenSlot       = oldestSameCueSlot;
        stoleActiveVoice = true;
    } else if (chosenSlot == 0xFFFFFFFFu) {
        if (def->Steal() == StealPolicy::Reject) {
            return {};
        }
        f32 lowestScore = 1.0e9f;
        for (u32 s = 0; s < kMaxVoices; ++s) {
            const auto& tr = m_bridge->voiceTrackers[s];
            if (tr.priority > def->priority) {
                continue;
            }
            const f32 score = static_cast<f32>(tr.priority) * 4.0f + tr.baseGain;
            if (score < lowestScore) {
                lowestScore = score;
                chosenSlot  = s;
            }
        }
        if (chosenSlot == 0xFFFFFFFFu) {
            return {};
        }
        stoleActiveVoice = true;
    }

    // Stage 3: Emit Steal Crossfade (if replacing an active voice) + Spawn Command
    auto& tr = m_bridge->voiceTrackers[chosenSlot];
    if (stoleActiveVoice) {
        AudioCommand stealCmd{};
        stealCmd.type                      = AudioCmdType::StopVoice;
        stealCmd.voiceSlot                 = static_cast<u16>(chosenSlot);
        stealCmd.generation                = tr.generation;
        stealCmd.payload.stop.stealPoolIdx = m_nextStealPoolIdx;
        m_nextStealPoolIdx                 = static_cast<u8>((m_nextStealPoolIdx + 1u) & (kStealPoolSlots - 1u));
        EnqueueCommand(stealCmd);
    }

    const u16 nextGen = static_cast<u16>(tr.generation + 1u == 0u ? 1u : tr.generation + 1u);
    const f32 gainJitter = (static_cast<f32>(def->gainVarQ8) / 255.0f) * NextSignedUnitFloat(m_rngState);
    const f32 semiJitter = (static_cast<f32>(def->pitchVarSemitonesQ4) / 16.0f) * NextSignedUnitFloat(m_rngState);
    const f32 totalSemi  = (static_cast<f32>(def->basePitchCents) / 100.0f) + semiJitter;

    tr.cueId           = cueId.value;
    tr.emitter         = emitter;
    tr.spawnTick       = m_currentTick;
    tr.worldX          = worldX;
    tr.worldY          = worldY;
    tr.baseGain        = std::max(0.0f, (static_cast<f32>(def->baseGainQ14) / 16384.0f) + gainJitter);
    tr.basePitch       = std::exp2(totalSemi / 12.0f);
    tr.minDist         = static_cast<f32>(def->minDistQ2) * 0.25f;
    tr.maxDist         = std::max(tr.minDist + 0.5f, static_cast<f32>(def->maxDistQ1) * 0.5f);
    tr.occlusionFactor = 1.0f;
    tr.generation      = nextGen;
    tr.priority        = def->priority;
    tr.curve           = static_cast<u8>(def->Curve());
    tr.flags           = static_cast<u8>(def->Flags());
    tr.active          = true;

    m_bridge->cueLastTriggerTick[cueIdx] = m_currentTick;
    m_bridge->cueLastVoiceSlot[cueIdx]   = static_cast<u16>(chosenSlot);

    ComputeVoiceSpatialTargets(chosenSlot, m_bridge->stagingSnapshot);

    AudioCommand spawnCmd{};
    spawnCmd.type                = AudioCmdType::SpawnVoice;
    spawnCmd.packedBusAndChannel = static_cast<u8>(
        (static_cast<u8>(def->Bus()) & 0x07u) | ((static_cast<u8>(def->Channel()) & 0x07u) << 3u));
    spawnCmd.voiceSlot           = static_cast<u16>(chosenSlot);
    spawnCmd.generation          = nextGen;
    spawnCmd.flags               = tr.flags;
    spawnCmd.assetScopeId        = m_bridge->cueScopes[cueIdx];
    spawnCmd.payload.spawn.pcm            = rec.pcm;
    spawnCmd.payload.spawn.frameCount     = rec.frameCount;
    spawnCmd.payload.spawn.loopStartFrame = 0u;
    spawnCmd.payload.spawn.sampleRate     = rec.sampleRate;
    spawnCmd.payload.spawn.gainL_Q14      = m_bridge->stagingSnapshot.targetGainL_Q14[chosenSlot];
    spawnCmd.payload.spawn.gainR_Q14      = m_bridge->stagingSnapshot.targetGainR_Q14[chosenSlot];
    spawnCmd.payload.spawn.pitchRatioQ12  = m_bridge->stagingSnapshot.targetPitchQ12[chosenSlot];

    EnqueueCommand(spawnCmd);
    return VoiceHandle{static_cast<u16>(chosenSlot), nextGen};
}

void AudioSystem::ConsumeSimCues(
    std::span<const gameplay::SimCue> cues,
    const world::World&,
    const assets::AssetBank& assetBank) noexcept {
    for (const auto& cue : cues) {
        if (cue.kind == gameplay::SimCueKind::AudioOneShot && cue.assetId.value != 0u) {
            PlayCue(SoundCueId::FromAssetId(cue.assetId), assetBank, cue.worldX, cue.worldY);
        }
    }
}

void AudioSystem::StopVoice(VoiceHandle handle, u16 fadeOutBlocks) noexcept {
    if (!m_bridge || !handle.IsValid() || handle.index >= kMaxVoices) {
        return;
    }
    auto& tr = m_bridge->voiceTrackers[handle.index];
    if (!tr.active || tr.generation != handle.generation) {
        return;
    }
    tr.active = false;

    AudioCommand cmd{};
    cmd.type                        = AudioCmdType::StopVoice;
    cmd.voiceSlot                   = handle.index;
    cmd.generation                  = handle.generation;
    cmd.payload.stop.fadeOutBlocks  = fadeOutBlocks;
    cmd.payload.stop.stealPoolIdx   = 0xFFu;
    EnqueueCommand(cmd);
}

void AudioSystem::SetBusGain(AudioBusId bus, f32 linearGain) noexcept {
    if (!m_bridge || static_cast<usize>(bus) >= kAudioBusCount) {
        return;
    }
    m_bridge->stagingSnapshot.busTargetGainQ14[static_cast<usize>(bus)] = FloatToQ14(linearGain);
}

void AudioSystem::SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept {
    if (!m_bridge || static_cast<usize>(bus) >= kAudioBusCount) {
        return;
    }
    const usize idx = static_cast<usize>(bus);
    m_bridge->stagingSnapshot.busSvfG_Q14[idx] = PrewarpCutoffToQ14(cutoffHz);
    const u16 clearMask = static_cast<u16>(~(0x03u << (idx * 2u)));
    m_bridge->stagingSnapshot.busFilterModesBits =
        static_cast<u16>((m_bridge->stagingSnapshot.busFilterModesBits & clearMask) |
                         ((static_cast<u16>(mode) & 0x03u) << (idx * 2u)));
}

void AudioSystem::TransitionMusic(u32 trackId, const assets::AssetBank& assetBank, u8 crossfadeBlocks) noexcept {
    if (!m_bridge) {
        return;
    }
    const MusicTrackDef* found = nullptr;
    for (u16 i = 0; i < m_bridge->musicTrackCount; ++i) {
        if (m_bridge->musicTracks[i].trackId == trackId) {
            found = &m_bridge->musicTracks[i];
            break;
        }
    }
    if (!found) {
        return;
    }

    const u8 stems = found->StemCount();
    for (u8 s = 0; s < stems; ++s) {
        const assets::AudioRecord& rec = assetBank.GetAudio(found->stems[s]);
        if (!rec.pcm || rec.frameCount == 0u) {
            continue;
        }
        AudioCommand bindCmd{};
        bindCmd.type                          = AudioCmdType::BindMusicStem;
        bindCmd.payload.music.pcm             = rec.pcm;
        bindCmd.payload.music.frameCount      = rec.frameCount;
        bindCmd.payload.music.loopStartFrame  = found->loopStartFrame;
        bindCmd.payload.music.loopEndFrame    = found->loopEndFrame;
        bindCmd.payload.music.tempoBpmQ8      = found->tempoBpmQ8;
        bindCmd.payload.music.deckAndStemIdx  = static_cast<u8>(0x80u | (s & 0x03u)); // Deck B
        bindCmd.payload.music.crossfadeBlocks = crossfadeBlocks;
        EnqueueCommand(bindCmd);
    }

    AudioCommand commitCmd{};
    commitCmd.type                          = AudioCmdType::CommitMusicDeck;
    commitCmd.payload.music.crossfadeBlocks = crossfadeBlocks;
    EnqueueCommand(commitCmd);
}

void AudioSystem::SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept {
    if (!m_bridge) {
        return;
    }
    for (usize s = 0; s < kMaxMusicStems; ++s) {
        m_bridge->stagingSnapshot.stemWeightsQ14[s] = FloatToQ14(weights[s]);
    }
}

void AudioSystem::PublishFrameSnapshot(const AudioFrameSnapshot& snap) noexcept {
    u8 flags        = m_bridge->tripleFlags.load(std::memory_order_relaxed);
    const u8 backId = static_cast<u8>((flags >> 4u) & 0x03u);
    m_bridge->snapshots[backId] = snap;

    // Wait-free atomic swap of Back and Spare indices with dirty bit [6] set.
    while (true) {
        const u8 frontId = static_cast<u8>(flags & 0x03u);
        const u8 spareId = static_cast<u8>((flags >> 2u) & 0x03u);
        const u8 curBack = static_cast<u8>((flags >> 4u) & 0x03u);
        const u8 desired = static_cast<u8>(frontId | (curBack << 2u) | (spareId << 4u) | 0x40u);
        if (m_bridge->tripleFlags.compare_exchange_weak(
                flags, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            break;
        }
    }
}

void AudioSystem::Update(
    const world::World&,
    const gameplay::SpatialIndex2D& spatial,
    const assets::AssetBank&,
    time::RenderTimeView timeView,
    u8 frozenChannelMask,
    std::span<const f32, 8> channelScales) noexcept {
    if (!m_bridge) {
        return;
    }
    m_currentTick = timeView.tickIndex;

    auto& snap             = m_bridge->stagingSnapshot;
    snap.frozenChannelMask = frozenChannelMask;
    for (usize ch = 0; ch < 8u; ++ch) {
        snap.channelTimeScaleQ12[ch] =
            static_cast<u16>(std::clamp(channelScales[ch] * 4096.0f, 1024.0f, 8192.0f));
    }

    // Staggered 30 Hz raycast occlusion across 16 voices per frame + spatial target update.
    const u32 staggerGroup = static_cast<u32>(m_currentTick & 3ULL);
    for (u32 slot = 0; slot < kMaxVoices; ++slot) {
        auto& tr = m_bridge->voiceTrackers[slot];
        if (!tr.active) {
            continue;
        }
        const VoiceFlags vf = static_cast<VoiceFlags>(tr.flags);
        if ((slot & 3u) == staggerGroup &&
            HasVoiceFlag(vf, VoiceFlags::Spatialized2D) &&
            !HasVoiceFlag(vf, VoiceFlags::BypassOcclusion)) {
            const f32 dx   = tr.worldX - m_listenerX;
            const f32 dy   = tr.worldY - m_listenerY;
            const f32 dist = std::sqrt(dx * dx + dy * dy);
            float targetOcc = 1.0f;
            if (dist > 0.5f && dist < tr.maxDist) {
                const auto hit = spatial.Raycast(m_listenerX, m_listenerY, dx / dist, dy / dist, dist, 0xFFFFu, tr.emitter);
                if (hit.hit) {
                    targetOcc = 0.0f;
                }
            }
            tr.occlusionFactor += (targetOcc - tr.occlusionFactor) * 0.25f;
        }
        ComputeVoiceSpatialTargets(slot, snap);
    }

    ++snap.snapshotSeq;
    PublishFrameSnapshot(snap);
}

void AudioSystem::ExecuteAudioCallback(SDL_AudioStream* stream, int additionalBytes) noexcept {
    EnableHardwareFlushToZero();

    // 1. Swap Spare -> Front in wait-free Triple Buffer if main thread published a newer snapshot.
    u8 flags = m_bridge->tripleFlags.load(std::memory_order_relaxed);
    while ((flags & 0x40u) != 0u) {
        const u8 frontId = static_cast<u8>(flags & 0x03u);
        const u8 spareId = static_cast<u8>((flags >> 2u) & 0x03u);
        const u8 backId  = static_cast<u8>((flags >> 4u) & 0x03u);
        const u8 desired = static_cast<u8>(spareId | (frontId << 2u) | (backId << 4u));
        if (m_bridge->tripleFlags.compare_exchange_weak(
                flags, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            flags = desired;
            break;
        }
    }
    const AudioFrameSnapshot& frontSnap = m_bridge->snapshots[flags & 0x03u];

    // 2. Drain discrete lifecycle commands from the SPSC ring buffer.
    u32 readIdx        = m_bridge->cmdReadIdx.load(std::memory_order_relaxed);
    const u32 writeIdx = m_bridge->cmdWriteIdx.load(std::memory_order_acquire);
    while (readIdx != writeIdx) {
        m_mixer.ApplyCommand(m_bridge->cmdRing[readIdx & (kCommandRingSize - 1u)]);
        ++readIdx;
    }
    m_bridge->cmdReadIdx.store(readIdx, std::memory_order_release);

    // 3. Mix 256-frame blocks and push to SDL_AudioStream.
    alignas(64) std::array<f32, kMixBlockFrames * 2> blockOut{};
    const int bytesPerBlock = static_cast<int>(sizeof(f32) * kMixBlockFrames * 2u);
    int bytesRemaining      = std::max(additionalBytes, bytesPerBlock);

    while (bytesRemaining > 0) {
        m_mixer.MixBlock(frontSnap, blockOut);
        if (stream) {
            SDL_PutAudioStreamData(stream, blockOut.data(), bytesPerBlock);
        }
        bytesRemaining -= bytesPerBlock;
    }

    const auto& tel = m_mixer.Telemetry();
    m_bridge->activeVoiceMask.store(tel.activeVoiceMask, std::memory_order_release);
    m_bridge->activeScopePinMask.store(tel.activeScopePinMask, std::memory_order_release);
    m_bridge->stolenVoicesTotal.store(tel.stolenVoicesTotal, std::memory_order_relaxed);
    m_bridge->masterPeakL.store(tel.masterPeakL, std::memory_order_relaxed);
    m_bridge->masterPeakR.store(tel.masterPeakR, std::memory_order_relaxed);
}

void AudioSystem::PumpHeadlessBlocks(u32 blockCount, std::span<f32> optionalCaptureOut) noexcept {
    if (!m_bridge) {
        return;
    }
    u8 flags = m_bridge->tripleFlags.load(std::memory_order_relaxed);
    if ((flags & 0x40u) != 0u) {
        const u8 frontId = static_cast<u8>(flags & 0x03u);
        const u8 spareId = static_cast<u8>((flags >> 2u) & 0x03u);
        const u8 backId  = static_cast<u8>((flags >> 4u) & 0x03u);
        flags            = static_cast<u8>(spareId | (frontId << 2u) | (backId << 4u));
        m_bridge->tripleFlags.store(flags, std::memory_order_release);
    }
    const AudioFrameSnapshot& frontSnap = m_bridge->snapshots[flags & 0x03u];

    u32 readIdx        = m_bridge->cmdReadIdx.load(std::memory_order_relaxed);
    const u32 writeIdx = m_bridge->cmdWriteIdx.load(std::memory_order_acquire);
    while (readIdx != writeIdx) {
        m_mixer.ApplyCommand(m_bridge->cmdRing[readIdx & (kCommandRingSize - 1u)]);
        ++readIdx;
    }
    m_bridge->cmdReadIdx.store(readIdx, std::memory_order_release);

    alignas(64) std::array<f32, kMixBlockFrames * 2> blockOut{};
    usize outOffset = 0u;
    for (u32 b = 0; b < blockCount; ++b) {
        m_mixer.MixBlock(frontSnap, blockOut);
        if (outOffset + blockOut.size() <= optionalCaptureOut.size()) {
            std::memcpy(optionalCaptureOut.data() + outOffset, blockOut.data(), blockOut.size() * sizeof(f32));
            outOffset += blockOut.size();
        }
    }

    const auto& tel = m_mixer.Telemetry();
    m_bridge->activeVoiceMask.store(tel.activeVoiceMask, std::memory_order_release);
    m_bridge->activeScopePinMask.store(tel.activeScopePinMask, std::memory_order_release);
    m_bridge->stolenVoicesTotal.store(tel.stolenVoicesTotal, std::memory_order_relaxed);
    m_bridge->masterPeakL.store(tel.masterPeakL, std::memory_order_relaxed);
    m_bridge->masterPeakR.store(tel.masterPeakR, std::memory_order_relaxed);
}

AudioTelemetrySnapshot AudioSystem::Telemetry() const noexcept {
    if (!m_bridge) {
        return {};
    }
    AudioTelemetrySnapshot out{};
    out.activeVoiceMask    = m_bridge->activeVoiceMask.load(std::memory_order_acquire);
    out.activeScopePinMask = m_bridge->activeScopePinMask.load(std::memory_order_acquire);
    out.stolenVoicesTotal  = m_bridge->stolenVoicesTotal.load(std::memory_order_relaxed);
    out.masterPeakL        = m_bridge->masterPeakL.load(std::memory_order_relaxed);
    out.masterPeakR        = m_bridge->masterPeakR.load(std::memory_order_relaxed);
    return out;
}

void AudioSystem::AudioStreamCallback(
    void* userdata,
    SDL_AudioStream* stream,
    int additionalAmount,
    int) noexcept {
    if (userdata && additionalAmount > 0) {
        static_cast<AudioSystem*>(userdata)->ExecuteAudioCallback(stream, additionalAmount);
    }
}

} // namespace en::audio