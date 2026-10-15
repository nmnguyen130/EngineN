#include "audio/AudioMixer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace en::audio {

namespace {

inline constexpr f32 kPcm16ToFloat   = 1.0f / 32768.0f;
inline constexpr f32 kQ14ToFloat     = 1.0f / 16384.0f;
inline constexpr f32 kInvBlockFrames = 1.0f / static_cast<f32>(kMixBlockFrames);
inline constexpr f32 kFracQ32ToFloat = 1.0f / 4294967296.0f;
inline constexpr f32 kAntiDenormalDc = 1.0e-20f;

[[nodiscard]] constexpr u8 AlignOffset64(const u8* ptr) noexcept {
    const usize addr = std::bit_cast<usize>(ptr);
    const usize rem  = addr & 63u;
    return rem == 0u ? 0u : static_cast<u8>(64u - rem);
}

// 4-point 3rd-order Hermite polynomial interpolation (Niemitalo, 2001).
// Eliminates high-frequency linear interpolation aliasing during slow-motion pitch ramps.
[[nodiscard]] constexpr f32 SampleHermite4(
    f32 ym1,
    f32 y0,
    f32 y1,
    f32 y2,
    f32 t) noexcept {
    const f32 c0 = y0;
    const f32 c1 = 0.5f * (y1 - ym1);
    const f32 c2 = ym1 - (2.5f * y0) + (2.0f * y1) - (0.5f * y2);
    const f32 c3 = (0.5f * (y2 - ym1)) + (1.5f * (y0 - y1));
    return (((c3 * t + c2) * t + c1) * t) + c0;
}

// Padé [3/2] rational approximant to tanh(x) for smooth master soft-clipping above -1.0 dBFS.
[[nodiscard]] constexpr f32 SoftClipPade(f32 x) noexcept {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const f32 x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

[[nodiscard]] constexpr u64 MixSplitMix64(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30u)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27u)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31u);
}

} // namespace

Result<void, AudioErrc> AudioMixer::Init(std::span<u8> arenaSlice) noexcept {
    if (arenaSlice.size() < RequiredArenaBytes()) {
        return Err(AudioErrc::ArenaExhausted);
    }

    std::memset(arenaSlice.data(), 0, arenaSlice.size());
    u8* cursor = arenaSlice.data();
    cursor += AlignOffset64(cursor);

    m_voices = std::construct_at(reinterpret_cast<VoicePoolSoA*>(cursor));
    cursor += sizeof(VoicePoolSoA);

    m_stealState = std::construct_at(reinterpret_cast<StealSlotState*>(cursor));
    cursor += sizeof(StealSlotState);

    m_decks = reinterpret_cast<MusicDeckRuntime*>(cursor);
    std::construct_at(&m_decks[0]);
    std::construct_at(&m_decks[1]);
    cursor += sizeof(MusicDeckRuntime) * 2u;

    m_busDsp = std::construct_at(reinterpret_cast<BusDspState*>(cursor));
    cursor += sizeof(BusDspState);

    const usize scratchBytesPerChannel = sizeof(f32) * kAudioBusCount * kMixBlockFrames;
    m_busScratchL = reinterpret_cast<f32*>(cursor);
    cursor += scratchBytesPerChannel;
    m_busScratchR = reinterpret_cast<f32*>(cursor);

    Reset();
    return Ok;
}

void AudioMixer::Reset() noexcept {
    if (!m_voices) {
        return;
    }
    *m_voices     = VoicePoolSoA{};
    *m_stealState = StealSlotState{};
    m_decks[0]    = MusicDeckRuntime{};
    m_decks[1]    = MusicDeckRuntime{};
    *m_busDsp     = BusDspState{};
    for (usize b = 0; b < kAudioBusCount; ++b) {
        m_busDsp->curGain[b] = 1.0f;
    }
    m_busDsp->masterFadeGain = 1.0f;
    m_deckCrossfadeAlpha     = 0.0f;
    m_deckCrossfadeStep      = 0.0f;
    m_telemetry              = AudioTelemetrySnapshot{};
    m_sampleCounter          = 0;
}

