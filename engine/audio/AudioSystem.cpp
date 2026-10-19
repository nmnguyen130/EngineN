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
inline constexpr u32 kInvalidTick32     = 0xFFFFFFFFu;

[[nodiscard]] constexpr u32 HashCueKey(u32 cueId) noexcept {
    const u64 prod = static_cast<u64>(cueId ^ 0x9E3779B9u) * kFibonacciHashMult;
    return static_cast<u32>(prod >> (64u - 10u)) & (kCueHashTableSize - 1u);
}

[[nodiscard]] constexpr u16 FloatToQ14(f32 val) noexcept {
    const f32 clamped = std::clamp(val, 0.0f, 3.999f);
    return static_cast<u16>(clamped * 16384.0f + 0.5f);
}

// Pre-warps cutoff frequency via 3rd-order Padé approximant to tan(pi * fc / fs).
// Full Q14 range [0.001, 3.5] covers 40 Hz..19.5 kHz smoothly without mid-treble clamping cliffs.
[[nodiscard]] constexpr u16 PrewarpCutoffToQ14(f32 cutoffHz) noexcept {
    const f32 clampedHz = std::clamp(cutoffHz, 40.0f, 20000.0f);
    if (clampedHz >= 19500.0f) {
        return 0u; // Bypass sentinel
    }
    const f32 w  = kPi * clampedHz / static_cast<f32>(kSampleRate);
    const f32 w2 = w * w;
    const f32 g  = w * (15.0f - w2) / (15.0f - 6.0f * w2);
    return FloatToQ14(std::clamp(g, 0.001f, 3.5f));
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

// Compact 8-byte per-cue runtime metadata co-located in 1 cache line (replaces 3 disjoint arrays).
struct alignas(8) CueRuntimeMeta {
    u32 lastTriggerTick{kInvalidTick32};
    u16 lastVoiceSlot{0xFFFFu};
    u8  owningScope{0};
    u8  flags{0};
};
static_assert(sizeof(CueRuntimeMeta) == 8);

} // namespace

// Cache-line isolated synchronization bridge residing at offset 0 of the single arena slab.
struct alignas(64) AudioSystem::SharedBridgeState {
    // Wait-free Triple Buffer: bits [1:0]=Front, [3:2]=Spare, [5:4]=Back, bit [6]=hasNewSpare
    alignas(64) std::array<AudioFrameSnapshot, 3> snapshots{};
    alignas(64) std::atomic<u8> tripleFlags{0b00'10'01'00u};

    // Wait-free SPSC Command Ring (producer & consumer indices isolated on separate 64B cache lines)
    alignas(64) std::atomic<u32> cmdWriteIdx{0};
    alignas(64) std::atomic<u32> cmdReadIdx{0};
    alignas(64) std::array<AudioCommand, kCommandRingSize> cmdRing{};

    // Atomic Telemetry Return Channel (written by audio thread, read by main thread)
    alignas(64) std::atomic<u64> activeVoiceMask{0};
    std::atomic<u32> activeScopePinMask{0};
    std::atomic<u32> stolenVoicesTotal{0};
    std::atomic<f32> masterPeakL{0.0f};
    std::atomic<f32> masterPeakR{0.0f};

    // Main-thread Cue, Music & Voice Tables
    alignas(64) std::array<SoundCueDef, kMaxSoundCues> cueDefs{};
    alignas(64) std::array<CueRuntimeMeta, kMaxSoundCues> cueMeta{};
    alignas(64) std::array<CueHashSlot, kCueHashTableSize> cueHashTable{};
    u16 cueCount{0};

    std::array<MusicTrackDef, kMaxMusicTracks> musicTracks{};
    std::array<u8, kMaxMusicTracks> musicTrackScopes{};
    u16 musicTrackCount{0};

    alignas(64) std::array<MainVoiceTracker, kMaxVoices> voiceTrackers{};
    alignas(64) AudioFrameSnapshot stagingSnapshot{};
};

AudioSystem::~AudioSystem() noexcept {
    Shutdown();
}

AudioSystem::AudioSystem(AudioSystem&& other) noexcept {
    *this = std::move(other);
}

AudioSystem& AudioSystem::operator=(AudioSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_bridge                = std::exchange(other.m_bridge, nullptr);
        m_stream                = std::exchange(other.m_stream, nullptr);
        m_trackedActiveMask     = std::exchange(other.m_trackedActiveMask, 0);
        m_spatialActiveMask     = std::exchange(other.m_spatialActiveMask, 0);
        m_currentTick           = other.m_currentTick;
        m_listenerX             = other.m_listenerX;
        m_listenerY             = other.m_listenerY;
        m_invPanWidth           = other.m_invPanWidth;
        m_rngState              = other.m_rngState;
        m_pendingUnpinScopeMask = other.m_pendingUnpinScopeMask;
        m_muteOnFocusLoss       = other.m_muteOnFocusLoss;
        m_stagingDirty          = other.m_stagingDirty;
        m_mixer                 = std::exchange(other.m_mixer, AudioMixer{});
    }
    return *this;
}

