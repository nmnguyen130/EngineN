#include "audio/AudioSystem.h"

#include "assets/AssetBank.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <atomic>

namespace en {
namespace {

constexpr u32 kMaxCues   = 256U;
constexpr u32 kMaxTracks = 32U;
constexpr u32 kCmdRing   = 256U;

[[nodiscard]] constexpr f32 EvalAttenuation(
    AttenuationCurve curve, f32 dist, f32 minDist, f32 maxDist) noexcept {
    if (curve == AttenuationCurve::None || dist <= minDist) return 1.0f;
    if (dist >= maxDist || maxDist <= minDist) return 0.0f;
    const f32 t = (dist - minDist) / (maxDist - minDist);
    switch (curve) {
        case AttenuationCurve::Linear:         return 1.0f - t;
        case AttenuationCurve::InverseClamped: return minDist / (minDist + 4.0f * (dist - minDist));
        case AttenuationCurve::Smoothstep:     return 1.0f - t * t * (3.0f - 2.0f * t);
        default:                               return 1.0f - t;
    }
}

struct VoiceTracker {
    Vec2 position{0.0f, 0.0f};
    f32  gain{1.0f};
    f32  pitch{1.0f};
    u32  emitter{kNullAudioEmitter};
    u16  gen{0};
    u16  cueIndex{0};
};
static_assert(sizeof(VoiceTracker) == 24);

} // namespace

struct AudioSystem::Bridge {
    AudioFrameSnapshot                   snap{};
    std::array<AudioFrameSnapshot, 2>    pub{};
    std::array<AudioCommand, kCmdRing>   ring{};
    std::array<SoundCueDef, kMaxCues>    cues{};
    std::array<u16, kMaxCues>            lastTick{};
    std::array<u8, kMaxCues>             concurrent{};
    std::array<MusicTrackDef, kMaxTracks> tracks{};
    std::array<VoiceTracker, kMaxAudioVoices> trackers{};
    HashMap<u16>                         cueMap{};
    SDL_AudioStream*                     stream{nullptr};
    Rng                                  rng{};
    u64                                  active{0};
    f32                                  invPan{1.0f / 16.0f};
    std::atomic<u64>                     done{0};
    std::atomic<u32>                     w{0};
    std::atomic<u32>                     r{0};
    std::atomic<u32>                     pubIdx{0};
    u16                                  cueCount{0};
    u16                                  trackCount{0};
    u16                                  nextGen{1};
    bool                                 ownsSdl{false};

    [[nodiscard]] VoiceTracker* Resolve(VoiceHandle h) noexcept {
        if (!h.IsValid() || h.index >= kMaxAudioVoices) return nullptr;
        VoiceTracker& t = trackers[h.index];
        return (t.gen == h.generation) ? &t : nullptr;
    }

    [[nodiscard]] u16 FindTrack(AssetId id) const noexcept {
        for (u16 i = 0U; i < trackCount; ++i) {
            if (tracks[i].id == id) return i;
        }
        return 0xFFFFU;
    }
};

Result<void, AudioErrc> AudioSystem::Init(const AudioConfig& cfg) noexcept {
    Shutdown();
    if (cfg.panWidth <= 0.1f) return Err(AudioErrc::InvalidConfig);

    const usize need = AudioMixer::RequiredArenaBytes() + sizeof(Bridge) + 16384U;
    if (!m_arena.Init(std::max<usize>(cfg.arenaBytes, need))) {
        return Err(AudioErrc::OutOfMemory);
    }

    auto mixerSlice  = m_arena.Alloc<u8>(AudioMixer::RequiredArenaBytes());
    auto bridgeSlice = m_arena.Alloc<Bridge>(1U);
    if (mixerSlice.empty() || bridgeSlice.empty()) {
        Shutdown();
        return Err(AudioErrc::OutOfMemory);
    }
    if (auto res = m_mixer.Init(mixerSlice); !res) {
        Shutdown();
        return res;
    }

    m_bridge = bridgeSlice.data();
    if (!m_bridge->cueMap.Init(m_arena, kMaxCues * 2U)) {
        Shutdown();
        return Err(AudioErrc::OutOfMemory);
    }

    m_bridge->invPan = 1.0f / cfg.panWidth;
    m_bridge->lastTick.fill(0xFFFFU);
    m_bridge->snap.occCutoffHz = std::clamp(cfg.occCutoffHz, kMinCutoffHz, kMaxCutoffHz);
    m_bridge->snap.pitch.fill(1.0f);
    m_bridge->pub[0] = m_bridge->snap;
    m_bridge->pub[1] = m_bridge->snap;

    if (!cfg.headless) {
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0U && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            m_bridge->ownsSdl = true;
        }
        SDL_AudioSpec spec{};
        spec.format   = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq     = static_cast<int>(kAudioSampleRate);
        const auto cb = [](void* ud, SDL_AudioStream*, int extra, int) noexcept {
            if (ud && extra > 0) static_cast<AudioSystem*>(ud)->PullDeviceBytes(extra);
        };
        m_bridge->stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, cb, this);
        if (m_bridge->stream) (void)SDL_ResumeAudioStreamDevice(m_bridge->stream);
    }

    m_spatialMask = 0ULL;
    m_listener    = {};
    m_tick        = 0U;
    m_muted       = false;
    m_dirty       = false;
    return {};
}

