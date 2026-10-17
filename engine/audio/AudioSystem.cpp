#include "audio/AudioSystem.h"

#include "assets/AssetBank.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

#include <SDL3/SDL_audio.h>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace en::audio {

namespace {

inline constexpr u64 kFibonacciHashMult = 11400714819323198485ULL;
inline constexpr f32 kPi                = 3.14159265358979323846f;
inline constexpr f32 kInvSqrt2          = 0.70710678118654752440f;

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
    const f32 w  = kPi * clampedHz / static_cast<f32>(kSampleRate);
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

// Swaps either Back<->Spare (publish = true) or Spare<->Front (publish = false) in one wait-free CAS loop.
inline u8 SwapTripleBufferFlags(std::atomic<u8>& tripleFlags, bool isProducerPublish) noexcept {
    u8 flags = tripleFlags.load(std::memory_order_relaxed);
    while (isProducerPublish || (flags & 0x40u) != 0u) {
        const u8 frontId = static_cast<u8>(flags & 0x03u);
        const u8 spareId = static_cast<u8>((flags >> 2u) & 0x03u);
        const u8 backId  = static_cast<u8>((flags >> 4u) & 0x03u);
        const u8 desired = isProducerPublish
                               ? static_cast<u8>(frontId | (backId << 2u) | (spareId << 4u) | 0x40u)
                               : static_cast<u8>(spareId | (frontId << 2u) | (backId << 4u));
        if (tripleFlags.compare_exchange_weak(
                flags, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return desired;
        }
    }
    return flags;
}

} // namespace

// Cache-line isolated synchronization bridge residing inside the pre-allocated arena slab.
struct alignas(64) AudioSystem::SharedBridgeState {
    // Wait-free Triple Buffer: bits [1:0]=Front, [3:2]=Spare, [5:4]=Back, bit [6]=hasNewSpare
    alignas(64) std::array<AudioFrameSnapshot, 3> snapshots{};
    alignas(64) std::atomic<u8> tripleFlags{0b00'10'01'00u};

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
    , m_trackedActiveMask(std::exchange(other.m_trackedActiveMask, 0))
    , m_listenerX(other.m_listenerX)
    , m_listenerY(other.m_listenerY)
    , m_currentTick(other.m_currentTick)
    , m_rngState(other.m_rngState)
    , m_pendingUnpinScopeMask(other.m_pendingUnpinScopeMask)
    , m_nextStealPoolIdx(other.m_nextStealPoolIdx) {}

AudioSystem& AudioSystem::operator=(AudioSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_cfg                   = other.m_cfg;
        m_stream                = std::exchange(other.m_stream, nullptr);
        m_arenaSlab             = std::exchange(other.m_arenaSlab, nullptr);
        m_bridge                = std::exchange(other.m_bridge, nullptr);
        m_mixer                 = std::exchange(other.m_mixer, AudioMixer{});
        m_trackedActiveMask     = std::exchange(other.m_trackedActiveMask, 0);
        m_listenerX             = other.m_listenerX;
        m_listenerY             = other.m_listenerY;
        m_currentTick           = other.m_currentTick;
        m_rngState              = other.m_rngState;
        m_pendingUnpinScopeMask = other.m_pendingUnpinScopeMask;
        m_nextStealPoolIdx      = other.m_nextStealPoolIdx;
    }
    return *this;
}

Result<void, AudioErrc> AudioSystem::Init(const AudioSystemConfig& cfg) noexcept {
    Shutdown();
    m_cfg = cfg;

    const usize minBytes   = sizeof(SharedBridgeState) + AudioMixer::RequiredArenaBytes() + 128u;
    const usize arenaBytes = (std::max<usize>(cfg.arenaBytes, minBytes) + 63u) & ~usize(63u);

    void* rawMem = ::operator new(arenaBytes, std::align_val_t{64}, std::nothrow);
    if (!rawMem) {
        return Err(AudioErrc::ArenaExhausted);
    }
    std::memset(rawMem, 0, arenaBytes);
    m_arenaSlab = static_cast<u8*>(rawMem);

    m_bridge               = std::construct_at(reinterpret_cast<SharedBridgeState*>(m_arenaSlab));
    u8* mixerStart         = m_arenaSlab + sizeof(SharedBridgeState);
    const usize mixerBytes = arenaBytes - sizeof(SharedBridgeState);

    if (auto res = m_mixer.Init(std::span<u8>(mixerStart, mixerBytes)); !res) {
        Shutdown();
        return res;
    }

    auto& snap = m_bridge->stagingSnapshot;
    const u16 sfxQ14 = FloatToQ14(cfg.initialSfxGain);
    snap.busTargetGainQ14.fill(sfxQ14);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::Master)] = FloatToQ14(cfg.initialMasterGain);
    snap.busTargetGainQ14[static_cast<usize>(AudioBusId::Music)]  = FloatToQ14(cfg.initialMusicGain);

