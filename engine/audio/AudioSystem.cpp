#include "audio/AudioSystem.h"
#include "assets/AssetBank.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <atomic>
#include <bit>
#include <cstring>

namespace en {
namespace {

constexpr u32 kMaxCues   = 256U;
constexpr u32 kMaxTracks = 32U;
constexpr u32 kCmdRing   = 256U;
constexpr u32 kHashSize  = 512U;

[[nodiscard]] constexpr f32 Atten(AttenuationCurve c, f32 d, f32 mn, f32 mx) noexcept {
    if (c == AttenuationCurve::None || d <= mn) return 1.0f;
    if (d >= mx || mx <= mn) return 0.0f;
    const f32 t = (d - mn) / (mx - mn);
    switch (c) {
        case AttenuationCurve::Linear:         return 1.0f - t;
        case AttenuationCurve::InverseClamped: return mn / (mn + 4.0f * (d - mn));
        case AttenuationCurve::Smoothstep:     return 1.0f - t * t * (3.0f - 2.0f * t);
        default: return 1.0f - t;
    }
}

[[nodiscard]] constexpr u32 Hash(u32 k) noexcept {
    const u32 x = k * 0x9E3779B9U;
    return (x ^ (x >> 16U)) & (kHashSize - 1U);
}

struct alignas(8) Tracker {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 gain{1.0f};
    f32 pitch{1.0f};
    u32 emitter{kNullAudioEmitter};
    u16 gen{0U};
    u16 cue{0U};
};
static_assert(sizeof(Tracker) == 24);

} // namespace

struct alignas(64) AudioSystem::Bridge {
    AudioFrameSnapshot snap{};
    std::array<AudioFrameSnapshot, 2> pub{};
    std::array<AudioCommand, kCmdRing> ring{};
    std::array<SoundCueDef, kMaxCues> cues{};
    std::array<u16, kHashSize> hash{};
    std::array<u8, kMaxCues> scopes{};
    std::array<u16, kMaxCues> lastTick{};
    std::array<u8, kMaxCues> concurrent{};
    std::array<MusicTrackDef, kMaxTracks> tracks{};
    std::array<u8, kMaxTracks> trackScopes{};
    std::array<Tracker, kMaxAudioVoices> trackers{};
    SDL_AudioStream* stream{nullptr};
    Rng rng{};
    u64 active{0ULL};
    f32 invPan{1.0f / 16.0f};
    std::atomic<u64> done{0ULL};
    std::atomic<u32> scopesAt{0U};
    std::atomic<u32> w{0U};
    std::atomic<u32> r{0U};
    std::atomic<u32> pubIdx{0U};
    u16 cueN{0U};
    u16 trackN{0U};
    u16 nextGen{1U};
    bool ownsSdl{false};

    [[nodiscard]] Tracker* Resolve(VoiceHandle h) noexcept {
        if (!h.IsValid() || h.index >= kMaxAudioVoices) return nullptr;
        Tracker& t = trackers[h.index];
        return t.gen == h.generation ? &t : nullptr;
    }

    [[nodiscard]] u16 FindCue(SoundCueId id) const noexcept {
        if (!id.IsValid()) return 0xFFFFU;
        u32 s = Hash(id.value);
        for (u32 p = 0U; p < kHashSize; ++p) {
            const u16 i = hash[s];
            if (i == 0xFFFFU) return 0xFFFFU;
            if (cues[i].id == id) return i;
            s = (s + 1U) & (kHashSize - 1U);
        }
        return 0xFFFFU;
    }

    void InsertCue(SoundCueId id, u16 idx) noexcept {
        u32 s = Hash(id.value);
        for (u32 p = 0U; p < kHashSize; ++p) {
            if (hash[s] == 0xFFFFU || hash[s] == idx) {
                hash[s] = idx;
                return;
            }
            s = (s + 1U) & (kHashSize - 1U);
        }
    }

