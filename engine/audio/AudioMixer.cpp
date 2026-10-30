#include "audio/AudioMixer.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace en {
namespace {

constexpr u32 kMinRate = 8000U;
constexpr u8 kDuckSrc = static_cast<u8>(
    (1U << static_cast<u8>(AudioBusId::Sfx)) | (1U << static_cast<u8>(AudioBusId::Ui)));
constexpr u8 kDuckDst = static_cast<u8>(
    (1U << static_cast<u8>(AudioBusId::Music)) | (1U << static_cast<u8>(AudioBusId::Ambience)));

void SvfCoeffs(f32 hz, f32& a1, f32& a2, f32& a3) noexcept {
    hz = std::clamp(hz, kMinCutoffHz, kMaxCutoffHz);
    const f32 w = kPi * hz * kInvSampleRate;
    const f32 w2 = w * w;
    const f32 g = w * (1.0f + w2 * (0.3333333f + 0.1333333f * w2));
    a1 = 1.0f / (1.0f + g * (g + kSvfDamp));
    a2 = g * a1;
    a3 = g * a2;
}

[[nodiscard]] constexpr u64 RateStep(u32 rate) noexcept {
    rate = std::max(kMinRate, rate);
    return rate == kAudioSampleRate ? kPhaseOne
                                    : (static_cast<u64>(rate) << 32U) / kAudioSampleRate;
}

[[nodiscard]] u64 Wrap(u64 phase, u32 frames, u32 loopStart) noexcept {
    if (frames == 0U) return 0ULL;
    const u64 end = static_cast<u64>(frames) << 32U;
    if (phase < end) return phase;
    const u32 s = (loopStart + 1U < frames) ? loopStart : 0U;
    const u64 sp = static_cast<u64>(s) << 32U;
    const u64 span = std::max(kPhaseOne, end - sp);
    return sp + ((phase - sp) % span);
}

void Sample(const f32* pcm, u32 frames, u32 loopStart, u8 ch,
            i64 idx, f32 frac, bool loop, f32& oL, f32& oR) noexcept {
    const i64 end = static_cast<i64>(frames);
    if (frac <= kSilence && idx >= 0 && idx < end) {
        const usize b = static_cast<usize>(idx) * ch;
        oL = pcm[b];
        oR = ch >= 2U ? pcm[b + 1U] : oL;
        return;
    }
    if (idx >= 1 && idx + 2 < end) {
        if (ch == 1U) {
            oL = oR = AudioMixer::Hermite4(pcm[idx - 1], pcm[idx], pcm[idx + 1], pcm[idx + 2], frac);
            return;
        }
        const usize b = static_cast<usize>(idx - 1) * 2U;
        oL = AudioMixer::Hermite4(pcm[b], pcm[b + 2], pcm[b + 4], pcm[b + 6], frac);
        oR = AudioMixer::Hermite4(pcm[b + 1], pcm[b + 3], pcm[b + 5], pcm[b + 7], frac);
        return;
    }
    auto at = [&](i64 i, f32& l, f32& r) noexcept {
        if (!loop) {
            if (i >= end) { l = r = 0.0f; return; }
            i = std::clamp<i64>(i, 0, std::max<i64>(0, end - 1));
        } else if (end > 1) {
            const i64 s = static_cast<i64>(loopStart) < end - 1 ? static_cast<i64>(loopStart) : 0;
            i = i < s ? std::max<i64>(0, i) : s + ((i - s) % (end - s));
        } else {
            i = 0;
        }
        const usize b = static_cast<usize>(i) * ch;
        l = pcm[b];
        r = ch >= 2U ? pcm[b + 1U] : l;
    };
    f32 aL, aR, bL, bR, cL, cR, dL, dR;
    at(idx - 1, aL, aR);
    at(idx,     bL, bR);
    at(idx + 1, cL, cR);
    at(idx + 2, dL, dR);
    oL = AudioMixer::Hermite4(aL, bL, cL, dL, frac);
    oR = ch >= 2U ? AudioMixer::Hermite4(aR, bR, cR, dR, frac) : oL;
}

struct Deck {
    const f32* pcm[kMaxMusicStems]{};
    u64 phase[kMaxMusicStems]{};
    u64 step[kMaxMusicStems]{};
    u32 frames[kMaxMusicStems]{};
    f32 gain[kMaxMusicStems]{};
    u8  ch[kMaxMusicStems]{};
    u8  scope[kMaxMusicStems]{};
    f32 fade{0.0f};
    f32 fadeStep{0.0f};
    u32 loopStart{0U};
    u8  mask{0U};
    bool loop{true};
};

void MixDeck(Deck& d, const f32* weights, bool fadeOut, f32* L, f32* R) noexcept {
    const f32 f0 = d.fade;
    const f32 f1 = fadeOut ? std::max(0.0f, f0 - d.fadeStep) : std::min(1.0f, f0 + d.fadeStep);
    d.fade = f1;
    for (u8 s = 0U; s < kMaxMusicStems; ++s) {
        if ((d.mask & (1U << s)) == 0U || !d.pcm[s]) continue;
        const u32 fr = d.frames[s];
        const u8 ch = d.ch[s];
        u64 ph = d.phase[s];
        const u64 st = d.step[s];
        const f32 w = (weights ? weights[s] : d.gain[s]) * f1;
        const f32 dg = (w - d.gain[s]) * kInvMixBlockFrames;
        f32 g = d.gain[s];
        for (u32 i = 0U; i < kMixBlockFrames; ++i) {
            const i64 bi = static_cast<i64>(ph >> 32U);
            const f32 frac = static_cast<f32>(ph & kPhaseFracMask) * kInvPhaseOne;
            f32 sL = 0.0f, sR = 0.0f;
            Sample(d.pcm[s], fr, d.loopStart, ch, bi, frac, d.loop, sL, sR);
            g += dg;
            L[i] += sL * g;
            R[i] += sR * g;
            ph += st;
        }
        d.gain[s] = w;
        d.phase[s] = d.loop ? Wrap(ph, fr, d.loopStart) : ph;
        if (!d.loop && (ph >> 32U) >= fr) d.mask &= static_cast<u8>(~(1U << s));
    }
    if (fadeOut && f1 <= kSilence) d.mask = 0U;
}

} // namespace