    snap.channelTimeScaleQ12.fill(4096u);
    snap.stemWeightsQ14.fill(16384u);

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

    return {};
}

void AudioSystem::Shutdown() noexcept {
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_arenaSlab) {
        ::operator delete(m_arenaSlab, std::align_val_t{64});
        m_arenaSlab         = nullptr;
        m_bridge            = nullptr;
        m_trackedActiveMask = 0;
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

    u32 slotIdx = HashCueKey(def.cueId.value);
    CueHashSlot incoming{def.cueId.value, newIdx, 1u};

    for (u32 probe = 0; probe < kCueHashTableSize; ++probe) {
        CueHashSlot& existing = m_bridge->cueHashTable[slotIdx];
        if (existing.psl == 0u) {
            existing = incoming;
            return {};
        }
        if (existing.key == incoming.key) {
            m_bridge->cueDefs[existing.cueIdx]   = def;
            m_bridge->cueScopes[existing.cueIdx] = owningScope;
            --m_bridge->cueCount;
            return {};
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
    return {};
}

Result<void, AudioErrc> AudioSystem::RegisterCueBank(
    const assets::BlobRecord& cueBlob,
    assets::ScopeId owningScope) noexcept {
    if (!cueBlob.data || cueBlob.size < sizeof(CookedAudioBankHeader)) {
        return Err(AudioErrc::CorruptedBankBlob);
    }
    CookedAudioBankHeader hdr{};
    std::memcpy(&hdr, cueBlob.data, sizeof(CookedAudioBankHeader));
    if (hdr.magic != kCookedAudioMagic || hdr.version != 1u) {
        return Err(AudioErrc::CorruptedBankBlob);
    }

    const usize expectedSize = sizeof(CookedAudioBankHeader) +
                               (static_cast<usize>(hdr.cueCount) * sizeof(SoundCueDef)) +
                               (static_cast<usize>(hdr.musicTrackCount) * sizeof(MusicTrackDef));
    if (cueBlob.size < expectedSize) {
        return Err(AudioErrc::CorruptedBankBlob);
    }

    const u8* ptr = cueBlob.data + sizeof(CookedAudioBankHeader);
    for (u16 i = 0; i < hdr.cueCount; ++i) {
        SoundCueDef def{};
        std::memcpy(&def, ptr, sizeof(SoundCueDef));
        ptr += sizeof(SoundCueDef);
        if (auto res = RegisterCueDef(def, owningScope); !res) {
            return res;
        }
    }

    for (u16 i = 0; i < hdr.musicTrackCount; ++i) {
        MusicTrackDef track{};
        std::memcpy(&track, ptr, sizeof(MusicTrackDef));
        ptr += sizeof(MusicTrackDef);
        if (auto res = RegisterMusicTrackDef(track); !res) {
            return res;
        }
    }
    return {};
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
            return nullptr;
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

void AudioSystem::RequestScopeRelease(assets::ScopeId scope) noexcept {
    if (scope < assets::kMaxScopes && m_bridge) {
        m_pendingUnpinScopeMask |= (1u << scope);
    }
}

u32 AudioSystem::PollReleasableScopeMask() noexcept {
    if (!m_bridge || m_pendingUnpinScopeMask == 0u) {
        return 0u;
    }
    const u32 pinned     = m_bridge->activeScopePinMask.load(std::memory_order_acquire);
    const u32 releasable = m_pendingUnpinScopeMask & ~pinned;
    m_pendingUnpinScopeMask &= ~releasable;
    return releasable;
}

void AudioSystem::SetFocusMuted(bool muted) noexcept {
    if (m_bridge && m_cfg.muteOnFocusLoss) {
        m_bridge->stagingSnapshot.masterMuteFlag = muted ? 1u : 0u;
    }
}

void AudioSystem::ReconcileFinishedVoices() noexcept {
    const u64 hwActiveMask = m_bridge->activeVoiceMask.load(std::memory_order_acquire);
    auto& snap             = m_bridge->stagingSnapshot;
    for (u64 check = m_trackedActiveMask & ~hwActiveMask; check != 0ULL; check &= (check - 1ULL)) {
        const u32 s = static_cast<u32>(std::countr_zero(check));
        if (m_currentTick > m_bridge->voiceTrackers[s].spawnTick + 2u) {
            snap.targetGainL_Q14[s] = 0u;
            snap.targetGainR_Q14[s] = 0u;
            m_trackedActiveMask &= ~(1ULL << s);
        }
    }
}

void AudioSystem::ComputeVoiceSpatialTargets(u32 slot, AudioFrameSnapshot& snap) const noexcept {
    const MainVoiceTracker& tr = m_bridge->voiceTrackers[slot];

    f32 atten  = 1.0f;
    f32 panL   = kInvSqrt2;
    f32 panR   = kInvSqrt2;
    f32 airCut = 20000.0f;

    const VoiceFlags vf = static_cast<VoiceFlags>(tr.flags);
    if (HasVoiceFlag(vf, VoiceFlags::Spatialized2D) &&
        static_cast<AttenuationCurve>(tr.curve) != AttenuationCurve::None) {
        const f32 dx     = tr.worldX - m_listenerX;
        const f32 dy     = tr.worldY - m_listenerY;
        const f32 distSq = dx * dx + dy * dy;
        const f32 maxSq  = tr.maxDist * tr.maxDist;

        if (distSq >= maxSq) {
            atten = 0.0f;
        } else if (distSq > tr.minDist * tr.minDist) {
            const f32 dist = std::sqrt(distSq);
            const f32 t    = (dist - tr.minDist) / std::max(0.01f, tr.maxDist - tr.minDist);
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
            airCut = 20000.0f * (1.0f - 0.65f * t);
        }

        const f32 panNorm = std::clamp(dx / std::max(1.0f, m_cfg.panWidthMeters), -1.0f, 1.0f);
        EvalConstantPowerPan(panNorm, panL, panR);
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
    u32 emitterId) noexcept {
    if (!m_bridge) {
        return {};
    }

    u16 cueIdx             = 0;
    const SoundCueDef* def = FindCueDef(cueId, cueIdx);
    if (!def || def->VariationCount() == 0u) {
        return {};
    }

    // Stage 1: Cooldown & Same-Tick Energy-Summing Coalescer
    const u64 lastTick = m_bridge->cueLastTriggerTick[cueIdx];
    const u32 cooldown = def->CooldownTicks();
    if (lastTick != 0ULL && m_currentTick >= lastTick && (m_currentTick - lastTick) <= cooldown) {
        const u16 prevSlot = m_bridge->cueLastVoiceSlot[cueIdx];
        if (prevSlot < kMaxVoices && (m_trackedActiveMask & (1ULL << prevSlot)) != 0ULL &&
            m_bridge->voiceTrackers[prevSlot].cueId == cueId.value) {
            auto& prevTr    = m_bridge->voiceTrackers[prevSlot];
            prevTr.baseGain = std::min(prevTr.baseGain * 1.1892f, 1.4142f);
            ComputeVoiceSpatialTargets(prevSlot, m_bridge->stagingSnapshot);
            return VoiceHandle{prevSlot, prevTr.generation};
        }
        return {};
    }

    const u8 varCount              = def->VariationCount();
    const u8 varSlot               = static_cast<u8>((m_rngState++) % varCount);
    const assets::AudioRecord& rec = assetBank.GetAudio(def->variations[varSlot]);
    if (!rec.pcm || rec.frameCount == 0u) {
        return {};
    }

    // Stage 2: Bitset-accelerated Concurrency & Physical Slot Selection
    ReconcileFinishedVoices();

    u8  sameCueCount      = 0u;
    u32 oldestSameCueSlot = 0xFFFFFFFFu;
    u64 oldestSameCueTick = 0xFFFFFFFFFFFFFFFFULL;
    const bool onePerEnt  = HasVoiceFlag(def->Flags(), VoiceFlags::OneVoicePerEntity) && emitterId != kNullEmitter;
    u32 entityMatchSlot   = 0xFFFFFFFFu;

    for (u64 active = m_trackedActiveMask; active != 0ULL; active &= (active - 1ULL)) {
        const u32 s     = static_cast<u32>(std::countr_zero(active));
        const auto& tr  = m_bridge->voiceTrackers[s];
        if (onePerEnt && tr.emitterId == emitterId && tr.cueId == cueId.value) {
            entityMatchSlot = s;
            break;
        }
        if (tr.cueId == cueId.value) {
            ++sameCueCount;
            if (tr.spawnTick < oldestSameCueTick) {
                oldestSameCueTick = tr.spawnTick;
                oldestSameCueSlot = s;
            }
        }
    }

    u32 chosenSlot        = 0xFFFFFFFFu;
    bool stoleActiveVoice = false;

    if (entityMatchSlot < kMaxVoices) {
        chosenSlot       = entityMatchSlot;
        stoleActiveVoice = true;
    } else if (sameCueCount >= def->MaxConcurrency() && oldestSameCueSlot < kMaxVoices) {
        chosenSlot       = oldestSameCueSlot;
        stoleActiveVoice = true;
    } else if (m_trackedActiveMask != ~0ULL) {
        chosenSlot = static_cast<u32>(std::countr_one(m_trackedActiveMask));
    } else {
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

    const u16 nextGen    = static_cast<u16>(tr.generation + 1u == 0u ? 1u : tr.generation + 1u);
    const f32 gainJitter = (static_cast<f32>(def->gainVarQ8) / 255.0f) * NextSignedUnitFloat(m_rngState);
    const f32 semiJitter = (static_cast<f32>(def->pitchVarSemitonesQ4) / 16.0f) * NextSignedUnitFloat(m_rngState);
    const f32 totalSemi  = (static_cast<f32>(def->basePitchCents) / 100.0f) + semiJitter;

    tr.cueId           = cueId.value;
    tr.emitterId       = emitterId;
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

    m_trackedActiveMask |= (1ULL << chosenSlot);
    m_bridge->cueLastTriggerTick[cueIdx] = m_currentTick;
    m_bridge->cueLastVoiceSlot[cueIdx]   = static_cast<u16>(chosenSlot);

    ComputeVoiceSpatialTargets(chosenSlot, m_bridge->stagingSnapshot);

    AudioCommand spawnCmd{};
    spawnCmd.type                = AudioCmdType::SpawnVoice;
    spawnCmd.packedBusAndChannel = static_cast<u8>(
        (static_cast<u8>(def->Bus()) & 0x07u) | ((static_cast<u8>(def->Channel()) & 0x07u) << 3u));
    spawnCmd.voiceSlot                    = static_cast<u16>(chosenSlot);
    spawnCmd.generation                   = nextGen;
    spawnCmd.flags                        = tr.flags;
    spawnCmd.assetScopeId                 = m_bridge->cueScopes[cueIdx];
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

void AudioSystem::SubmitTriggers(
    std::span<const AudioTriggerReq> triggers,
    const assets::AssetBank& assetBank) noexcept {
    for (const auto& req : triggers) {
        if (req.cueId.value != 0u) {
            PlayCue(req.cueId, assetBank, req.worldX, req.worldY, req.emitterId);
        }
    }
}

void AudioSystem::StopVoice(VoiceHandle handle, u16 fadeOutBlocks) noexcept {
    if (!m_bridge || !handle.IsValid() || handle.index >= kMaxVoices) {
        return;
    }
    const u64 bit = 1ULL << handle.index;
    const auto& tr = m_bridge->voiceTrackers[handle.index];
    if ((m_trackedActiveMask & bit) == 0ULL || tr.generation != handle.generation) {
        return;
    }
    m_trackedActiveMask &= ~bit;
    m_bridge->stagingSnapshot.targetGainL_Q14[handle.index] = 0u;
    m_bridge->stagingSnapshot.targetGainR_Q14[handle.index] = 0u;

    AudioCommand cmd{};
    cmd.type                       = AudioCmdType::StopVoice;
    cmd.voiceSlot                  = handle.index;
    cmd.generation                 = handle.generation;
    cmd.payload.stop.fadeOutBlocks = fadeOutBlocks;
    cmd.payload.stop.stealPoolIdx  = 0xFFu;
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
    const usize idx                            = static_cast<usize>(bus);
    m_bridge->stagingSnapshot.busSvfG_Q14[idx] = PrewarpCutoffToQ14(cutoffHz);
    const u16 clearMask                        = static_cast<u16>(~(0x03u << (idx * 2u)));
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
        bindCmd.payload.music.deckAndStemIdx  = static_cast<u8>(0x80u | (s & 0x03u));
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
    const u8 flags  = m_bridge->tripleFlags.load(std::memory_order_relaxed);
    const u8 backId = static_cast<u8>((flags >> 4u) & 0x03u);
    m_bridge->snapshots[backId] = snap;
    SwapTripleBufferFlags(m_bridge->tripleFlags, true);
}

void AudioSystem::Update(const AudioUpdateDesc& desc) noexcept {
    if (!m_bridge) {
        return;
    }
    m_currentTick = desc.tickIndex;
    m_listenerX   = desc.listenerX;
    m_listenerY   = desc.listenerY;

    auto& snap             = m_bridge->stagingSnapshot;
    snap.frozenChannelMask = desc.frozenChannelMask;
    const usize scaleCount = std::min<usize>(8u, desc.channelScales.size());
    for (usize ch = 0; ch < scaleCount; ++ch) {
        snap.channelTimeScaleQ12[ch] =
            static_cast<u16>(std::clamp(desc.channelScales[ch] * 4096.0f, 1024.0f, 8192.0f));
    }

    ReconcileFinishedVoices();

    // Staggered 30 Hz raycast occlusion strictly on the main thread across active voices only.
    const u32 staggerGroup = static_cast<u32>(m_currentTick & 3ULL);
    for (u64 active = m_trackedActiveMask; active != 0ULL; active &= (active - 1ULL)) {
        const u32 slot = static_cast<u32>(std::countr_zero(active));
        auto& tr       = m_bridge->voiceTrackers[slot];
        const VoiceFlags vf = static_cast<VoiceFlags>(tr.flags);

        if (desc.occlusionQuery && (slot & 3u) == staggerGroup &&
            HasVoiceFlag(vf, VoiceFlags::Spatialized2D) &&
            !HasVoiceFlag(vf, VoiceFlags::BypassOcclusion)) {
            const f32 dx     = tr.worldX - m_listenerX;
            const f32 dy     = tr.worldY - m_listenerY;
            const f32 distSq = dx * dx + dy * dy;
            f32 targetOcc    = 1.0f;
            if (distSq > 0.25f && distSq < tr.maxDist * tr.maxDist) {
                const f32 dist    = std::sqrt(distSq);
                const f32 invDist = 1.0f / dist;
                if (desc.occlusionQuery(
                        desc.occlusionUserCtx,
                        m_listenerX,
                        m_listenerY,
                        dx * invDist,
                        dy * invDist,
                        dist,
                        tr.emitterId)) {
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

void AudioSystem::ConsumeAndMixBlocks(
    u32 blockCount,
    SDL_AudioStream* stream,
    std::span<f32> captureOut) noexcept {
    // 1. Swap of Spare -> Front in the Triple Buffer when dirty bit [6] is set.
    const u8 flags = SwapTripleBufferFlags(m_bridge->tripleFlags, false);
    const AudioFrameSnapshot& frontSnap = m_bridge->snapshots[flags & 0x03u];

    // 2. Drain discrete lifecycle commands from the SPSC ring buffer.
    u32 readIdx        = m_bridge->cmdReadIdx.load(std::memory_order_relaxed);
    const u32 writeIdx = m_bridge->cmdWriteIdx.load(std::memory_order_acquire);
    while (readIdx != writeIdx) {
        m_mixer.ApplyCommand(m_bridge->cmdRing[readIdx & (kCommandRingSize - 1u)]);
        ++readIdx;
    }
    m_bridge->cmdReadIdx.store(readIdx, std::memory_order_release);

    // 3. Mix 256-frame blocks and push to SDL_AudioStream or headless capture span.
    alignas(64) std::array<f32, kMixBlockFrames * 2> blockOut{};
    constexpr int kBytesPerBlock = static_cast<int>(sizeof(f32) * kMixBlockFrames * 2u);
    usize outOffset              = 0u;

    for (u32 b = 0; b < blockCount; ++b) {
        m_mixer.MixBlock(frontSnap, blockOut);
        if (stream) {
            SDL_PutAudioStreamData(stream, blockOut.data(), kBytesPerBlock);
        }
        if (outOffset + blockOut.size() <= captureOut.size()) {
            std::memcpy(captureOut.data() + outOffset, blockOut.data(), blockOut.size() * sizeof(f32));
            outOffset += blockOut.size();
        }
    }

    // 4. Publish lock-free telemetry back to the main thread.
    const auto& tel = m_mixer.Telemetry();
    m_bridge->activeVoiceMask.store(tel.activeVoiceMask, std::memory_order_release);
    m_bridge->activeScopePinMask.store(tel.activeScopePinMask, std::memory_order_release);
    m_bridge->stolenVoicesTotal.store(tel.stolenVoicesTotal, std::memory_order_relaxed);
    m_bridge->masterPeakL.store(tel.masterPeakL, std::memory_order_relaxed);
    m_bridge->masterPeakR.store(tel.masterPeakR, std::memory_order_relaxed);
}

void AudioSystem::PumpHeadlessBlocks(u32 blockCount, std::span<f32> optionalCaptureOut) noexcept {
    if (m_bridge && blockCount > 0u) {
        ConsumeAndMixBlocks(blockCount, nullptr, optionalCaptureOut);
    }
}

u32 AudioSystem::ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept {
    if (!m_bridge || outEmitters.empty()) {
        return 0u;
    }
    u32 count = 0u;
    for (u64 active = m_trackedActiveMask; active != 0ULL && count < outEmitters.size(); active &= (active - 1ULL)) {
        const u32 slot = static_cast<u32>(std::countr_zero(active));
        const auto& tr = m_bridge->voiceTrackers[slot];
        if (HasVoiceFlag(static_cast<VoiceFlags>(tr.flags), VoiceFlags::Spatialized2D)) {
            outEmitters[count++] = AudioEmitterView{
                tr.worldX,
                tr.worldY,
                tr.maxDist,
                tr.occlusionFactor};
        }
    }
    return count;
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
        EnableHardwareFlushToZero();
        constexpr int kBytesPerBlock = static_cast<int>(sizeof(f32) * kMixBlockFrames * 2u);
        const u32 blocks = static_cast<u32>((additionalAmount + kBytesPerBlock - 1) / kBytesPerBlock);
        static_cast<AudioSystem*>(userdata)->ConsumeAndMixBlocks(blocks, stream, {});
    }
}

} // namespace en::audio