void AudioMixer::RefreshLoopGuard(u32 slot) noexcept {
    const i16* pcm     = m_voices->pcmPtr[slot];
    const u32  frames  = m_voices->frameCount[slot];
    const u32  loopBeg = m_voices->loopStart[slot];
    const bool looping = (m_voices->loopMask & (1ULL << slot)) != 0;
    i16* guard         = &m_voices->loopGuardSamples[slot * 4u];

    if (!pcm || frames == 0u) {
        guard[0] = guard[1] = guard[2] = guard[3] = 0;
        return;
    }
    // Pre-cache samples across the loop/end boundary so inner sample loops avoid branchy modulo wraps.
    guard[0] = pcm[frames > 1u ? frames - 2u : 0u];
    guard[1] = pcm[frames - 1u];
    if (looping && loopBeg < frames) {
        guard[2] = pcm[loopBeg];
        guard[3] = pcm[(loopBeg + 1u < frames) ? (loopBeg + 1u) : loopBeg];
    } else {
        guard[2] = 0;
        guard[3] = 0;
    }
}

void AudioMixer::CaptureStealTail(u32 slot, u8 stealIdx) noexcept {
    if (stealIdx >= kStealPoolSlots || (m_voices->activeMask & (1ULL << slot)) == 0) {
        return;
    }
    const i16* pcm    = m_voices->pcmPtr[slot];
    const u32  frames = m_voices->frameCount[slot];
    if (!pcm || frames < 4u) {
        return;
    }

    m_stealState->activeMask |= static_cast<u16>(1u << stealIdx);
    m_stealState->busIdx[stealIdx] = static_cast<u8>(m_voices->busAndChannel[slot] & 0x07u);
    m_stealState->gainL[stealIdx]  = m_voices->curGainL[slot];
    m_stealState->gainR[stealIdx]  = m_voices->curGainR[slot];

    f32* dst     = &m_stealState->samples[static_cast<usize>(stealIdx) * kMixBlockFrames];
    u64  phase   = m_voices->phaseQ32[slot];
    const u64 st = m_voices->baseStepQ32[slot];
    for (u32 i = 0; i < kMixBlockFrames; ++i) {
        const u32 idx  = static_cast<u32>(phase >> 32u);
        const f32 s    = (idx < frames) ? (static_cast<f32>(pcm[idx]) * kPcm16ToFloat) : 0.0f;
        const f32 env  = 1.0f - (static_cast<f32>(i) * kInvBlockFrames);
        dst[i]         = s * env;
        phase         += st;
    }
    ++m_telemetry.stolenVoicesTotal;
}