    [[nodiscard]] u16 FindTrack(u32 id) const noexcept {
        for (u16 i = 0U; i < trackN; ++i)
            if (tracks[i].trackId == id) return i;
        return 0xFFFFU;
    }
};

Result<void, AudioErrc> AudioSystem::Init(const AudioSystemConfig& cfg) noexcept {
    Shutdown();
    if (cfg.panWidth <= 0.1f) return Err(AudioErrc::InvalidConfig);

    const usize need = AudioMixer::RequiredArenaBytes() + sizeof(Bridge) + 256U;
    if (!m_arena.Init(std::max(cfg.arenaBytes, need))) return Err(AudioErrc::OutOfMemory);

    auto ms = m_arena.Alloc<u8>(AudioMixer::RequiredArenaBytes());
    auto bs = m_arena.Alloc<Bridge>(1U);
    if (ms.empty() || bs.empty()) { Shutdown(); return Err(AudioErrc::OutOfMemory); }
    if (auto r = m_mixer.Init(ms); !r) { Shutdown(); return r; }

    m_bridge = bs.data();
    m_bridge->invPan = 1.0f / cfg.panWidth;
    m_bridge->hash.fill(0xFFFFU);
    m_bridge->lastTick.fill(0xFFFFU);
    m_bridge->snap.occCutoffHz = std::clamp(cfg.occCutoffHz, kMinCutoffHz, kMaxCutoffHz);
    m_bridge->snap.pitch.fill(1.0f);
    m_bridge->pub[0] = m_bridge->snap;
    m_bridge->pub[1] = m_bridge->snap;

    if (!cfg.headless) {
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0U && SDL_InitSubSystem(SDL_INIT_AUDIO))
            m_bridge->ownsSdl = true;
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = static_cast<int>(kAudioSampleRate);
        auto cb = [](void* ud, SDL_AudioStream*, int extra, int) noexcept {
            if (ud && extra > 0) static_cast<AudioSystem*>(ud)->Pull(extra);
        };
        m_bridge->stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, cb, this);
        if (m_bridge->stream) (void)SDL_ResumeAudioStreamDevice(m_bridge->stream);
    }

    m_spatial = 0ULL;
    m_lx = m_ly = 0.0f;
    m_releaseMask = 0U;
    m_tick = 0U;
    m_muted = false;
    m_dirty = false;
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
    m_mixer = AudioMixer{};
    m_bridge = nullptr;
    m_spatial = 0ULL;
    m_lx = m_ly = 0.0f;
    m_releaseMask = 0U;
    m_tick = 0U;
    m_muted = false;
    m_dirty = false;
}

bool AudioSystem::HasDevice() const noexcept {
    return m_bridge && m_bridge->stream != nullptr;
}

void AudioSystem::SetFocusMuted(bool muted) noexcept {
    m_muted = muted;
    if (m_bridge) {
        m_bridge->snap.muted = muted;
        m_dirty = true;
    }
}

void AudioSystem::Publish() noexcept {
    if (!m_bridge) return;
    const u32 next = (m_bridge->pubIdx.load(std::memory_order_relaxed) + 1U) & 1U;
    m_bridge->pub[next] = m_bridge->snap;
    m_bridge->pubIdx.store(next, std::memory_order_release);
    m_dirty = false;
}

void AudioSystem::SyncDone() noexcept {
    if (!m_bridge) return;
    const u64 fin = m_bridge->done.exchange(0ULL, std::memory_order_acquire);
    if (fin == 0ULL) return;
    for (u8 i = 0U; i < kMaxAudioVoices; ++i) {
        if ((fin & (1ULL << i)) == 0ULL) continue;
        const u16 c = m_bridge->trackers[i].cue;
        if (c < kMaxCues && m_bridge->concurrent[c] > 0U) --m_bridge->concurrent[c];
    }
    m_bridge->active &= ~fin;
    m_spatial &= ~fin;
    m_bridge->snap.occludedMask &= ~fin;
}