struct alignas(64) AudioMixer::State {
    const f32* pcm[kMaxAudioVoices]{};
    u64 phase[kMaxAudioVoices]{};
    u64 step[kMaxAudioVoices]{};
    u32 frames[kMaxAudioVoices]{};
    u32 loopStart[kMaxAudioVoices]{};
    f32 gainL[kMaxAudioVoices]{};
    f32 gainR[kMaxAudioVoices]{};
    f32 pitch[kMaxAudioVoices]{};
    f32 ic1L[kMaxAudioVoices]{};
    f32 ic2L[kMaxAudioVoices]{};
    f32 ic1R[kMaxAudioVoices]{};
    f32 ic2R[kMaxAudioVoices]{};
    u16 gen[kMaxAudioVoices]{};
    u16 fade[kMaxAudioVoices]{};
    u8  ch[kMaxAudioVoices]{};
    u8  bus[kMaxAudioVoices]{};
    VoiceFlags flags[kMaxAudioVoices]{};
    u8  scope[kMaxAudioVoices]{};

    Deck active{};
    Deck prev{};

    f32 busCur[kAudioBusCount]{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    f32 duckEnv{0.0f};
    f32 mute{1.0f};

    alignas(64) f32 planeL[kAudioBusCount][kMixBlockFrames]{};
    alignas(64) f32 planeR[kAudioBusCount][kMixBlockFrames]{};
};
static_assert(sizeof(AudioMixer::State) % 64U == 0U);

constexpr usize AudioMixer::RequiredArenaBytes() noexcept { return sizeof(State); }

Result<void, AudioErrc> AudioMixer::Init(std::span<u8> arena) noexcept {
    if (arena.size() < sizeof(State) || (reinterpret_cast<usize>(arena.data()) & 63U) != 0U) {
        return Err(AudioErrc::OutOfMemory);
    }
    m_state = reinterpret_cast<State*>(arena.data());
    Reset();
    return {};
}

void AudioMixer::Reset() noexcept {
    if (m_state) {
        *m_state = State{};
        m_state->active.fade = 1.0f;
    }
    m_active = 0ULL;
    m_tel = {};
}

void AudioMixer::Apply(const AudioCommand& cmd) noexcept {
    if (!m_state) return;
    switch (cmd.type) {
    case AudioCmdType::Spawn: {
        const u8 s = cmd.slot % kMaxAudioVoices;
        const u64 bit = 1ULL << s;
        if ((m_active & bit) != 0ULL) ++m_tel.stolen;
        if (!cmd.pcm || cmd.frames == 0U) {
            m_active &= ~bit;
            return;
        }
        m_state->pcm[s]       = cmd.pcm;
        m_state->phase[s]     = 0ULL;
        m_state->step[s]      = RateStep(cmd.sampleRate);
        m_state->frames[s]    = cmd.frames;
        m_state->loopStart[s] = (cmd.loopStart + 1U < cmd.frames) ? cmd.loopStart : 0U;
        m_state->gainL[s]     = cmd.gainL;
        m_state->gainR[s]     = cmd.gainR;
        m_state->pitch[s]     = std::clamp(cmd.pitch, kMinPitch, kMaxPitch);
        m_state->ic1L[s] = m_state->ic2L[s] = m_state->ic1R[s] = m_state->ic2R[s] = 0.0f;
        m_state->gen[s]       = cmd.generation;
        m_state->fade[s]      = 0U;
        m_state->ch[s]        = std::max<u8>(1U, cmd.channels);
        m_state->bus[s]       = static_cast<u8>(cmd.bus) % kAudioBusCount;
        m_state->flags[s]     = cmd.flags;
        m_state->scope[s]     = cmd.scope;
        m_active |= bit;
        break;
    }
    case AudioCmdType::Stop: {
        const u8 s = cmd.slot % kMaxAudioVoices;
        const u64 bit = 1ULL << s;
        if ((m_active & bit) == 0ULL) break;
        if (cmd.generation != 0U && m_state->gen[s] != cmd.generation) break;
        if (cmd.fadeBlocks == 0U) m_active &= ~bit;
        else m_state->fade[s] = cmd.fadeBlocks;
        break;
    }
    case AudioCmdType::CommitDeck: {
        if (m_state->active.mask != 0U && cmd.fadeBlocks > 1U) {
            m_state->prev = m_state->active;
            m_state->prev.fadeStep = 1.0f / static_cast<f32>(cmd.fadeBlocks);
        } else {
            m_state->prev.mask = 0U;
        }
        m_state->active = Deck{};
        m_state->active.loopStart = cmd.loopStart;
        m_state->active.loop = HasFlag(cmd.flags, VoiceFlags::Looping);
        if (cmd.fadeBlocks > 1U) {
            m_state->active.fade = 0.0f;
            m_state->active.fadeStep = 1.0f / static_cast<f32>(cmd.fadeBlocks);
        } else {
            m_state->active.fade = 1.0f;
        }
        break;
    }
    case AudioCmdType::BindStem: {
        const u8 st = cmd.slot & 3U;
        Deck& d = m_state->active;
        if (cmd.pcm && cmd.frames > 0U) {
            d.pcm[st]    = cmd.pcm;
            d.frames[st] = cmd.frames;
            d.ch[st]     = std::max<u8>(1U, cmd.channels);
            d.scope[st]  = cmd.scope;
            d.gain[st]   = cmd.gainL;
            d.phase[st]  = 0ULL;
            d.step[st]   = RateStep(cmd.sampleRate);
            d.mask      |= static_cast<u8>(1U << st);
        }
        break;
    }
    case AudioCmdType::Panic:
        Reset();
        break;
    }
}

void AudioMixer::Mix(const AudioFrameSnapshot& snap, std::span<f32, kMixBlockFrames * 2U> out) noexcept {
    if (!m_state) {
        std::memset(out.data(), 0, out.size_bytes());
        return;
    }

    u8 dirty = 0U;
    auto ensure = [&](u8 b) noexcept {
        const u8 bit = static_cast<u8>(1U << (b % kAudioBusCount));
        if ((dirty & bit) == 0U) {
            std::memset(m_state->planeL[b], 0, sizeof(m_state->planeL[b]));
            std::memset(m_state->planeR[b], 0, sizeof(m_state->planeR[b]));
            dirty |= bit;
        }
    };

    f32 a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    const bool needSvf = snap.occludedMask != 0ULL && snap.occCutoffHz < kSvfBypassHz;
    if (needSvf) SvfCoeffs(snap.occCutoffHz, a1, a2, a3);

    u64 mask = m_active & ~snap.pausedMask;
    while (mask != 0ULL) {
        const u8 s = static_cast<u8>(std::countr_zero(mask));
        mask &= mask - 1ULL;
        const u64 bit = 1ULL << s;

        const f32* pcm = m_state->pcm[s];
        const u32 frames = m_state->frames[s];
        if (!pcm || frames == 0U) {
            m_active &= ~bit;
            continue;
        }

        f32 curL = m_state->gainL[s];
        f32 curR = m_state->gainR[s];
        f32 tgtL = snap.gainL[s];
        f32 tgtR = snap.gainR[s];

        bool fadeDone = false;
        if (m_state->fade[s] > 0U) {
            const u16 rem = m_state->fade[s]--;
            const f32 r = static_cast<f32>(rem - 1U) / static_cast<f32>(rem);
            tgtL *= r;
            tgtR *= r;
            fadeDone = rem == 1U;
        }

        const bool looping = HasFlag(m_state->flags[s], VoiceFlags::Looping);
        const f32 pitch = snap.pitch[s] > 0.0f ? snap.pitch[s] : m_state->pitch[s];
        m_state->pitch[s] = pitch;
        const u64 base = m_state->step[s];
        const u64 step = pitch == 1.0f ? base : static_cast<u64>(static_cast<f32>(base) * pitch);

        // Silent looping: advance phase only
        if (looping && !fadeDone &&
            curL <= kSilence && curR <= kSilence && tgtL <= kSilence && tgtR <= kSilence) {
            m_state->phase[s] = Wrap(m_state->phase[s] + step * kMixBlockFrames, frames, m_state->loopStart[s]);
            m_state->gainL[s] = m_state->gainR[s] = 0.0f;
            continue;
        }

        const u8 bus = m_state->bus[s] % kAudioBusCount;
        ensure(bus);
        f32* bL = m_state->planeL[bus];
        f32* bR = m_state->planeR[bus];
        const f32 dL = (tgtL - curL) * kInvMixBlockFrames;
        const f32 dR = (tgtR - curR) * kInvMixBlockFrames;
        const bool svf = needSvf && (snap.occludedMask & bit) != 0ULL;
        f32 ic1L = m_state->ic1L[s], ic2L = m_state->ic2L[s];
        f32 ic1R = m_state->ic1R[s], ic2R = m_state->ic2R[s];
        u64 phase = m_state->phase[s];
        const u8 ch = m_state->ch[s];
        const u32 ls = m_state->loopStart[s];
        bool done = fadeDone;

        for (u32 i = 0U; i < kMixBlockFrames; ++i) {
            const i64 bi = static_cast<i64>(phase >> 32U);
            if (!looping && bi >= static_cast<i64>(frames)) {
                done = true;
                break;
            }
            const f32 frac = static_cast<f32>(phase & kPhaseFracMask) * kInvPhaseOne;
            f32 sL = 0.0f, sR = 0.0f;
            Sample(pcm, frames, ls, ch, bi, frac, looping, sL, sR);
            if (svf) {
                sL = SvfLp(sL, ic1L, ic2L, a1, a2, a3);
                sR = ch >= 2U ? SvfLp(sR, ic1R, ic2R, a1, a2, a3) : sL;
            }
            curL += dL;
            curR += dR;
            bL[i] += sL * curL;
            bR[i] += sR * curR;
            phase += step;
        }

        if (done) {
            m_active &= ~bit;
        } else {
            m_state->phase[s] = looping ? Wrap(phase, frames, ls) : phase;
            m_state->gainL[s] = tgtL;
            m_state->gainR[s] = tgtR;
            if (svf) {
                m_state->ic1L[s] = ic1L;
                m_state->ic2L[s] = ic2L;
                m_state->ic1R[s] = ic1R;
                m_state->ic2R[s] = ic2R;
            }
        }
    }

    constexpr u8 kMus = static_cast<u8>(AudioBusId::Music);
    if (m_state->prev.mask != 0U) {
        ensure(kMus);
        MixDeck(m_state->prev, nullptr, true, m_state->planeL[kMus], m_state->planeR[kMus]);
    }
    if (m_state->active.mask != 0U) {
        ensure(kMus);
        MixDeck(m_state->active, snap.stemWeights.data(), false,
                m_state->planeL[kMus], m_state->planeR[kMus]);
    }

    if (dirty == 0U) {
        std::memset(out.data(), 0, out.size_bytes());
        m_state->duckEnv *= 0.95f;
        m_state->mute = snap.muted ? 0.0f : 1.0f;
        m_tel.blocks += 1ULL;
        m_tel.peakL = m_tel.peakR = 0.0f;
        m_tel.voices = static_cast<u16>(std::popcount(m_active));
        m_tel.stems = static_cast<u16>(std::popcount(m_state->active.mask));
        return;
    }

    f32 trig = 0.0f;
    for (u8 b = 0U; b < kAudioBusCount; ++b) {
        if ((kDuckSrc & (1U << b)) == 0U || (dirty & (1U << b)) == 0U) continue;
        for (u32 i = 0U; i < kMixBlockFrames; ++i) {
            trig = std::max(trig, std::abs(m_state->planeL[b][i]));
            trig = std::max(trig, std::abs(m_state->planeR[b][i]));
        }
    }
    const f32 duckT = trig > 0.05f ? snap.duckStrength : 0.0f;
    m_state->duckEnv += (duckT - m_state->duckEnv) * 0.15f;

    const f32 muteT = snap.muted ? 0.0f : 1.0f;
    f32 mute = m_state->mute;
    const f32 dMute = (muteT - mute) * kInvMixBlockFrames;
    f32 peakL = 0.0f, peakR = 0.0f;

    for (u32 i = 0U; i < kMixBlockFrames; ++i) {
        mute += dMute;
        f32 sumL = 0.0f, sumR = 0.0f;
        for (u8 b = 0U; b < kAudioBusCount; ++b) {
            if ((dirty & (1U << b)) == 0U) continue;
            f32 g = snap.busGain[b];
            if ((kDuckDst & (1U << b)) != 0U) g *= (1.0f - m_state->duckEnv);
            m_state->busCur[b] += (g - m_state->busCur[b]) * 0.25f;
            const f32 bg = m_state->busCur[b];
            sumL += m_state->planeL[b][i] * bg;
            sumR += m_state->planeR[b][i] * bg;
        }
        sumL = SoftClip(sumL * mute);
        sumR = SoftClip(sumR * mute);
        out[i * 2U]     = sumL;
        out[i * 2U + 1] = sumR;
        peakL = std::max(peakL, std::abs(sumL));
        peakR = std::max(peakR, std::abs(sumR));
    }
    m_state->mute = muteT;
    m_tel.blocks += 1ULL;
    m_tel.peakL = peakL;
    m_tel.peakR = peakR;
    m_tel.duckEnv = m_state->duckEnv;
    m_tel.voices = static_cast<u16>(std::popcount(m_active));
    m_tel.stems = static_cast<u16>(std::popcount(m_state->active.mask));
}

u32 AudioMixer::ScopeMask() const noexcept {
    if (!m_state) return 0U;
    u32 m = 0U;
    u64 v = m_active;
    while (v != 0ULL) {
        const u8 s = static_cast<u8>(std::countr_zero(v));
        v &= v - 1ULL;
        m |= 1U << (m_state->scope[s] & 31U);
    }
    for (u8 st = 0U; st < kMaxMusicStems; ++st) {
        if ((m_state->active.mask & (1U << st)) != 0U)
            m |= 1U << (m_state->active.scope[st] & 31U);
        if ((m_state->prev.mask & (1U << st)) != 0U)
            m |= 1U << (m_state->prev.scope[st] & 31U);
    }
    return m;
}

u64 AudioMixer::AuditHash() const noexcept {
    u64 h = 0xCBF29CE484222325ULL;
    h = HashCombine(h, m_active);
    h = HashCombine(h, m_tel.blocks);
    h = HashCombine(h, static_cast<u64>(m_tel.voices) | (static_cast<u64>(m_tel.stolen) << 16U));
    if (m_state) {
        u64 v = m_active;
        while (v != 0ULL) {
            const u8 s = static_cast<u8>(std::countr_zero(v));
            v &= v - 1ULL;
            h = HashCombine(h, m_state->phase[s]);
        }
        for (u8 st = 0U; st < kMaxMusicStems; ++st) {
            if ((m_state->active.mask & (1U << st)) != 0U)
                h = HashCombine(h, m_state->active.phase[st]);
        }
    }
    return h;
}

} // namespace en