void AudioMixer::ApplyCommand(const AudioCommand& cmd) noexcept {
    if (!m_voices) {
        return;
    }

    switch (cmd.type) {
    case AudioCmdType::SpawnVoice: {
        const u32 slot = cmd.voiceSlot;
        if (slot >= kMaxVoices || !cmd.payload.spawn.pcm || cmd.payload.spawn.frameCount == 0u) {
            return;
        }
        const u64 bit = 1ULL << slot;
        const VoiceFlags vf = static_cast<VoiceFlags>(cmd.flags);

        m_voices->activeMask |= bit;
        m_voices->stoppingMask &= ~bit;
        if (HasVoiceFlag(vf, VoiceFlags::Looping)) {
            m_voices->loopMask |= bit;
        } else {
            m_voices->loopMask &= ~bit;
        }
        if (HasVoiceFlag(vf, VoiceFlags::PauseOnHitstop)) {
            m_voices->pauseOnHitstopMask |= bit;
        } else {
            m_voices->pauseOnHitstopMask &= ~bit;
        }
        if (HasVoiceFlag(vf, VoiceFlags::FollowTimeScale)) {
            m_voices->followTimeScaleMask |= bit;
        } else {
            m_voices->followTimeScaleMask &= ~bit;
        }

        m_voices->pcmPtr[slot]      = cmd.payload.spawn.pcm;
        m_voices->phaseQ32[slot]    = 0ULL;
        const u64 rateNum           = static_cast<u64>(cmd.payload.spawn.sampleRate) << 32u;
        m_voices->baseStepQ32[slot] = rateNum / kSampleRate;
        m_voices->frameCount[slot]  = cmd.payload.spawn.frameCount;
        m_voices->loopStart[slot]   = std::min(cmd.payload.spawn.loopStartFrame, cmd.payload.spawn.frameCount - 1u);
        m_voices->curGainL[slot]    = static_cast<f32>(cmd.payload.spawn.gainL_Q14) * kQ14ToFloat;
        m_voices->curGainR[slot]    = static_cast<f32>(cmd.payload.spawn.gainR_Q14) * kQ14ToFloat;
        m_voices->svfIc1eq[slot]    = 0.0f;
        m_voices->svfIc2eq[slot]    = 0.0f;
        m_voices->generation[slot]  = cmd.generation;
        m_voices->busAndChannel[slot] = cmd.packedBusAndChannel;
        m_voices->scopeId[slot]     = cmd.assetScopeId & 0x1Fu;
        RefreshLoopGuard(slot);
        break;
    }
    case AudioCmdType::StopVoice: {
        const u32 slot = cmd.voiceSlot;
        if (slot >= kMaxVoices || m_voices->generation[slot] != cmd.generation) {
            return;
        }
        if (cmd.payload.stop.stealPoolIdx < kStealPoolSlots) {
            CaptureStealTail(slot, cmd.payload.stop.stealPoolIdx);
            m_voices->activeMask &= ~(1ULL << slot);
        } else if (cmd.payload.stop.fadeOutBlocks == 0u) {
            m_voices->activeMask &= ~(1ULL << slot);
        } else {
            m_voices->stoppingMask |= (1ULL << slot);
            m_voices->stopBlocksRemaining[slot] = cmd.payload.stop.fadeOutBlocks;
        }
        break;
    }
    case AudioCmdType::BindMusicStem: {
        const u8 deckIdx = (cmd.payload.music.deckAndStemIdx >> 7u) & 0x01u;
        const u8 stemIdx = cmd.payload.music.deckAndStemIdx & 0x03u;
        MusicDeckRuntime& deck = m_decks[deckIdx];
        deck.stemPcm[stemIdx]  = cmd.payload.music.pcm;
        deck.frameCount        = cmd.payload.music.frameCount;
        deck.loopStartFrame    = cmd.payload.music.loopStartFrame;
        deck.loopEndFrame      = cmd.payload.music.loopEndFrame > 0u
                                     ? cmd.payload.music.loopEndFrame
                                     : cmd.payload.music.frameCount;
        deck.tempoBpmQ8        = cmd.payload.music.tempoBpmQ8;
        deck.stemCount         = std::max<u8>(deck.stemCount, static_cast<u8>(stemIdx + 1u));
        break;
    }
    case AudioCmdType::CommitMusicDeck: {
        const u8 crossfadeBlocks = cmd.payload.music.crossfadeBlocks;
        m_decks[1].cursorFrame   = 0u;
        m_decks[1].active        = true;
        if (!m_decks[0].active || crossfadeBlocks == 0u) {
            m_decks[0]           = m_decks[1];
            m_decks[1]           = MusicDeckRuntime{};
            m_deckCrossfadeAlpha = 0.0f;
            m_deckCrossfadeStep  = 0.0f;
        } else {
            m_deckCrossfadeAlpha = 0.0f;
            m_deckCrossfadeStep  = 1.0f / static_cast<f32>(crossfadeBlocks);
        }
        break;
    }
    case AudioCmdType::TriggerStinger:
        break;
    case AudioCmdType::PanicReset:
        Reset();
        break;
    }
}