Result<void, AudioErrc> AudioSystem::RegisterCue(const SoundCueDef& def, u8 scope) noexcept {
    if (!m_bridge || !def.id.IsValid()) return Err(AudioErrc::InvalidConfig);
    const u16 ex = m_bridge->FindCue(def.id);
    if (ex != 0xFFFFU) {
        m_bridge->cues[ex] = def;
        m_bridge->scopes[ex] = scope;
        return {};
    }
    if (m_bridge->cueN >= kMaxCues) return Err(AudioErrc::CueTableFull);
    const u16 i = m_bridge->cueN++;
    m_bridge->cues[i] = def;
    m_bridge->scopes[i] = scope;
    m_bridge->InsertCue(def.id, i);
    return {};
}

Result<void, AudioErrc> AudioSystem::RegisterMusic(const MusicTrackDef& def, u8 scope) noexcept {
    if (!m_bridge || def.trackId == 0U) return Err(AudioErrc::InvalidConfig);
    const u16 ex = m_bridge->FindTrack(def.trackId);
    if (ex != 0xFFFFU) {
        m_bridge->tracks[ex] = def;
        m_bridge->trackScopes[ex] = scope;
        return {};
    }
    if (m_bridge->trackN >= kMaxTracks) return Err(AudioErrc::CueTableFull);
    const u16 i = m_bridge->trackN++;
    m_bridge->tracks[i] = def;
    m_bridge->trackScopes[i] = scope;
    return {};
}

Result<void, AudioErrc> AudioSystem::RegisterBank(const BlobRecord& blob, u8 scope) noexcept {
    if (!m_bridge || !blob.data || blob.size == 0U) return Err(AudioErrc::CorruptedBankBlob);
    const auto bytes = blob.Bytes();
    if (bytes.size() < sizeof(CookedAudioBankHeader)) return Err(AudioErrc::CorruptedBankBlob);
    const auto* hdr = reinterpret_cast<const CookedAudioBankHeader*>(bytes.data());
    const usize cb = static_cast<usize>(hdr->cueCount) * sizeof(SoundCueDef);
    const usize tb = static_cast<usize>(hdr->musicTrackCount) * sizeof(MusicTrackDef);
    if (hdr->magic != CookedAudioBankHeader::kMagic ||
        hdr->version != CookedAudioBankHeader::kVersion ||
        bytes.size() < sizeof(CookedAudioBankHeader) + cb + tb) {
        return Err(AudioErrc::CorruptedBankBlob);
    }
    const std::span<const SoundCueDef> cues{
        reinterpret_cast<const SoundCueDef*>(bytes.data() + sizeof(CookedAudioBankHeader)),
        hdr->cueCount};
    const std::span<const MusicTrackDef> tracks{
        reinterpret_cast<const MusicTrackDef*>(bytes.data() + sizeof(CookedAudioBankHeader) + cb),
        hdr->musicTrackCount};
    for (const auto& d : cues)
        if (auto r = RegisterCue(d, scope); !r) return r;
    for (const auto& t : tracks)
        if (auto r = RegisterMusic(t, scope); !r) return r;
    return {};
}

u32 AudioSystem::PollReleasableScopes() noexcept {
    if (!m_bridge || m_releaseMask == 0U) return 0U;
    Pump(0U);
    const u32 active = m_bridge->scopesAt.load(std::memory_order_acquire);
    const u32 rel = m_releaseMask & ~active;
    m_releaseMask &= ~rel;
    return rel;
}

void AudioSystem::Enqueue(const AudioCommand& cmd) noexcept {
    if (!m_bridge) return;
    const u32 w = m_bridge->w.load(std::memory_order_relaxed);
    const u32 next = (w + 1U) & (kCmdRing - 1U);
    if (next == m_bridge->r.load(std::memory_order_acquire)) return; // full: drop
    m_bridge->ring[w] = cmd;
    m_bridge->w.store(next, std::memory_order_release);
}