Result<void, AudioErrc> AudioSystem::Init(const AudioSystemConfig& cfg) noexcept {
    Shutdown();
    m_invPanWidth     = 1.0f / std::max(1.0f, cfg.panWidthMeters);
    m_muteOnFocusLoss = cfg.muteOnFocusLoss;
    m_stagingDirty    = false;

    const usize minBytes   = sizeof(SharedBridgeState) + AudioMixer::RequiredArenaBytes();
    const usize arenaBytes = (std::max<usize>(cfg.arenaBytes, minBytes) + 63u) & ~usize(63u);

    void* rawMem = ::operator new(arenaBytes, std::align_val_t{64}, std::nothrow);
    if (!rawMem) {
        return Err(AudioErrc::ArenaExhausted);
    }
    std::memset(rawMem, 0, arenaBytes);
    m_bridge = std::construct_at(static_cast<SharedBridgeState*>(rawMem));

    u8* mixerStart         = reinterpret_cast<u8*>(m_bridge) + sizeof(SharedBridgeState);
    const usize mixerBytes = arenaBytes - sizeof(SharedBridgeState);

    if (auto res = m_mixer.Init(std::span<u8>(mixerStart, mixerBytes)); !res) {
        Shutdown();
        return res;
    }

    auto& snap       = m_bridge->stagingSnapshot;
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
    if (m_bridge) {
        ::operator delete(m_bridge, std::align_val_t{64});
        m_bridge                = nullptr;
        m_trackedActiveMask     = 0;
        m_spatialActiveMask     = 0;
        m_pendingUnpinScopeMask = 0;
        m_stagingDirty          = false;
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

    const u16 newIdx = m_bridge->cueCount;
    u32 slotIdx      = HashCueKey(def.cueId.value);
    CueHashSlot incoming{def.cueId.value, newIdx, 1u};

    for (u32 probe = 0; probe < kCueHashTableSize; ++probe) {
        CueHashSlot& existing = m_bridge->cueHashTable[slotIdx];
        if (existing.psl == 0u) {
            existing                  = incoming;
            m_bridge->cueDefs[newIdx] = def;
            m_bridge->cueMeta[newIdx] = CueRuntimeMeta{kInvalidTick32, 0xFFFFu, owningScope, 0u};
            ++m_bridge->cueCount;
            return {};
        }
        if (existing.key == incoming.key) {
            m_bridge->cueDefs[existing.cueIdx] = def;
            m_bridge->cueMeta[existing.cueIdx] = CueRuntimeMeta{kInvalidTick32, 0xFFFFu, owningScope, 0u};
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

Result<void, AudioErrc> AudioSystem::RegisterMusicTrackDef(
    const MusicTrackDef& def,
    assets::ScopeId owningScope) noexcept {
    if (!m_bridge || def.trackId == 0u) {
        return Err(AudioErrc::InvalidConfig);
    }
    for (u16 i = 0; i < m_bridge->musicTrackCount; ++i) {
        if (m_bridge->musicTracks[i].trackId == def.trackId) {
            m_bridge->musicTracks[i]      = def;
            m_bridge->musicTrackScopes[i] = owningScope;
            return {};
        }
    }
    if (m_bridge->musicTrackCount >= kMaxMusicTracks) {
        return Err(AudioErrc::CueTableFull);
    }
    const u16 idx                   = m_bridge->musicTrackCount++;
    m_bridge->musicTracks[idx]      = def;
    m_bridge->musicTrackScopes[idx] = owningScope;
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
        if (auto res = RegisterMusicTrackDef(track, owningScope); !res) {
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
    if (m_bridge && m_muteOnFocusLoss) {
        const u8 flag = muted ? 1u : 0u;
        if (m_bridge->stagingSnapshot.masterMuteFlag != flag) {
            m_bridge->stagingSnapshot.masterMuteFlag = flag;
            m_stagingDirty                           = true;
        }
    }
}

void AudioSystem::ReconcileFinishedVoices() noexcept {
    const u64 hwActiveMask = m_bridge->activeVoiceMask.load(std::memory_order_acquire);
    const u32 curTick32    = static_cast<u32>(m_currentTick);
    auto& snap             = m_bridge->stagingSnapshot;
    for (u64 check = m_trackedActiveMask & ~hwActiveMask; check != 0ULL; check &= (check - 1ULL)) {
        const u32 s = static_cast<u32>(std::countr_zero(check));
        if (curTick32 - m_bridge->voiceTrackers[s].spawnTick > 2u) {
            const u64 bit           = 1ULL << s;
            snap.targetGainL_Q14[s] = 0u;
            snap.targetGainR_Q14[s] = 0u;
            m_trackedActiveMask    &= ~bit;
            m_spatialActiveMask    &= ~bit;
        }
    }
}

void AudioSystem::ComputeVoiceSpatialTargets(u32 slot, AudioFrameSnapshot& snap) const noexcept {
    const MainVoiceTracker& tr = m_bridge->voiceTrackers[slot];
    const VoiceFlags vf        = tr.Flags();
    const AttenuationCurve crv = tr.Curve();

    f32 atten  = 1.0f;
    f32 panL   = kInvSqrt2;
    f32 panR   = kInvSqrt2;
    f32 airCut = 20000.0f;

    if (HasVoiceFlag(vf, VoiceFlags::Spatialized2D) && crv != AttenuationCurve::None) {
        const f32 dx     = tr.worldX - m_listenerX;
        const f32 dy     = tr.worldY - m_listenerY;
        const f32 distSq = dx * dx + dy * dy;
        const f32 minD   = tr.MinDist();
        const f32 maxD   = tr.MaxDist();
        const f32 maxSq  = maxD * maxD;

        if (distSq >= maxSq) {
            atten = 0.0f;
        } else if (distSq > minD * minD) {
            const f32 dist = std::sqrt(distSq);
            const f32 t    = (dist - minD) / (maxD - minD);
            switch (crv) {
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

        const f32 panNorm = std::clamp(dx * m_invPanWidth, -1.0f, 1.0f);
        EvalConstantPowerPan(panNorm, panL, panR);
    }

    const f32 occGain   = HasVoiceFlag(vf, VoiceFlags::BypassOcclusion) ? 1.0f : tr.Occlusion();
    const f32 totalGain = tr.BaseGain() * atten * (0.45f + 0.55f * occGain);
    const f32 cutoffHz  = std::min(airCut, 900.0f + 19100.0f * occGain);

    snap.targetGainL_Q14[slot] = FloatToQ14(totalGain * panL);
    snap.targetGainR_Q14[slot] = FloatToQ14(totalGain * panR);
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

    // Stage 1: Cooldown & Same-Tick Energy-Summing Coalescer (supports tick 0 via kInvalidTick32 sentinel).
    CueRuntimeMeta& meta = m_bridge->cueMeta[cueIdx];
    const u32 curTick32  = static_cast<u32>(m_currentTick);
    const u32 cooldown   = def->CooldownTicks();
    if (meta.lastTriggerTick != kInvalidTick32 && (curTick32 - meta.lastTriggerTick) <= cooldown) {
        const u16 prevSlot = meta.lastVoiceSlot;
        if (prevSlot < kMaxVoices && (m_trackedActiveMask & (1ULL << prevSlot)) != 0ULL &&
            m_bridge->voiceTrackers[prevSlot].cueId == cueId.value) {
            auto& prevTr       = m_bridge->voiceTrackers[prevSlot];
            prevTr.baseGainQ14 = FloatToQ14(std::min(prevTr.BaseGain() * 1.1892f, 1.4142f));
            ComputeVoiceSpatialTargets(prevSlot, m_bridge->stagingSnapshot);
            m_stagingDirty = true;
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

    // Stage 2: Single-pass bitset-accelerated Concurrency, Entity Replacement & Steal Selection.
    if (m_trackedActiveMask == ~0ULL) {
        ReconcileFinishedVoices();
    }

    const VoiceFlags vf        = def->Flags();
    const StealPolicy stealPol = def->Steal();
    const bool onePerEnt       = HasVoiceFlag(vf, VoiceFlags::OneVoicePerEntity) && emitterId != kNullEmitter;
    const bool poolFull        = (m_trackedActiveMask == ~0ULL);
    auto& snap                 = m_bridge->stagingSnapshot;

    u8  sameCueCount      = 0u;
    u32 oldestSameCueSlot = 0xFFFFFFFFu;
    u32 oldestSameCueAge  = 0u;
    u32 entityMatchSlot   = 0xFFFFFFFFu;
    u32 bestStealSlot     = 0xFFFFFFFFu;
    u64 lowestStealScore  = 0xFFFFFFFFFFFFFFFFULL;

    for (u64 active = m_trackedActiveMask; active != 0ULL; active &= (active - 1ULL)) {
        const u32 s    = static_cast<u32>(std::countr_zero(active));
        const auto& tr = m_bridge->voiceTrackers[s];
        const u32 age  = curTick32 - tr.spawnTick;

        if (onePerEnt && tr.emitterId == emitterId && tr.cueId == cueId.value) {
            entityMatchSlot = s;
            break;
        }
        if (tr.cueId == cueId.value) {
            ++sameCueCount;
            if (oldestSameCueSlot == 0xFFFFFFFFu || age >= oldestSameCueAge) {
                oldestSameCueAge  = age;
                oldestSameCueSlot = s;
            }
        }
        if (poolFull && stealPol != StealPolicy::Reject && tr.priority <= def->priority) {
            const u64 metric = (stealPol == StealPolicy::StealOldest)
                                   ? static_cast<u64>(~age)
                                   : static_cast<u64>(snap.targetGainL_Q14[s]) + snap.targetGainR_Q14[s];
            const u64 score  = (static_cast<u64>(tr.priority) << 32u) | metric;
            if (score < lowestStealScore) {
                lowestStealScore = score;
                bestStealSlot    = s;
            }
        }
    }

    u32 chosenSlot = 0xFFFFFFFFu;
    if (entityMatchSlot < kMaxVoices) {
        chosenSlot = entityMatchSlot;
    } else if (sameCueCount >= def->MaxConcurrency() && oldestSameCueSlot < kMaxVoices) {
        chosenSlot = oldestSameCueSlot;
    } else if (!poolFull) {
        chosenSlot = static_cast<u32>(std::countr_one(m_trackedActiveMask));
    } else if (bestStealSlot < kMaxVoices) {
        chosenSlot = bestStealSlot;
    } else {
        return {};
    }

    // Stage 3: Populate 32B tracker & emit single SpawnVoice command (AudioMixer auto-fades stolen slot).
    auto& tr             = m_bridge->voiceTrackers[chosenSlot];
    const u16 nextGen    = static_cast<u16>(tr.generation + 1u == 0u ? 1u : tr.generation + 1u);
    const f32 gainJitter = (static_cast<f32>(def->gainVarQ8) / 255.0f) * NextSignedUnitFloat(m_rngState);
    const f32 semiJitter = (static_cast<f32>(def->pitchVarSemitonesQ4) / 16.0f) * NextSignedUnitFloat(m_rngState);
    const f32 totalSemi  = (static_cast<f32>(def->basePitchCents) / 100.0f) + semiJitter;
    const f32 pitchRatio = std::exp2(totalSemi / 12.0f);
    const f32 baseGain   = std::max(0.0f, (static_cast<f32>(def->baseGainQ14) * (1.0f / 16384.0f)) + gainJitter);

    tr = MainVoiceTracker{
        .cueId        = cueId.value,
        .emitterId    = emitterId,
        .spawnTick    = curTick32,
        .worldX       = worldX,
        .worldY       = worldY,
        .baseGainQ14  = FloatToQ14(baseGain),
        .occlusionQ14 = 16384u,
        .generation   = nextGen,
        .minDistQ2    = def->minDistQ2,
        .maxDistQ1    = def->maxDistQ1,
        .priority     = def->priority,
        .curve        = static_cast<u8>(def->Curve()),
        .flags        = static_cast<u8>(vf),
        .scopeId      = meta.owningScope};

    const u64 slotBit = 1ULL << chosenSlot;
    m_trackedActiveMask |= slotBit;
    if (HasVoiceFlag(vf, VoiceFlags::Spatialized2D)) {
        m_spatialActiveMask |= slotBit;
    } else {
        m_spatialActiveMask &= ~slotBit;
    }

    meta.lastTriggerTick = curTick32;
    meta.lastVoiceSlot   = static_cast<u16>(chosenSlot);

    snap.targetPitchQ12[chosenSlot] = static_cast<u16>(std::clamp(pitchRatio * 4096.0f, 1024.0f, 8192.0f));
    ComputeVoiceSpatialTargets(chosenSlot, snap);
    m_stagingDirty = true;

    AudioCommand spawnCmd{};
    spawnCmd.type                = AudioCmdType::SpawnVoice;
    spawnCmd.packedBusAndChannel = static_cast<u8>(
        (static_cast<u8>(def->Bus()) & 0x07u) | ((static_cast<u8>(def->Channel()) & 0x07u) << 3u));
    spawnCmd.voiceSlot                    = static_cast<u16>(chosenSlot);
    spawnCmd.generation                   = nextGen;
    spawnCmd.flags                        = static_cast<u8>(vf);
    spawnCmd.assetScopeId                 = meta.owningScope;
    spawnCmd.payload.spawn.pcm            = rec.pcm;
    spawnCmd.payload.spawn.frameCount     = rec.frameCount;
    spawnCmd.payload.spawn.loopStartFrame = 0u;
    spawnCmd.payload.spawn.sampleRate     = rec.sampleRate;
    spawnCmd.payload.spawn.gainL_Q14      = snap.targetGainL_Q14[chosenSlot];
    spawnCmd.payload.spawn.gainR_Q14      = snap.targetGainR_Q14[chosenSlot];
    spawnCmd.payload.spawn.pitchRatioQ12  = snap.targetPitchQ12[chosenSlot];

    EnqueueCommand(spawnCmd);
    return VoiceHandle{static_cast<u16>(chosenSlot), nextGen};
}

void AudioSystem::SubmitTriggers(
    std::span<const AudioTriggerReq> triggers,
    const assets::AssetBank& assetBank) noexcept {
    if (!m_bridge || triggers.empty()) {
        return;
    }
    ReconcileFinishedVoices();
    for (const auto& req : triggers) {
        if (req.cueId.value != 0u) {
            (void)PlayCue(req.cueId, assetBank, req.worldX, req.worldY, req.emitterId);
        }
    }
}

void AudioSystem::StopVoice(VoiceHandle handle, u16 fadeOutBlocks) noexcept {
    if (!m_bridge || !handle.IsValid() || handle.index >= kMaxVoices) {
        return;
    }
    const u64 bit  = 1ULL << handle.index;
    const auto& tr = m_bridge->voiceTrackers[handle.index];
    if ((m_trackedActiveMask & bit) == 0ULL || tr.generation != handle.generation) {
        return;
    }
    m_trackedActiveMask &= ~bit;
    m_spatialActiveMask &= ~bit;
    m_bridge->stagingSnapshot.targetGainL_Q14[handle.index] = 0u;
    m_bridge->stagingSnapshot.targetGainR_Q14[handle.index] = 0u;
    m_stagingDirty = true;

    AudioCommand cmd{};
    cmd.type                       = AudioCmdType::StopVoice;
    cmd.voiceSlot                  = handle.index;
    cmd.generation                 = handle.generation;
    cmd.payload.stop.fadeOutBlocks = fadeOutBlocks;
    EnqueueCommand(cmd);
}

void AudioSystem::SetBusGain(AudioBusId bus, f32 linearGain) noexcept {
    if (!m_bridge || static_cast<usize>(bus) >= kAudioBusCount) {
        return;
    }
    m_bridge->stagingSnapshot.busTargetGainQ14[static_cast<usize>(bus)] = FloatToQ14(linearGain);
    m_stagingDirty = true;
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
    m_stagingDirty = true;
}

void AudioSystem::TransitionMusic(u32 trackId, const assets::AssetBank& assetBank, u8 crossfadeBlocks) noexcept {
    if (!m_bridge) {
        return;
    }
    const MusicTrackDef* found = nullptr;
    u8 trackScope              = 0u;
    for (u16 i = 0; i < m_bridge->musicTrackCount; ++i) {
        if (m_bridge->musicTracks[i].trackId == trackId) {
            found      = &m_bridge->musicTracks[i];
            trackScope = m_bridge->musicTrackScopes[i];
            break;
        }
    }
    if (!found) {
        return;
    }

    const u8 stems  = found->StemCount();
    bool firstBound = true;
    for (u8 s = 0; s < stems; ++s) {
        const assets::AudioRecord& rec = assetBank.GetAudio(found->stems[s]);
        if (!rec.pcm || rec.frameCount == 0u) {
            continue;
        }
        AudioCommand bindCmd{};
        bindCmd.type                          = AudioCmdType::BindMusicStem;
        bindCmd.assetScopeId                  = trackScope;
        bindCmd.payload.music.pcm             = rec.pcm;
        bindCmd.payload.music.frameCount      = rec.frameCount;
        bindCmd.payload.music.loopStartFrame  = found->loopStartFrame;
        bindCmd.payload.music.loopEndFrame    = found->loopEndFrame;
        bindCmd.payload.music.tempoBpmQ8      = found->tempoBpmQ8;
        bindCmd.payload.music.deckAndStemIdx  = static_cast<u8>(0x80u | (firstBound ? 0x40u : 0x00u) | (s & 0x03u));
        bindCmd.payload.music.crossfadeBlocks = crossfadeBlocks;
        EnqueueCommand(bindCmd);
        firstBound = false;
    }

    if (!firstBound) {
        AudioCommand commitCmd{};
        commitCmd.type                          = AudioCmdType::CommitMusicDeck;
        commitCmd.assetScopeId                  = trackScope;
        commitCmd.payload.music.crossfadeBlocks = crossfadeBlocks;
        EnqueueCommand(commitCmd);
    }
}

void AudioSystem::SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept {
    if (!m_bridge) {
        return;
    }
    for (usize s = 0; s < kMaxMusicStems; ++s) {
        m_bridge->stagingSnapshot.stemWeightsQ14[s] = FloatToQ14(weights[s]);
    }
    m_stagingDirty = true;
}

void AudioSystem::PublishFrameSnapshot(const AudioFrameSnapshot& snap) noexcept {
    const u8 flags              = m_bridge->tripleFlags.load(std::memory_order_relaxed);
    const u8 backId             = static_cast<u8>((flags >> 4u) & 0x03u);
    m_bridge->snapshots[backId] = snap;
    SwapTripleBufferFlags(m_bridge->tripleFlags, true);
    m_stagingDirty = false;
}

void AudioSystem::Update(const AudioUpdateDesc& desc) noexcept {
    if (!m_bridge) {
        return;
    }
    m_currentTick = desc.tickIndex;
    m_listenerX   = desc.listenerX;
    m_listenerY   = desc.listenerY;

    if (desc.focusMuteOverride >= 0) {
        SetFocusMuted(desc.focusMuteOverride != 0);
    }

    ReconcileFinishedVoices();

    if (!desc.triggers.empty() && desc.assetBank) {
        for (const auto& req : desc.triggers) {
            if (req.cueId.value != 0u) {
                (void)PlayCue(req.cueId, *desc.assetBank, req.worldX, req.worldY, req.emitterId);
            }
        }
    }

    auto& snap             = m_bridge->stagingSnapshot;
    snap.frozenChannelMask = desc.frozenChannelMask;
    const usize scaleCount = std::min<usize>(8u, desc.channelScales.size());
    for (usize ch = 0; ch < scaleCount; ++ch) {
        snap.channelTimeScaleQ12[ch] =
            static_cast<u16>(std::clamp(desc.channelScales[ch] * 4096.0f, 1024.0f, 8192.0f));
    }

    // Staggered 30 Hz raycast occlusion and spatial panning strictly across Spatialized2D voices.
    const u32 staggerGroup = static_cast<u32>(m_currentTick & 3ULL);
    for (u64 spatial = m_spatialActiveMask; spatial != 0ULL; spatial &= (spatial - 1ULL)) {
        const u32 slot = static_cast<u32>(std::countr_zero(spatial));
        auto& tr       = m_bridge->voiceTrackers[slot];

        if (desc.occlusionQuery && (slot & 3u) == staggerGroup &&
            !HasVoiceFlag(tr.Flags(), VoiceFlags::BypassOcclusion)) {
            const f32 dx     = tr.worldX - m_listenerX;
            const f32 dy     = tr.worldY - m_listenerY;
            const f32 distSq = dx * dx + dy * dy;
            const f32 maxD   = tr.MaxDist();
            f32 targetOcc    = 1.0f;
            if (distSq > 0.25f && distSq < maxD * maxD) {
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
            const f32 curOcc = tr.Occlusion();
            tr.occlusionQ14  = FloatToQ14(curOcc + (targetOcc - curOcc) * 0.25f);
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
        if (m_stagingDirty) {
            ++m_bridge->stagingSnapshot.snapshotSeq;
            PublishFrameSnapshot(m_bridge->stagingSnapshot);
        }
        ConsumeAndMixBlocks(blockCount, nullptr, optionalCaptureOut);
    }
}

u32 AudioSystem::ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept {
    if (!m_bridge || outEmitters.empty()) {
        return 0u;
    }
    u32 count = 0u;
    for (u64 spatial = m_spatialActiveMask; spatial != 0ULL && count < outEmitters.size(); spatial &= (spatial - 1ULL)) {
        const u32 slot = static_cast<u32>(std::countr_zero(spatial));
        const auto& tr = m_bridge->voiceTrackers[slot];
        outEmitters[count++] = AudioEmitterView{
            tr.worldX,
            tr.worldY,
            tr.MaxDist(),
            tr.Occlusion()};
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