void AudioMixer::MixActiveVoicesPlanar(const AudioFrameSnapshot& snapshot) noexcept {
    u64 mask = m_voices->activeMask;
    while (mask != 0ULL) {
        const u32 slot = static_cast<u32>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        const u64 bit = 1ULL << slot;

        const u8  busAndCh  = m_voices->busAndChannel[slot];
        const u8  busIdx    = static_cast<u8>(busAndCh & 0x07u);
        const u8  channelId = static_cast<u8>((busAndCh >> 3u) & 0x07u);
        const bool isFrozen = (snapshot.frozenChannelMask & (1u << channelId)) != 0u;
        const bool pauseHit = (m_voices->pauseOnHitstopMask & bit) != 0ULL;

        f32 targetL = static_cast<f32>(snapshot.targetGainL_Q14[slot]) * kQ14ToFloat;
        f32 targetR = static_cast<f32>(snapshot.targetGainR_Q14[slot]) * kQ14ToFloat;

        if (isFrozen && pauseHit) {
            targetL = 0.0f;
            targetR = 0.0f;
        } else if ((m_voices->stoppingMask & bit) != 0ULL) {
            targetL = 0.0f;
            targetR = 0.0f;
        }

        f32 gainL       = m_voices->curGainL[slot];
        f32 gainR       = m_voices->curGainR[slot];
        const f32 stepL = (targetL - gainL) * kInvBlockFrames;
        const f32 stepR = (targetR - gainR) * kInvBlockFrames;

        // If frozen by hitstop and already ramped to silence, hold phaseQ32 constant.
        if (isFrozen && pauseHit && gainL <= 1.0e-5f && gainR <= 1.0e-5f) {
            m_voices->curGainL[slot] = 0.0f;
            m_voices->curGainR[slot] = 0.0f;
            continue;
        }

        u64 pitchScaleQ12 = snapshot.targetPitchQ12[slot] != 0u ? snapshot.targetPitchQ12[slot] : 4096u;
        if ((m_voices->followTimeScaleMask & bit) != 0ULL) {
            const u64 chScale = snapshot.channelTimeScaleQ12[channelId];
            pitchScaleQ12     = (pitchScaleQ12 * (chScale != 0u ? chScale : 4096u)) >> 12u;
        }
        pitchScaleQ12     = std::clamp<u64>(pitchScaleQ12, 1024u, 8192u);
        const u64 stepQ32 = (m_voices->baseStepQ32[slot] * pitchScaleQ12) >> 12u;

        const f32 g     = static_cast<f32>(snapshot.targetSvfG_Q14[slot]) * kQ14ToFloat;
        const bool useFilter = (g > 0.001f && g < 0.96f);
        const f32 k     = 1.41421356f; // Butterworth Q = 0.7071
        const f32 a1    = useFilter ? (1.0f / (1.0f + g * (g + k))) : 0.0f;
        const f32 a2    = g * a1;
        const f32 a3    = g * a2;
        f32 ic1eq       = m_voices->svfIc1eq[slot];
        f32 ic2eq       = m_voices->svfIc2eq[slot];

        const i16* pcm    = m_voices->pcmPtr[slot];
        const u32  frames = m_voices->frameCount[slot];
        const u32  loopSt = m_voices->loopStart[slot];
        const bool loops  = (m_voices->loopMask & bit) != 0ULL;
        const i16* guard  = &m_voices->loopGuardSamples[slot * 4u];

        f32* busL = &m_busScratchL[static_cast<usize>(busIdx) * kMixBlockFrames];
        f32* busR = &m_busScratchR[static_cast<usize>(busIdx) * kMixBlockFrames];

        u64 phase  = m_voices->phaseQ32[slot];
        bool ended = false;

        for (u32 i = 0; i < kMixBlockFrames; ++i) {
            u32 idx = static_cast<u32>(phase >> 32u);
            if (idx >= frames) {
                if (loops && loopSt < frames) {
                    const u32 loopLen = frames - loopSt;
                    idx               = loopSt + ((idx - loopSt) % loopLen);
                    phase             = (static_cast<u64>(idx) << 32u) | (phase & 0xFFFFFFFFULL);
                } else {
                    ended = true;
                    break;
                }
            }

            const f32 frac = static_cast<f32>(phase & 0xFFFFFFFFULL) * kFracQ32ToFloat;
            f32 sample     = 0.0f;

            // Fast branchless interior span when 4-tap window lies inside valid PCM frames.
            if (idx >= 1u && idx + 2u < frames) {
                sample = SampleHermite4(
                    static_cast<f32>(pcm[idx - 1u]),
                    static_cast<f32>(pcm[idx]),
                    static_cast<f32>(pcm[idx + 1u]),
                    static_cast<f32>(pcm[idx + 2u]),
                    frac) * kPcm16ToFloat;
            } else {
                const i16 s0 = (idx > 0u) ? pcm[idx - 1u] : pcm[0];
                const i16 s1 = pcm[idx];
                const i16 s2 = (idx + 1u < frames) ? pcm[idx + 1u] : guard[2];
                const i16 s3 = (idx + 2u < frames) ? pcm[idx + 2u] : guard[3];
                sample = SampleHermite4(
                    static_cast<f32>(s0),
                    static_cast<f32>(s1),
                    static_cast<f32>(s2),
                    static_cast<f32>(s3),
                    frac) * kPcm16ToFloat;
            }

            if (useFilter) {
                // Cytomic Linear Trap-Integrated SVF low-pass stage with anti-denormal offset.
                const f32 v0 = sample + kAntiDenormalDc;
                const f32 v3 = v0 - ic2eq;
                const f32 v1 = a1 * ic1eq + a2 * v3;
                const f32 v2 = ic2eq + a2 * ic1eq + a3 * v3;
                ic1eq        = 2.0f * v1 - ic1eq;
                ic2eq        = 2.0f * v2 - ic2eq;
                sample       = v2;
            }

            busL[i] += sample * gainL;
            busR[i] += sample * gainR;
            gainL   += stepL;
            gainR   += stepR;

            if (!(isFrozen && pauseHit)) {
                phase += stepQ32;
            }
        }

        m_voices->phaseQ32[slot] = phase;
        m_voices->curGainL[slot] = gainL;
        m_voices->curGainR[slot] = gainR;
        m_voices->svfIc1eq[slot] = ic1eq;
        m_voices->svfIc2eq[slot] = ic2eq;

        if (ended) {
            m_voices->activeMask &= ~bit;
        } else if ((m_voices->stoppingMask & bit) != 0ULL) {
            if (m_voices->stopBlocksRemaining[slot] <= 1u) {
                m_voices->activeMask &= ~bit;
                m_voices->stoppingMask &= ~bit;
            } else {
                --m_voices->stopBlocksRemaining[slot];
            }
        }
    }
}