void AudioSystem::Spatial(f32 gain, f32 wx, f32 wy, const SoundCueDef& def,
                          f32 lx, f32 ly, f32& oL, f32& oR) const noexcept {
    if (!HasFlag(def.flags, VoiceFlags::Spatialized2D)) {
        const f32 g = std::clamp(gain * kEqPower, 0.0f, kMaxGain);
        oL = oR = g;
        return;
    }
    const f32 dx = wx - lx;
    const f32 dy = wy - ly;
    const f32 d2 = dx * dx + dy * dy;
    if (def.atten != AttenuationCurve::None &&
        (def.maxDist <= def.minDist || d2 >= def.maxDist * def.maxDist)) {
        oL = oR = 0.0f;
        return;
    }
    if (d2 <= kSilence) {
        const f32 g = std::clamp(gain * kEqPower, 0.0f, kMaxGain);
        oL = oR = g;
        return;
    }
    const f32 dist = std::sqrt(d2);
    const f32 att = Atten(def.atten, dist, def.minDist, def.maxDist);
    const f32 pan = std::clamp(dx * m_bridge->invPan, -1.0f, 1.0f);
    const f32 ang = pan * (kPi * 0.25f);
    const f32 c = std::cos(ang);
    const f32 s = std::sin(ang);
    const f32 g = std::clamp(gain * att, 0.0f, kMaxGain);
    oL = g * kEqPower * (c - s);
    oR = g * kEqPower * (c + s);
}

VoiceHandle AudioSystem::Play(SoundCueId id, const AssetBank& assets,
                              f32 x, f32 y, u32 emitter) noexcept {
    if (!m_bridge || !id.IsValid()) return kNullVoiceHandle;
    const u16 ci = m_bridge->FindCue(id);
    if (ci == 0xFFFFU) return kNullVoiceHandle;
    const SoundCueDef& def = m_bridge->cues[ci];

    // Cooldown
    if (def.cooldownTicks > 0U && m_bridge->lastTick[ci] != 0xFFFFU &&
        static_cast<u16>(m_tick - m_bridge->lastTick[ci]) < def.cooldownTicks) {
        return kNullVoiceHandle;
    }
    // Concurrency
    if (def.maxConcurrency > 0U && m_bridge->concurrent[ci] >= def.maxConcurrency) {
        return kNullVoiceHandle;
    }

    // One per entity
    if (HasFlag(def.flags, VoiceFlags::OneVoicePerEntity) && emitter != kNullAudioEmitter) {
        for (u8 i = 0U; i < kMaxAudioVoices; ++i) {
            if ((m_bridge->active & (1ULL << i)) == 0ULL) continue;
            Tracker& t = m_bridge->trackers[i];
            if (t.emitter == emitter && t.cue == ci) {
                Stop(VoiceHandle{i, t.gen}, 1U);
                break;
            }
        }
    }

    // Free or steal lowest priority
    u8 slot = 0xFFU;
    u8 lowPri = 255U;
    for (u8 i = 0U; i < kMaxAudioVoices; ++i) {
        if ((m_bridge->active & (1ULL << i)) == 0ULL) {
            slot = i;
            break;
        }
        const u16 c = m_bridge->trackers[i].cue;
        const u8 p = c < kMaxCues ? m_bridge->cues[c].priority : 0U;
        if (p < lowPri) { lowPri = p; slot = i; }
    }
    if (slot == 0xFFU) return kNullVoiceHandle;
    if ((m_bridge->active & (1ULL << slot)) != 0ULL) {
        if (def.priority <= lowPri) return kNullVoiceHandle;
        Tracker& old = m_bridge->trackers[slot];
        Stop(VoiceHandle{slot, old.gen}, 0U);
    }

    AudioHandle clip = def.variants[0];
    if (def.variants[1].IsValid() && (m_bridge->rng.NextU64() & 1ULL) != 0ULL)
        clip = def.variants[1];
    const AudioRecord& rec = assets.GetAudio(clip);
    if (!rec.pcm || rec.frameCount == 0U) return kNullVoiceHandle;

    const f32 gain = def.baseGain * (def.gainJitter > 0.0f
        ? m_bridge->rng.Range(1.0f - def.gainJitter, 1.0f + def.gainJitter) : 1.0f);
    const f32 pitch = def.basePitch * (def.pitchJitter > 0.0f
        ? m_bridge->rng.Range(1.0f - def.pitchJitter, 1.0f + def.pitchJitter) : 1.0f);

    f32 gL = 0.0f, gR = 0.0f;
    Spatial(gain, x, y, def, m_lx, m_ly, gL, gR);

    const u16 gen = m_bridge->nextGen++;
    if (m_bridge->nextGen == 0U) m_bridge->nextGen = 1U;

    Tracker& tr = m_bridge->trackers[slot];
    tr = Tracker{};
    tr.worldX = x;
    tr.worldY = y;
    tr.gain = gain;
    tr.pitch = pitch;
    tr.emitter = emitter;
    tr.gen = gen;
    tr.cue = ci;

    AudioCommand cmd{};
    cmd.type = AudioCmdType::Spawn;
    cmd.pcm = rec.pcm;
    cmd.frames = rec.frameCount;
    cmd.loopStart = def.loopStart;
    cmd.gainL = gL;
    cmd.gainR = gR;
    cmd.pitch = pitch;
    cmd.sampleRate = rec.sampleRate;
    cmd.generation = gen;
    cmd.bus = def.bus;
    cmd.slot = slot;
    cmd.flags = def.flags;
    if (rec.looping) cmd.flags = cmd.flags | VoiceFlags::Looping;
    cmd.channels = rec.channels;
    cmd.scope = m_bridge->scopes[ci];
    Enqueue(cmd);

    m_bridge->active |= (1ULL << slot);
    if (HasFlag(def.flags, VoiceFlags::Spatialized2D)) m_spatial |= (1ULL << slot);
    m_bridge->snap.gainL[slot] = gL;
    m_bridge->snap.gainR[slot] = gR;
    m_bridge->snap.pitch[slot] = pitch;
    m_bridge->lastTick[ci] = m_tick;
    ++m_bridge->concurrent[ci];
    m_dirty = true;
    return VoiceHandle{slot, gen};
}