void AudioSystem::Shutdown() noexcept {
    if (m_bridge) {
        if (m_bridge->stream) {
            SDL_DestroyAudioStream(m_bridge->stream);
            m_bridge->stream = nullptr;
        }
        if (m_bridge->ownsSdl) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            m_bridge->ownsSdl = false;
        }
    }
    m_arena.Shutdown();
    m_mixer       = AudioMixer{};
    m_bridge      = nullptr;
    m_spatialMask = 0ULL;
    m_listener    = {};
    m_tick        = 0U;
    m_muted       = false;
    m_dirty       = false;
}

bool AudioSystem::HasDevice() const noexcept {
    return m_bridge && m_bridge->stream != nullptr;
}

void AudioSystem::SetFocusMuted(bool muted) noexcept {
    m_muted = muted;
    if (m_bridge) {
        m_bridge->snap.muted = muted;
        m_dirty              = true;
    }
}

void AudioSystem::PublishSnapshot() noexcept {
    if (!m_bridge) return;
    const u32 next       = (m_bridge->pubIdx.load(std::memory_order_relaxed) + 1U) & 1U;
    m_bridge->pub[next] = m_bridge->snap;
    m_bridge->pubIdx.store(next, std::memory_order_release);
    m_dirty = false;
}

void AudioSystem::SyncFinishedVoices() noexcept {
    if (!m_bridge) return;
    const u64 fin = m_bridge->done.exchange(0ULL, std::memory_order_acquire);
    if (fin == 0ULL) return;
    for (u8 i = 0U; i < kMaxAudioVoices; ++i) {
        if ((fin & (1ULL << i)) == 0ULL) continue;
        const u16 c = m_bridge->trackers[i].cueIndex;
        if (c < kMaxCues && m_bridge->concurrent[c] > 0U) --m_bridge->concurrent[c];
    }
    m_bridge->active            &= ~fin;
    m_spatialMask               &= ~fin;
    m_bridge->snap.occludedMask &= ~fin;
}

Result<void, AudioErrc> AudioSystem::RegisterCue(const SoundCueDef& def) noexcept {
    if (!m_bridge || !def.id.IsValid()) return Err(AudioErrc::InvalidConfig);
    if (const u16* existing = m_bridge->cueMap.Find(def.id.value)) {
        m_bridge->cues[*existing] = def;
        return {};
    }
    if (m_bridge->cueCount >= kMaxCues) return Err(AudioErrc::CueTableFull);
    const u16 idx         = m_bridge->cueCount++;
    m_bridge->cues[idx]   = def;
    (void)m_bridge->cueMap.Insert(def.id.value, idx);
    return {};
}

Result<void, AudioErrc> AudioSystem::RegisterMusic(const MusicTrackDef& def) noexcept {
    if (!m_bridge || !def.id.IsValid()) return Err(AudioErrc::InvalidConfig);
    const u16 existing = m_bridge->FindTrack(def.id);
    if (existing != 0xFFFFU) {
        m_bridge->tracks[existing] = def;
        return {};
    }
    if (m_bridge->trackCount >= kMaxTracks) return Err(AudioErrc::CueTableFull);
    m_bridge->tracks[m_bridge->trackCount++] = def;
    return {};
}