void AudioMixer::MixStealPoolPlanar() noexcept {
    u16 mask = m_stealState->activeMask;
    while (mask != 0u) {
        const u32 stealIdx = static_cast<u32>(std::countr_zero(mask));
        mask &= static_cast<u16>(mask - 1u);

        const u8   busIdx = m_stealState->busIdx[stealIdx];
        const f32  gL     = m_stealState->gainL[stealIdx];
        const f32  gR     = m_stealState->gainR[stealIdx];
        const f32* src    = &m_stealState->samples[stealIdx * kMixBlockFrames];
        f32*       busL   = &m_busScratchL[static_cast<usize>(busIdx) * kMixBlockFrames];
        f32*       busR   = &m_busScratchR[static_cast<usize>(busIdx) * kMixBlockFrames];

        for (u32 i = 0; i < kMixBlockFrames; ++i) {
            busL[i] += src[i] * gL;
            busR[i] += src[i] * gR;
        }
    }
    m_stealState->activeMask = 0u;
}

void AudioMixer::MixMusicDecksPlanar(const AudioFrameSnapshot& snapshot) noexcept {
    f32* musicL = &m_busScratchL[static_cast<usize>(AudioBusId::Music) * kMixBlockFrames];
    f32* musicR = &m_busScratchR[static_cast<usize>(AudioBusId::Music) * kMixBlockFrames];

    for (u32 d = 0; d < 2u; ++d) {
        MusicDeckRuntime& deck = m_decks[d];
        if (!deck.active || deck.frameCount == 0u) {
            continue;
        }

        const f32 deckGain = (d == 0u)
                                 ? std::cos(m_deckCrossfadeAlpha * 1.57079632f)
                                 : std::sin(m_deckCrossfadeAlpha * 1.57079632f);

        for (u8 s = 0; s < deck.stemCount; ++s) {
            const i16* pcm = deck.stemPcm[s];
            if (!pcm) {
                continue;
            }
            const f32 targetW = static_cast<f32>(snapshot.stemWeightsQ14[s]) * kQ14ToFloat * deckGain;
            f32       curW    = deck.curStemWeight[s];
            const f32 stepW   = (targetW - curW) * kInvBlockFrames;

            u32 cursor = deck.cursorFrame;
            for (u32 i = 0; i < kMixBlockFrames; ++i) {
                if (cursor >= deck.loopEndFrame) {
                    cursor = deck.loopStartFrame < deck.loopEndFrame ? deck.loopStartFrame : 0u;
                }
                const f32 sample = static_cast<f32>(pcm[cursor]) * kPcm16ToFloat * curW;
                musicL[i] += sample;
                musicR[i] += sample;
                curW      += stepW;
                ++cursor;
            }
            deck.curStemWeight[s] = curW;
        }

        deck.cursorFrame += kMixBlockFrames;
        if (deck.loopEndFrame > deck.loopStartFrame && deck.cursorFrame >= deck.loopEndFrame) {
            const u32 loopLen = deck.loopEndFrame - deck.loopStartFrame;
            deck.cursorFrame  = deck.loopStartFrame + ((deck.cursorFrame - deck.loopStartFrame) % loopLen);
        }
    }

    if (m_decks[1].active && m_deckCrossfadeStep > 0.0f) {
        m_deckCrossfadeAlpha += m_deckCrossfadeStep;
        if (m_deckCrossfadeAlpha >= 1.0f) {
            m_decks[0]           = m_decks[1];
            m_decks[1]           = MusicDeckRuntime{};
            m_deckCrossfadeAlpha = 0.0f;
            m_deckCrossfadeStep  = 0.0f;
        }
    }
}