void AudioSystem::Stop(VoiceHandle h, u16 fade) noexcept {
    if (!m_bridge) return;
    Tracker* tr = m_bridge->Resolve(h);
    if (!tr) return;
    AudioCommand cmd{};
    cmd.type = AudioCmdType::Stop;
    cmd.slot = static_cast<u8>(h.index);
    cmd.generation = h.generation;
    cmd.fadeBlocks = fade;
    Enqueue(cmd);
    if (fade == 0U) {
        if (tr->cue < kMaxCues && m_bridge->concurrent[tr->cue] > 0U)
            --m_bridge->concurrent[tr->cue];
        m_bridge->active &= ~(1ULL << h.index);
        m_spatial &= ~(1ULL << h.index);
    }
    m_dirty = true;
}

void AudioSystem::StopAll(u16 fade) noexcept {
    if (!m_bridge) return;
    for (u8 i = 0U; i < kMaxAudioVoices; ++i) {
        if ((m_bridge->active & (1ULL << i)) == 0ULL) continue;
        Stop(VoiceHandle{i, m_bridge->trackers[i].gen}, fade);
    }
}

void AudioSystem::SetBusGain(AudioBusId bus, f32 gain) noexcept {
    if (!m_bridge) return;
    m_bridge->snap.busGain[static_cast<u8>(bus) % kAudioBusCount] =
        std::clamp(gain, 0.0f, kMaxGain);
    m_dirty = true;
}