void AudioSystem::Enqueue(const AudioCommand& cmd) noexcept {
    if (!m_bridge) return;
    const u32 w    = m_bridge->w.load(std::memory_order_relaxed);
    const u32 next = (w + 1U) & (kCmdRing - 1U);
    if (next == m_bridge->r.load(std::memory_order_acquire)) return;
    m_bridge->ring[w] = cmd;
    m_bridge->w.store(next, std::memory_order_release);
}

void AudioSystem::ComputeSpatialGains(
    f32 gain,
    Vec2 pos,
    const SoundCueDef& def,
    Vec2 listener,
    f32& outL,
    f32& outR) const noexcept {
    if (!HasFlag(def.flags, VoiceFlags::Spatial2D)) {
        const f32 g = std::clamp(gain * kEqPower, 0.0f, kMaxGain);
        outL = outR = g;
        return;
    }
    const Vec2 delta = pos - listener;
    const f32  d2    = delta.LengthSq();
    if (def.atten != AttenuationCurve::None &&
        (def.maxDist <= def.minDist || d2 >= def.maxDist * def.maxDist)) {
        outL = outR = 0.0f;
        return;
    }
    if (d2 <= kSilence) {
        const f32 g = std::clamp(gain * kEqPower, 0.0f, kMaxGain);
        outL = outR = g;
        return;
    }
    const f32 dist = std::sqrt(d2);
    const f32 att  = EvalAttenuation(def.atten, dist, def.minDist, def.maxDist);
    const f32 pan  = std::clamp(delta.x * m_bridge->invPan, -1.0f, 1.0f);
    f32 c = 1.0f, s = 0.0f;
    FastSinCos(pan * (kPi * 0.25f), c, s);
    const f32 g = std::clamp(gain * att, 0.0f, kMaxGain);
    outL = g * kEqPower * (c - s);
    outR = g * kEqPower * (c + s);
}