void AudioMixer::ProcessBusesAndMaster(
    const AudioFrameSnapshot& snapshot,
    std::span<f32, kMixBlockFrames * 2> outStereo) noexcept {
    f32* masterL = &m_busScratchL[0];
    f32* masterR = &m_busScratchR[0];

    // 1. Measure combat peak energy on SfxPlayer (2) and SfxCombat (3) for sidechain ducking.
    f32 combatPeak = 0.0f;
    for (u32 b : {2u, 3u}) {
        const f32* bL = &m_busScratchL[b * kMixBlockFrames];
        const f32* bR = &m_busScratchR[b * kMixBlockFrames];
        for (u32 i = 0; i < kMixBlockFrames; ++i) {
            combatPeak = std::max(combatPeak, std::max(std::abs(bL[i]), std::abs(bR[i])));
        }
    }
    const f32 envTarget = combatPeak > 0.35f ? (combatPeak - 0.35f) : 0.0f;
    const f32 envRate   = envTarget > m_busDsp->sidechainEnvelope ? 0.35f : 0.03f;
    m_busDsp->sidechainEnvelope += (envTarget - m_busDsp->sidechainEnvelope) * envRate;
    const f32 duckGain = std::clamp(1.0f - (m_busDsp->sidechainEnvelope * 0.65f), 0.55f, 1.0f);

    // 2. Process Sub-Buses 1..7 and sum into Master Bus 0.
    for (usize b = 1; b < kAudioBusCount; ++b) {
        f32* bL = &m_busScratchL[b * kMixBlockFrames];
        f32* bR = &m_busScratchR[b * kMixBlockFrames];

        f32 targetGain = static_cast<f32>(snapshot.busTargetGainQ14[b]) * kQ14ToFloat;
        if (b == static_cast<usize>(AudioBusId::Music) || b == static_cast<usize>(AudioBusId::Ambience)) {
            targetGain *= duckGain;
        }
        f32 curGain       = m_busDsp->curGain[b];
        const f32 gainStp = (targetGain - curGain) * kInvBlockFrames;

        const FilterMode mode = static_cast<FilterMode>((snapshot.busFilterModesBits >> (b * 2u)) & 0x03u);
        const f32 g           = static_cast<f32>(snapshot.busSvfG_Q14[b]) * kQ14ToFloat;
        const bool applySvf   = (mode != FilterMode::Bypass && g > 0.001f && g < 0.98f);
        const f32 k           = 1.41421356f;
        const f32 a1          = applySvf ? (1.0f / (1.0f + g * (g + k))) : 0.0f;
        const f32 a2          = g * a1;
        const f32 a3          = g * a2;

        f32 ic1L = m_busDsp->svfIc1eqL[b];
        f32 ic2L = m_busDsp->svfIc2eqL[b];
        f32 ic1R = m_busDsp->svfIc1eqR[b];
        f32 ic2R = m_busDsp->svfIc2eqR[b];

        for (u32 i = 0; i < kMixBlockFrames; ++i) {
            f32 sL = bL[i];
            f32 sR = bR[i];
            if (applySvf) {
                const f32 v3L = (sL + kAntiDenormalDc) - ic2L;
                const f32 v1L = a1 * ic1L + a2 * v3L;
                const f32 v2L = ic2L + a2 * ic1L + a3 * v3L;
                ic1L          = 2.0f * v1L - ic1L;
                ic2L          = 2.0f * v2L - ic2L;

                const f32 v3R = (sR + kAntiDenormalDc) - ic2R;
                const f32 v1R = a1 * ic1R + a2 * v3R;
                const f32 v2R = ic2R + a2 * ic1R + a3 * v3R;
                ic1R          = 2.0f * v1R - ic1R;
                ic2R          = 2.0f * v2R - ic2R;

                if (mode == FilterMode::LowPass) {
                    sL = v2L;
                    sR = v2R;
                } else if (mode == FilterMode::BandPass) {
                    sL = v1L;
                    sR = v1R;
                } else {
                    sL = sL - k * v1L - v2L;
                    sR = sR - k * v1R - v2R;
                }
            }
            masterL[i] += sL * curGain;
            masterR[i] += sR * curGain;
            curGain    += gainStp;
        }

        m_busDsp->curGain[b]   = curGain;
        m_busDsp->svfIc1eqL[b] = ic1L;
        m_busDsp->svfIc2eqL[b] = ic2L;
        m_busDsp->svfIc1eqR[b] = ic1R;
        m_busDsp->svfIc2eqR[b] = ic2R;
    }

    // 3. Master bus gain ramp, focus mute fade, Padé soft-clipper, and final stereo interleave.
    const f32 masterTarget = snapshot.masterMuteFlag != 0u
                                 ? 0.0f
                                 : (static_cast<f32>(snapshot.busTargetGainQ14[0]) * kQ14ToFloat);
    f32 masterGain         = m_busDsp->curGain[0];
    const f32 masterStep   = (masterTarget - masterGain) * kInvBlockFrames;

    f32 peakL = 0.0f;
    f32 peakR = 0.0f;

    for (u32 i = 0; i < kMixBlockFrames; ++i) {
        const f32 outL         = SoftClipPade(masterL[i] * masterGain);
        const f32 outR         = SoftClipPade(masterR[i] * masterGain);
        outStereo[i * 2u]      = outL;
        outStereo[i * 2u + 1u] = outR;
        peakL                  = std::max(peakL, std::abs(outL));
        peakR                  = std::max(peakR, std::abs(outR));
        masterGain            += masterStep;
    }

    m_busDsp->curGain[0]   = masterGain;
    m_telemetry.masterPeakL = peakL;
    m_telemetry.masterPeakR = peakR;
}