bool AudioSystem::SetMusic(u32 trackId, const AssetBank& assets, u8 fade) noexcept {
    if (!m_bridge || trackId == 0U) return false;
    const u16 ti = m_bridge->FindTrack(trackId);
    if (ti == 0xFFFFU) return false;
    const MusicTrackDef& t = m_bridge->tracks[ti];

    AudioCommand commit{};
    commit.type = AudioCmdType::CommitDeck;
    commit.loopStart = t.loopStart;
    commit.fadeBlocks = fade;
    if (t.loop) commit.flags = VoiceFlags::Looping;
    Enqueue(commit);

    const u8 n = std::min(t.stemCount, static_cast<u8>(kMaxMusicStems));
    for (u8 s = 0U; s < n; ++s) {
        if (!t.stems[s].IsValid()) continue;
        const AudioRecord& rec = assets.GetAudio(t.stems[s]);
        if (!rec.pcm || rec.frameCount == 0U) continue;
        AudioCommand stem{};
        stem.type = AudioCmdType::BindStem;
        stem.pcm = rec.pcm;
        stem.frames = rec.frameCount;
        stem.gainL = t.weights[s];
        stem.sampleRate = rec.sampleRate;
        stem.slot = s;
        stem.channels = rec.channels;
        stem.scope = m_bridge->trackScopes[ti];
        Enqueue(stem);
    }
    for (u8 s = 0U; s < kMaxMusicStems; ++s)
        m_bridge->snap.stemWeights[s] = t.weights[s];
    m_dirty = true;
    return true;
}

void AudioSystem::SetStemWeights(std::span<const f32, kMaxMusicStems> w) noexcept {
    if (!m_bridge) return;
    for (u8 i = 0U; i < kMaxMusicStems; ++i)
        m_bridge->snap.stemWeights[i] = std::clamp(w[i], 0.0f, 2.0f);
    m_dirty = true;
}

void AudioSystem::Update(const AudioUpdateDesc& desc) noexcept {
    if (!m_bridge) return;
    SyncDone();
    m_tick = static_cast<u16>(desc.tick);
    m_lx = desc.listenerX;
    m_ly = desc.listenerY;

    if (desc.focusMute >= 0) SetFocusMuted(desc.focusMute != 0);

    if (desc.assets && !desc.triggers.empty()) {
        for (const auto& req : desc.triggers)
            (void)Play(req.cueId, *desc.assets, req.worldX, req.worldY, req.emitterId);
    }

    u64 paused = 0ULL;
    u64 occ = 0ULL;
    u64 live = m_bridge->active;
    while (live != 0ULL) {
        const u8 s = static_cast<u8>(std::countr_zero(live));
        live &= live - 1ULL;
        Tracker& tr = m_bridge->trackers[s];
        if (tr.cue >= kMaxCues) continue;
        const SoundCueDef& def = m_bridge->cues[tr.cue];

        if (HasFlag(def.flags, VoiceFlags::PauseOnFreeze) &&
            (desc.frozenMask & ChannelBit(def.channel)) != 0U) {
            paused |= (1ULL << s);
        }

        f32 pitch = tr.pitch;
        if (HasFlag(def.flags, VoiceFlags::FollowTimeScale) && desc.channelScales)
            pitch *= desc.channelScales[static_cast<u8>(def.channel)];
        m_bridge->snap.pitch[s] = std::clamp(pitch, kMinPitch, kMaxPitch);

        if ((m_spatial & (1ULL << s)) != 0ULL) {
            f32 gL = 0.0f, gR = 0.0f;
            Spatial(tr.gain, tr.worldX, tr.worldY, def, m_lx, m_ly, gL, gR);
            m_bridge->snap.gainL[s] = gL;
            m_bridge->snap.gainR[s] = gR;
        }

        if (!HasFlag(def.flags, VoiceFlags::BypassOcclusion) && desc.occlusion) {
            if (desc.occlusion(m_lx, m_ly, tr.worldX, tr.worldY, desc.occlusionCtx))
                occ |= (1ULL << s);
        }
    }
    m_bridge->snap.pausedMask = paused;
    m_bridge->snap.occludedMask = occ;
    m_dirty = true;
    Publish();

    if (!m_bridge->stream) Pump(0U);
}