VoiceHandle AudioSystem::Play(
    AssetId cueId,
    const AssetBank& assets,
    Vec2 position,
    u32 emitter,
    f32 gainScale) noexcept {
    if (!m_bridge || !cueId.IsValid()) return kNullVoice;
    const u16* cueIdxPtr = m_bridge->cueMap.Find(cueId.value);
    if (!cueIdxPtr) return kNullVoice;
    const u16          ci  = *cueIdxPtr;
    const SoundCueDef& def = m_bridge->cues[ci];

    if (def.cooldownTicks > 0U && m_bridge->lastTick[ci] != 0xFFFFU &&
        static_cast<u16>(m_tick - m_bridge->lastTick[ci]) < def.cooldownTicks) {
        return kNullVoice;
    }
    if (def.maxConcurrency > 0U && m_bridge->concurrent[ci] >= def.maxConcurrency) {
        return kNullVoice;
    }

    if (HasFlag(def.flags, VoiceFlags::OneVoicePerEntity) && emitter != kNullAudioEmitter) {
        for (u8 i = 0U; i < kMaxAudioVoices; ++i) {
            if ((m_bridge->active & (1ULL << i)) == 0ULL) continue;
            VoiceTracker& t = m_bridge->trackers[i];
            if (t.emitter == emitter && t.cueIndex == ci) {
                Stop(VoiceHandle{i, t.gen}, 1U);
                break;
            }
        }
    }

    u8 slot   = 0xFFU;
    u8 lowPri = 255U;
    for (u8 i = 0U; i < kMaxAudioVoices; ++i) {
        if ((m_bridge->active & (1ULL << i)) == 0ULL) {
            slot = i;
            break;
        }
        const u16 c = m_bridge->trackers[i].cueIndex;
        const u8  p = (c < kMaxCues) ? m_bridge->cues[c].priority : 0U;
        if (p < lowPri) {
            lowPri = p;
            slot   = i;
        }
    }
    if (slot == 0xFFU) return kNullVoice;
    if ((m_bridge->active & (1ULL << slot)) != 0ULL) {
        if (def.priority <= lowPri) return kNullVoice;
        const u16 oldCue = m_bridge->trackers[slot].cueIndex;
        if (oldCue < kMaxCues && m_bridge->concurrent[oldCue] > 0U) {
            --m_bridge->concurrent[oldCue];
        }
        m_spatialMask &= ~(1ULL << slot);
    }

    AudioHandle clip = def.variants[0];
    if (def.variants[1].IsValid() && (m_bridge->rng.NextU64() & 1ULL) != 0ULL) {
        clip = def.variants[1];
    }
    const AudioRecord& rec = assets.GetAudio(clip);
    if (!rec.pcm || rec.frameCount == 0U) return kNullVoice;

    const f32 gain = def.baseGain * gainScale *
        (def.gainJitter > 0.0f ? m_bridge->rng.Range(1.0f - def.gainJitter, 1.0f + def.gainJitter) : 1.0f);
    const f32 pitch = def.basePitch *
        (def.pitchJitter > 0.0f ? m_bridge->rng.Range(1.0f - def.pitchJitter, 1.0f + def.pitchJitter) : 1.0f);

    f32 gL = 0.0f, gR = 0.0f;
    ComputeSpatialGains(gain, position, def, m_listener, gL, gR);

    const u16 gen = m_bridge->nextGen++;
    if (m_bridge->nextGen == 0U) m_bridge->nextGen = 1U;

    m_bridge->trackers[slot] = VoiceTracker{
        .position = position,
        .gain     = gain,
        .pitch    = pitch,
        .emitter  = emitter,
        .gen      = gen,
        .cueIndex = ci,
    };

    AudioCommand cmd{
        .pcm        = rec.pcm,
        .frames     = rec.frameCount,
        .loopStart  = def.loopStart,
        .gainL      = gL,
        .gainR      = gR,
        .pitch      = pitch,
        .sampleRate = rec.sampleRate,
        .generation = gen,
        .type       = AudioCmdType::Spawn,
        .bus        = def.bus,
        .slot       = slot,
        .flags      = rec.looping ? (def.flags | VoiceFlags::Looping) : def.flags,
        .channels   = rec.channels,
    };
    Enqueue(cmd);

    m_bridge->active |= (1ULL << slot);
    if (HasFlag(def.flags, VoiceFlags::Spatial2D)) m_spatialMask |= (1ULL << slot);
    m_bridge->snap.gainL[slot] = gL;
    m_bridge->snap.gainR[slot] = gR;
    m_bridge->snap.pitch[slot] = pitch;
    m_bridge->lastTick[ci]     = m_tick;
    ++m_bridge->concurrent[ci];
    m_dirty = true;
    return VoiceHandle{slot, gen};
}

void AudioSystem::Stop(VoiceHandle handle, u16 fadeBlocks) noexcept {
    if (!m_bridge) return;
    VoiceTracker* tr = m_bridge->Resolve(handle);
    if (!tr) return;

    Enqueue(AudioCommand{
        .generation = handle.generation,
        .fadeBlocks = fadeBlocks,
        .type       = AudioCmdType::Stop,
        .slot       = static_cast<u8>(handle.index),
    });

    if (fadeBlocks == 0U) {
        if (tr->cueIndex < kMaxCues && m_bridge->concurrent[tr->cueIndex] > 0U) {
            --m_bridge->concurrent[tr->cueIndex];
        }
        m_bridge->active &= ~(1ULL << handle.index);
        m_spatialMask    &= ~(1ULL << handle.index);
    }
    m_dirty = true;
}

void AudioSystem::StopAll(u16 fadeBlocks) noexcept {
    if (!m_bridge) return;
    for (u8 i = 0U; i < kMaxAudioVoices; ++i) {
        if ((m_bridge->active & (1ULL << i)) != 0ULL) {
            Stop(VoiceHandle{i, m_bridge->trackers[i].gen}, fadeBlocks);
        }
    }
}

void AudioSystem::SetBusGain(AudioBus bus, f32 gain) noexcept {
    if (!m_bridge) return;
    m_bridge->snap.busGain[static_cast<u8>(bus) % kAudioBusCount] =
        std::clamp(gain, 0.0f, kMaxGain);
    m_dirty = true;
}