void AudioMixer::MixBlock(
    const AudioFrameSnapshot& snapshot,
    std::span<f32, kMixBlockFrames * 2> outInterleavedStereo) noexcept {
    if (!m_voices) {
        std::memset(outInterleavedStereo.data(), 0, outInterleavedStereo.size_bytes());
        return;
    }

    const usize totalScratchBytes = sizeof(f32) * kAudioBusCount * kMixBlockFrames;
    std::memset(m_busScratchL, 0, totalScratchBytes);
    std::memset(m_busScratchR, 0, totalScratchBytes);

    MixActiveVoicesPlanar(snapshot);
    MixStealPoolPlanar();
    MixMusicDecksPlanar(snapshot);
    ProcessBusesAndMaster(snapshot, outInterleavedStereo);

    // Publish active voice mask and OR-reduced AssetBank ScopeId pin mask for wait-free retirement.
    u32 scopeMask = 0u;
    u64 vMask     = m_voices->activeMask;
    while (vMask != 0ULL) {
        const u32 slot = static_cast<u32>(std::countr_zero(vMask));
        vMask &= (vMask - 1ULL);
        scopeMask |= (1u << (m_voices->scopeId[slot] & 0x1Fu));
    }

    m_telemetry.activeVoiceMask    = m_voices->activeMask;
    m_telemetry.activeScopePinMask = scopeMask;
    m_sampleCounter += kMixBlockFrames;
}

u64 AudioMixer::ComputeAuditHash() const noexcept {
    if (!m_voices) {
        return 0ULL;
    }
    u64 h = MixSplitMix64(m_voices->activeMask ^ m_sampleCounter);
    u64 m = m_voices->activeMask;
    while (m != 0ULL) {
        const u32 slot = static_cast<u32>(std::countr_zero(m));
        m &= (m - 1ULL);
        h ^= MixSplitMix64(m_voices->phaseQ32[slot] ^ (static_cast<u64>(m_voices->generation[slot]) << 16u));
    }
    h ^= MixSplitMix64(static_cast<u64>(m_decks[0].cursorFrame) | (static_cast<u64>(m_decks[1].cursorFrame) << 32u));
    return h;
}

} // namespace en::audio