void AudioSystem::Run(u32 blocks, std::span<f32> capture, bool device) noexcept {
    if (!m_bridge) return;

    u32 rd = m_bridge->r.load(std::memory_order_relaxed);
    const u32 wr = m_bridge->w.load(std::memory_order_acquire);
    while (rd != wr) {
        m_mixer.Apply(m_bridge->ring[rd]);
        rd = (rd + 1U) & (kCmdRing - 1U);
    }
    m_bridge->r.store(rd, std::memory_order_release);

    const u32 pi = m_bridge->pubIdx.load(std::memory_order_acquire);
    const AudioFrameSnapshot& snap = m_bridge->pub[pi];

    alignas(64) f32 block[kMixBlockFrames * 2U]{};
    usize capOff = 0U;
    for (u32 b = 0U; b < blocks; ++b) {
        m_mixer.Mix(snap, std::span<f32, kMixBlockFrames * 2U>{block, kMixBlockFrames * 2U});

        const u64 dropped = m_bridge->active & ~m_mixer.ActiveMask();
        if (dropped != 0ULL)
            m_bridge->done.fetch_or(dropped, std::memory_order_release);

        if (device && m_bridge->stream)
            (void)SDL_PutAudioStreamData(m_bridge->stream, block, static_cast<int>(sizeof(block)));
        if (!capture.empty() && capOff + kMixBlockFrames * 2U <= capture.size()) {
            std::memcpy(capture.data() + capOff, block, sizeof(block));
            capOff += kMixBlockFrames * 2U;
        }
    }
    m_bridge->scopesAt.store(m_mixer.ScopeMask(), std::memory_order_release);
}

void AudioSystem::Pull(i32 bytes) noexcept {
    if (!m_bridge || bytes <= 0) return;
    constexpr i32 kBytes = static_cast<i32>(kMixBlockFrames * 2U * sizeof(f32));
    const u32 n = static_cast<u32>((bytes + kBytes - 1) / kBytes);
    Run(std::max(1U, n), {}, true);
}

void AudioSystem::Pump(u32 blocks, std::span<f32> capture) noexcept {
    Run(blocks, capture, false);
    SyncDone();
}

u32 AudioSystem::ExtractEmitters(std::span<AudioEmitterView> out) const noexcept {
    if (!m_bridge || out.empty()) return 0U;
    u32 n = 0U;
    u64 m = m_spatial & m_bridge->active;
    while (m != 0ULL && n < out.size()) {
        const u8 s = static_cast<u8>(std::countr_zero(m));
        m &= m - 1ULL;
        const Tracker& tr = m_bridge->trackers[s];
        if (tr.cue >= kMaxCues) continue;
        const SoundCueDef& def = m_bridge->cues[tr.cue];
        const bool occ = (m_bridge->snap.occludedMask & (1ULL << s)) != 0ULL;
        out[n++] = AudioEmitterView{tr.worldX, tr.worldY, def.maxDist, occ ? 1.0f : 0.0f};
    }
    return n;
}

u64 AudioSystem::AuditHash() const noexcept {
    u64 h = 0xCBF29CE484222325ULL;
    if (!m_bridge) return h;
    h = HashCombine(h, m_bridge->rng.State());
    h = HashCombine(h, m_bridge->nextGen);
    h = HashCombine(h, m_bridge->snap.occludedMask);
    h = HashCombine(h, static_cast<u64>(std::bit_cast<u32>(m_lx)) |
                       (static_cast<u64>(std::bit_cast<u32>(m_ly)) << 32U));
    for (u8 i = 0U; i < kAudioBusCount; ++i)
        h = HashCombine(h, std::bit_cast<u32>(m_bridge->snap.busGain[i]));
    return HashCombine(h, m_mixer.AuditHash());
}

} // namespace en