f32 AudioSystem::GetBusGain(AudioBus bus) const noexcept {
    if (!m_bridge) return 1.0f;
    return m_bridge->snap.busGain[static_cast<u8>(bus) % kAudioBusCount];
}

bool AudioSystem::PlayMusic(AssetId trackId, const AssetBank& assets, u8 fadeBlocks) noexcept {
    if (!m_bridge || !trackId.IsValid()) return false;
    const u16 ti = m_bridge->FindTrack(trackId);
    if (ti == 0xFFFFU) return false;
    const MusicTrackDef& t = m_bridge->tracks[ti];

    Enqueue(AudioCommand{
        .loopStart  = t.loopStart,
        .fadeBlocks = fadeBlocks,
        .type       = AudioCmdType::CommitDeck,
        .flags      = t.loop ? VoiceFlags::Looping : VoiceFlags::None,
    });

    const u8 n = std::min<u8>(t.stemCount, static_cast<u8>(kMaxMusicStems));
    for (u8 s = 0U; s < n; ++s) {
        if (!t.stems[s].IsValid()) continue;
        const AudioRecord& rec = assets.GetAudio(t.stems[s]);
        if (!rec.pcm || rec.frameCount == 0U) continue;
        Enqueue(AudioCommand{
            .pcm        = rec.pcm,
            .frames     = rec.frameCount,
            .gainL      = t.weights[s],
            .sampleRate = rec.sampleRate,
            .type       = AudioCmdType::BindStem,
            .slot       = s,
            .channels   = rec.channels,
        });
    }
    for (u8 s = 0U; s < kMaxMusicStems; ++s) {
        m_bridge->snap.stemWeights[s] = t.weights[s];
    }
    m_dirty = true;
    return true;
}

void AudioSystem::SetStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept {
    if (!m_bridge) return;
    for (u8 i = 0U; i < kMaxMusicStems; ++i) {
        m_bridge->snap.stemWeights[i] = std::clamp(weights[i], 0.0f, 2.0f);
    }
    m_dirty = true;
}

void AudioSystem::Update(const AudioUpdateDesc& desc) noexcept {
    if (!m_bridge) return;
    SyncFinishedVoices();
    m_tick     = static_cast<u16>(desc.tick);
    m_listener = desc.listenerPos;

    if (desc.focusMute >= 0) SetFocusMuted(desc.focusMute != 0);

    if (desc.assets && !desc.triggers.empty()) {
        for (const AudioTrigger& req : desc.triggers) {
            (void)Play(req.cueId, *desc.assets, req.position, req.emitter, req.gainScale);
        }
    }

    u64 paused = 0ULL;
    u64 occ    = 0ULL;
    u64 live   = m_bridge->active;
    while (live != 0ULL) {
        const u8 s = static_cast<u8>(std::countr_zero(live));
        live &= live - 1ULL;
        const VoiceTracker& tr = m_bridge->trackers[s];
        if (tr.cueIndex >= kMaxCues) continue;
        const SoundCueDef& def = m_bridge->cues[tr.cueIndex];

        if (HasFlag(def.flags, VoiceFlags::PauseOnFreeze) &&
            (desc.frozenMask & ChannelBit(def.channel)) != 0U) {
            paused |= (1ULL << s);
        }

        f32 pitch = tr.pitch;
        if (HasFlag(def.flags, VoiceFlags::FollowTimeScale) && desc.channelScales) {
            pitch *= desc.channelScales[static_cast<u8>(def.channel)];
        }
        m_bridge->snap.pitch[s] = std::clamp(pitch, kMinPitch, kMaxPitch);

        if ((m_spatialMask & (1ULL << s)) != 0ULL) {
            f32 gL = 0.0f, gR = 0.0f;
            ComputeSpatialGains(tr.gain, tr.position, def, m_listener, gL, gR);
            m_bridge->snap.gainL[s] = gL;
            m_bridge->snap.gainR[s] = gR;
        }

        if (!HasFlag(def.flags, VoiceFlags::BypassOcclusion) && desc.occlusionFn) {
            if (desc.occlusionFn(m_listener, tr.position, desc.occlusionCtx)) {
                occ |= (1ULL << s);
            }
        }
    }
    m_bridge->snap.pausedMask   = paused;
    m_bridge->snap.occludedMask = occ;
    m_dirty                     = true;
    PublishSnapshot();

    if (!m_bridge->stream) Pump(0U);
}

void AudioSystem::RunBlocks(u32 blocks, std::span<f32> captureOut, bool pushToDevice) noexcept {
    if (!m_bridge) return;

    u32 rd       = m_bridge->r.load(std::memory_order_relaxed);
    const u32 wr = m_bridge->w.load(std::memory_order_acquire);
    while (rd != wr) {
        m_mixer.Apply(m_bridge->ring[rd]);
        rd = (rd + 1U) & (kCmdRing - 1U);
    }
    m_bridge->r.store(rd, std::memory_order_release);

    const u32                 pi   = m_bridge->pubIdx.load(std::memory_order_acquire);
    const AudioFrameSnapshot& snap = m_bridge->pub[pi];

    f32   block[kMixBlockFrames * 2U]{};
    usize capOff = 0U;
    for (u32 b = 0U; b < blocks; ++b) {
        m_mixer.Mix(snap, std::span<f32, kMixBlockFrames * 2U>{block, kMixBlockFrames * 2U});

        const u64 dropped = m_bridge->active & ~m_mixer.ActiveMask();
        if (dropped != 0ULL) {
            m_bridge->done.fetch_or(dropped, std::memory_order_release);
        }

        if (pushToDevice && m_bridge->stream) {
            (void)SDL_PutAudioStreamData(m_bridge->stream, block, static_cast<int>(sizeof(block)));
        }
        if (!captureOut.empty() && capOff + kMixBlockFrames * 2U <= captureOut.size()) {
            std::memcpy(captureOut.data() + capOff, block, sizeof(block));
            capOff += kMixBlockFrames * 2U;
        }
    }
}

void AudioSystem::PullDeviceBytes(i32 bytes) noexcept {
    if (!m_bridge || bytes <= 0) return;
    constexpr i32 kBlockBytes = static_cast<i32>(kMixBlockFrames * 2U * sizeof(f32));
    const u32     n           = static_cast<u32>((bytes + kBlockBytes - 1) / kBlockBytes);
    RunBlocks(std::max(1U, n), {}, true);
}

void AudioSystem::Pump(u32 blocks, std::span<f32> captureOut) noexcept {
    if (m_dirty) PublishSnapshot();
    RunBlocks(blocks, captureOut, false);
    SyncFinishedVoices();
}

u32 AudioSystem::ExtractEmitters(std::span<AudioEmitterView> out) const noexcept {
    if (!m_bridge || out.empty()) return 0U;
    u32 n = 0U;
    u64 m = m_spatialMask & m_bridge->active;
    while (m != 0ULL && n < out.size()) {
        const u8 s = static_cast<u8>(std::countr_zero(m));
        m &= m - 1ULL;
        const VoiceTracker& tr = m_bridge->trackers[s];
        if (tr.cueIndex >= kMaxCues) continue;
        const SoundCueDef& def = m_bridge->cues[tr.cueIndex];
        const bool occ = (m_bridge->snap.occludedMask & (1ULL << s)) != 0ULL;
        out[n++] = AudioEmitterView{
            .position  = tr.position,
            .maxDist   = def.maxDist,
            .occlusion = occ ? 1.0f : 0.0f,
        };
    }
    return n;
}

u64 AudioSystem::ComputeAuditHash() const noexcept {
    u64 h = 0xCBF29CE484222325ULL;
    if (!m_bridge) return h;
    h = HashCombine(h, m_bridge->rng.State());
    h = HashCombine(h, m_bridge->nextGen);
    h = HashCombine(h, m_bridge->snap.occludedMask);
    h = HashCombine(h, static_cast<u64>(std::bit_cast<u32>(m_listener.x)) |
                       (static_cast<u64>(std::bit_cast<u32>(m_listener.y)) << 32U));
    for (u8 i = 0U; i < kAudioBusCount; ++i) {
        h = HashCombine(h, std::bit_cast<u32>(m_bridge->snap.busGain[i]));
    }
    return HashCombine(h, m_mixer.ComputeAuditHash());
}

} // namespace en
