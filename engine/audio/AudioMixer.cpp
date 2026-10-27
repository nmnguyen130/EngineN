#include "audio/AudioMixer.h"

#include <cstring>

namespace en {

namespace {

inline constexpr u32 kMinSampleRate = 8000U;
inline constexpr u8 kSidechainTriggerBusMask =
    static_cast<u8>((1U << static_cast<u8>(AudioBusId::SfxCombat)) |
                    (1U << static_cast<u8>(AudioBusId::Voice)));
inline constexpr u8 kSidechainDuckedBusMask =
    static_cast<u8>((1U << static_cast<u8>(AudioBusId::Music)) |
                    (1U << static_cast<u8>(AudioBusId::Ambience)));

inline void ComputeCytomicSvfCoeffs(f32 cutoffHz, f32& outA1, f32& outA2, f32& outA3) noexcept {
    const f32 clampedHz = std::clamp(cutoffHz, kMinCutoffHz, kMaxCutoffHz);
    const f32 w = kPi * (clampedHz * kInvAudioSampleRate);
    const f32 w2 = w * w;
    const f32 g = w * (1.0f + w2 * (0.3333333f + 0.1333333f * w2));
    outA1 = 1.0f / (1.0f + g * (g + kSvfButterworthDamping));
    outA2 = g * outA1;
    outA3 = g * outA2;
}

[[nodiscard]] constexpr u64 ComputeBaseStep32_32(u32 sampleRate) noexcept {
    const u32 safeRate = std::max<u32>(kMinSampleRate, sampleRate);
    return (safeRate == kAudioSampleRate)
        ? kPhaseOne
        : ((static_cast<u64>(safeRate) << 32U) / kAudioSampleRate);
}

inline void SampleHermiteStereo(
    const f32* pcm,
    u32 frameCount,
    u32 loopStartFrame,
    u8 channels,
    i64 baseIdx,
    f32 frac,
    bool looping,
    f32& outL,
    f32& outR) noexcept {
    const i64 end = static_cast<i64>(frameCount);
    // 1:1 integer-phase fast-path: skip 4-point Hermite when fractional phase is zero.
    if (frac <= kSilenceThreshold && baseIdx >= 0 && baseIdx < end) {
        const usize b = static_cast<usize>(baseIdx) * static_cast<usize>(channels);
        outL = pcm[b];
        outR = (channels >= 2U) ? pcm[b + 1U] : outL;
        return;
    }

    if (baseIdx >= 1 && baseIdx + 2 < end) {
        if (channels == 1U) {
            const f32 s = AudioMixer::InterpolateHermite4(
                pcm[baseIdx - 1], pcm[baseIdx], pcm[baseIdx + 1], pcm[baseIdx + 2], frac);
            outL = s;
            outR = s;
            return;
        }
        const usize b = static_cast<usize>(baseIdx - 1) * 2U;
        outL = AudioMixer::InterpolateHermite4(pcm[b], pcm[b + 2U], pcm[b + 4U], pcm[b + 6U], frac);
        outR = AudioMixer::InterpolateHermite4(pcm[b + 1U], pcm[b + 3U], pcm[b + 5U], pcm[b + 7U], frac);
        return;
    }

    const auto sampleAt = [&](i64 idx, f32& l, f32& r) noexcept {
        if (!looping) {
            if (idx >= end) {
                l = 0.0f;
                r = 0.0f;
                return;
            }
            idx = std::clamp<i64>(idx, 0, std::max<i64>(0, end - 1));
        } else if (end > 1) {
            const i64 start = (static_cast<i64>(loopStartFrame) < end - 1) ? static_cast<i64>(loopStartFrame) : 0;
            idx = (idx < start) ? std::max<i64>(0, idx) : (start + ((idx - start) % (end - start)));
        } else {
            idx = 0;
        }
        const usize base = static_cast<usize>(idx) * static_cast<usize>(channels);
        l = pcm[base];
        r = (channels >= 2U) ? pcm[base + 1U] : l;
    };

    f32 l0, r0, l1, r1, l2, r2, l3, r3;
    sampleAt(baseIdx - 1, l0, r0);
    sampleAt(baseIdx,     l1, r1);
    sampleAt(baseIdx + 1, l2, r2);
    sampleAt(baseIdx + 2, l3, r3);
    outL = AudioMixer::InterpolateHermite4(l0, l1, l2, l3, frac);
    outR = (channels >= 2U) ? AudioMixer::InterpolateHermite4(r0, r1, r2, r3, frac) : outL;
}

[[nodiscard]] inline u64 WrapLoopPhase32_32(
    u64 phase,
    u32 frameCount,
    u32 loopStartFrame) noexcept {
    if (frameCount == 0U) return 0ULL;
    const u64 endPhase = static_cast<u64>(frameCount) << 32U;
    if (phase < endPhase) return phase;
    const u32 safeStart = (loopStartFrame + 1U < frameCount) ? loopStartFrame : 0U;
    const u64 startPhase = static_cast<u64>(safeStart) << 32U;
    const u64 spanPhase = std::max<u64>(kPhaseOne, endPhase - startPhase);
    return startPhase + ((phase - startPhase) % spanPhase);
}

struct MusicDeckState {
    const f32* stemsPcm[kMaxMusicStems]{};
    u64 stemPhase32_32[kMaxMusicStems]{};
    u64 stemStep32_32[kMaxMusicStems]{};
    u32 stemFrames[kMaxMusicStems]{};
    f32 stemGain[kMaxMusicStems]{};
    u8 stemChannels[kMaxMusicStems]{};
    u8 stemScope[kMaxMusicStems]{};
    f32 deckFadeGain{0.0f};
    f32 deckFadeStep{0.0f};
    u32 loopStartFrame{0U};
    u8 activeMask{0U};
    bool loop{true};
};

inline void MixMusicDeck(
    MusicDeckState& deck,
    const f32* targetWeights,
    bool fadeOut,
    f32* musL,
    f32* musR) noexcept {
    const f32 deckStartFade = deck.deckFadeGain;
    const f32 deckEndFade = fadeOut
        ? std::max(0.0f, deckStartFade - deck.deckFadeStep)
        : std::min(1.0f, deckStartFade + deck.deckFadeStep);
    deck.deckFadeGain = deckEndFade;

    for (u8 stem = 0U; stem < kMaxMusicStems; ++stem) {
        if ((deck.activeMask & (1U << stem)) == 0U) continue;
        const f32* pcm = deck.stemsPcm[stem];
        const u32 frames = deck.stemFrames[stem];
        if (!pcm || frames == 0U) continue;

        const f32 rawTargetG = targetWeights ? targetWeights[stem] : deck.stemGain[stem];
        f32 curG = deck.stemGain[stem] * deckStartFade;
        const f32 endG = rawTargetG * deckEndFade;
        deck.stemGain[stem] = rawTargetG;

        const u8 ch = deck.stemChannels[stem];
        u64 phase = deck.stemPhase32_32[stem];
        const u64 step = deck.stemStep32_32[stem];

        if (curG <= kSilenceThreshold && endG <= kSilenceThreshold) {
            phase += step * kMixBlockFrames;
            deck.stemPhase32_32[stem] = deck.loop ? WrapLoopPhase32_32(phase, frames, deck.loopStartFrame) : phase;
            continue;
        }

        const f32 stepG = (endG - curG) * kInvMixBlockFrames;
        const u32 baseFrame = static_cast<u32>(phase >> 32U);

        // 1:1 contiguous block fast-path for 48 kHz music stems.
        if (step == kPhaseOne && (phase & kPhaseFracMask) == 0ULL && baseFrame + kMixBlockFrames <= frames) {
            if (ch == 1U) {
                const f32* src = pcm + baseFrame;
                for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                    curG += stepG;
                    const f32 out = src[s] * curG;
                    musL[s] += out;
                    musR[s] += out;
                }
            } else {
                const f32* src = pcm + static_cast<usize>(baseFrame) * ch;
                for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                    curG += stepG;
                    musL[s] += src[s * ch] * curG;
                    musR[s] += src[s * ch + 1U] * curG;
                }
            }
            phase += kPhaseOne * kMixBlockFrames;
        } else {
            for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                const i64 baseIdx = static_cast<i64>(phase >> 32U);
                const f32 frac = static_cast<f32>(phase & kPhaseFracMask) * kInvPhaseOne;
                f32 sL = 0.0f, sR = 0.0f;
                SampleHermiteStereo(pcm, frames, deck.loopStartFrame, ch, baseIdx, frac, deck.loop, sL, sR);
                curG += stepG;
                musL[s] += sL * curG;
                musR[s] += sR * curG;
                phase += step;
            }
        }
        deck.stemPhase32_32[stem] = deck.loop ? WrapLoopPhase32_32(phase, frames, deck.loopStartFrame) : phase;
    }

    if (fadeOut && deckEndFade <= kSilenceThreshold) {
        deck.activeMask = 0U;
        deck.deckFadeGain = 0.0f;
    }
}

} // namespace

struct alignas(64) AudioMixer::MixerArenaState {
    const f32* pcmPtr[kMaxAudioVoices]{};
    u64 phase32_32[kMaxAudioVoices]{};
    u64 baseRateStep32_32[kMaxAudioVoices]{};
    u32 frameCount[kMaxAudioVoices]{};
    u32 loopStartFrame[kMaxAudioVoices]{};
    f32 currentGainL[kMaxAudioVoices]{};
    f32 currentGainR[kMaxAudioVoices]{};
    f32 currentPitch[kMaxAudioVoices]{};
    f32 svfIc1L[kMaxAudioVoices]{};
    f32 svfIc2L[kMaxAudioVoices]{};
    f32 svfIc1R[kMaxAudioVoices]{};
    f32 svfIc2R[kMaxAudioVoices]{};
    u16 generation[kMaxAudioVoices]{};
    u16 fadeRemainingBlocks[kMaxAudioVoices]{};
    u8 channels[kMaxAudioVoices]{};
    u8 busIndex[kMaxAudioVoices]{};
    VoiceFlags flags[kMaxAudioVoices]{};
    u8 assetScope[kMaxAudioVoices]{};

    MusicDeckState activeDeck{};
    MusicDeckState prevDeck{};

    f32 busIc1L[kAudioBusCount]{};
    f32 busIc2L[kAudioBusCount]{};
    f32 busIc1R[kAudioBusCount]{};
    f32 busIc2R[kAudioBusCount]{};
    f32 busCurrentGain[kAudioBusCount]{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    f32 sidechainEnvelope{0.0f};
    f32 masterMuteGain{1.0f};

    alignas(64) f32 busPlaneL[kAudioBusCount][kMixBlockFrames]{};
    alignas(64) f32 busPlaneR[kAudioBusCount][kMixBlockFrames]{};
};

Result<void, AudioErrc> AudioMixer::Init(std::span<u8> arenaSlice) noexcept {
    if (arenaSlice.size() < sizeof(MixerArenaState) ||
        (reinterpret_cast<usize>(arenaSlice.data()) & 63U) != 0U) {
        return Err(AudioErrc::OutOfMemory);
    }
    m_state = reinterpret_cast<MixerArenaState*>(arenaSlice.data());
    Reset();
    return {};
}

void AudioMixer::Reset() noexcept {
    if (m_state) {
        *m_state = MixerArenaState{};
        m_state->activeDeck.deckFadeGain = 1.0f;
    }
    m_activeVoicesMask = 0ULL;
    m_virtualVoicesMask = 0ULL;
    m_telemetry = AudioTelemetrySnapshot{};
}

void AudioMixer::ApplyCommand(const AudioCommand& cmd) noexcept {
    if (!m_state) return;

    switch (cmd.type) {
        case AudioCmdType::SpawnVoice: {
            const u8 slot = cmd.voiceSlot & 63U;
            const u64 bit = (1ULL << slot);
            if ((m_activeVoicesMask & bit) != 0ULL) {
                ++m_telemetry.stolenVoiceCount;
            }
            if (!cmd.pcmData || cmd.frameCount == 0U) {
                m_activeVoicesMask &= ~bit;
                m_virtualVoicesMask &= ~bit;
                return;
            }

            m_state->pcmPtr[slot] = cmd.pcmData;
            m_state->phase32_32[slot] = 0ULL;
            m_state->baseRateStep32_32[slot] = ComputeBaseStep32_32(cmd.sampleRate);
            m_state->frameCount[slot] = cmd.frameCount;
            m_state->loopStartFrame[slot] = (cmd.loopStartFrame + 1U < cmd.frameCount) ? cmd.loopStartFrame : 0U;
            m_state->currentGainL[slot] = cmd.initialGainL;
            m_state->currentGainR[slot] = cmd.initialGainR;
            m_state->currentPitch[slot] = std::clamp(cmd.initialPitch, kMinVoicePitch, kMaxVoicePitch);
            m_state->svfIc1L[slot] = 0.0f;
            m_state->svfIc2L[slot] = 0.0f;
            m_state->svfIc1R[slot] = 0.0f;
            m_state->svfIc2R[slot] = 0.0f;
            m_state->generation[slot] = cmd.generation;
            m_state->fadeRemainingBlocks[slot] = 0U;
            m_state->channels[slot] = std::max<u8>(1U, cmd.channels);
            m_state->busIndex[slot] = static_cast<u8>(cmd.bus) & 7U;
            m_state->flags[slot] = cmd.flags;
            m_state->assetScope[slot] = cmd.assetScope;

            m_activeVoicesMask |= bit;
            if (HasFlag(cmd.flags, VoiceFlags::Looping) &&
                cmd.initialGainL <= kSilenceThreshold && cmd.initialGainR <= kSilenceThreshold) {
                m_virtualVoicesMask |= bit;
            } else {
                m_virtualVoicesMask &= ~bit;
            }
            m_telemetry.activeVoiceCount = static_cast<u16>(std::popcount(m_activeVoicesMask & ~m_virtualVoicesMask));
            m_telemetry.virtualVoiceCount = static_cast<u16>(std::popcount(m_virtualVoicesMask));
            break;
        }
        case AudioCmdType::StopVoice: {
            const u8 slot = cmd.voiceSlot & 63U;
            const u64 bit = (1ULL << slot);
            if ((m_activeVoicesMask & bit) == 0ULL) break;
            if (cmd.generation != 0U && m_state->generation[slot] != cmd.generation) break;

            if (cmd.fadeBlocks == 0U || (m_virtualVoicesMask & bit) != 0ULL) {
                m_activeVoicesMask &= ~bit;
                m_virtualVoicesMask &= ~bit;
                m_telemetry.activeVoiceCount = static_cast<u16>(std::popcount(m_activeVoicesMask & ~m_virtualVoicesMask));
                m_telemetry.virtualVoiceCount = static_cast<u16>(std::popcount(m_virtualVoicesMask));
            } else {
                m_state->fadeRemainingBlocks[slot] = cmd.fadeBlocks;
            }
            break;
        }
        case AudioCmdType::CommitMusicDeck: {
            if (m_state->activeDeck.activeMask != 0U && cmd.fadeBlocks > 1U) {
                m_state->prevDeck = m_state->activeDeck;
                m_state->prevDeck.deckFadeStep = 1.0f / static_cast<f32>(cmd.fadeBlocks);
            } else {
                m_state->prevDeck.activeMask = 0U;
            }
            m_state->activeDeck = MusicDeckState{};
            m_state->activeDeck.loopStartFrame = cmd.loopStartFrame;
            m_state->activeDeck.loop = HasFlag(cmd.flags, VoiceFlags::Looping);
            if (cmd.fadeBlocks > 1U) {
                m_state->activeDeck.deckFadeGain = 0.0f;
                m_state->activeDeck.deckFadeStep = 1.0f / static_cast<f32>(cmd.fadeBlocks);
            } else {
                m_state->activeDeck.deckFadeGain = 1.0f;
                m_state->activeDeck.deckFadeStep = 0.0f;
            }
            break;
        }
        case AudioCmdType::BindMusicStem: {
            const u8 stemIdx = cmd.voiceSlot & 3U;
            MusicDeckState& deck = m_state->activeDeck;
            if (cmd.pcmData && cmd.frameCount > 0U) {
                deck.stemsPcm[stemIdx] = cmd.pcmData;
                deck.stemFrames[stemIdx] = cmd.frameCount;
                deck.stemChannels[stemIdx] = std::max<u8>(1U, cmd.channels);
                deck.stemScope[stemIdx] = cmd.assetScope;
                deck.stemGain[stemIdx] = cmd.initialGainL;
                deck.stemPhase32_32[stemIdx] = 0ULL;
                deck.stemStep32_32[stemIdx] = ComputeBaseStep32_32(cmd.sampleRate);
                deck.activeMask |= static_cast<u8>(1U << stemIdx);
            }
            break;
        }
        case AudioCmdType::PanicReset:
            Reset();
            break;
    }
}

void AudioMixer::MixBlock(
    const AudioFrameSnapshot& snapshot,
    std::span<f32, kMixBlockFrames * 2U> outInterleavedStereo) noexcept {
    if (!m_state) {
        std::memset(outInterleavedStereo.data(), 0, outInterleavedStereo.size_bytes());
        return;
    }

    u8 dirtyBusMask = 0U;
    const auto ensureBus = [&](u8 busIdx) noexcept {
        const u8 bit = static_cast<u8>(1U << (busIdx & 7U));
        if ((dirtyBusMask & bit) == 0U) {
            std::memset(m_state->busPlaneL[busIdx], 0, sizeof(m_state->busPlaneL[busIdx]));
            std::memset(m_state->busPlaneR[busIdx], 0, sizeof(m_state->busPlaneR[busIdx]));
            dirtyBusMask |= bit;
        }
    };

    u64 voiceMask = m_activeVoicesMask & ~snapshot.pausedVoicesMask;
    const u64 activeOccludedMask = voiceMask & snapshot.occludedVoicesMask;
    f32 occA1 = 0.0f, occA2 = 0.0f, occA3 = 0.0f;
    if (activeOccludedMask != 0ULL && snapshot.occludedCutoffHz < kVoiceSvfBypassHz) {
        ComputeCytomicSvfCoeffs(snapshot.occludedCutoffHz, occA1, occA2, occA3);
    }

    while (voiceMask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(voiceMask));
        voiceMask &= (voiceMask - 1ULL);
        const u64 slotBit = (1ULL << slot);

        const f32* pcm = m_state->pcmPtr[slot];
        const u32 frames = m_state->frameCount[slot];
        if (!pcm || frames == 0U) {
            m_activeVoicesMask &= ~slotBit;
            m_virtualVoicesMask &= ~slotBit;
            continue;
        }

        f32 curGainL = m_state->currentGainL[slot];
        f32 curGainR = m_state->currentGainR[slot];
        f32 targetGainL = snapshot.targetGainL[slot];
        f32 targetGainR = snapshot.targetGainR[slot];

        bool fadeFinished = false;
        if (m_state->fadeRemainingBlocks[slot] > 0U) {
            const u16 rem = m_state->fadeRemainingBlocks[slot]--;
            const f32 fadeRatio = static_cast<f32>(rem - 1U) / static_cast<f32>(rem);
            targetGainL *= fadeRatio;
            targetGainR *= fadeRatio;
            fadeFinished = (rem == 1U);
        }

        const f32 pitch = (snapshot.targetPitch[slot] > 0.0f) ? snapshot.targetPitch[slot] : m_state->currentPitch[slot];
        m_state->currentPitch[slot] = pitch;
        const u64 baseStep = m_state->baseRateStep32_32[slot];
        const u64 step = (pitch == 1.0f)
            ? baseStep
            : static_cast<u64>(static_cast<f32>(baseStep) * pitch);
        const bool looping = HasFlag(m_state->flags[slot], VoiceFlags::Looping);
        const u32 loopStart = m_state->loopStartFrame[slot];

        if (looping && !fadeFinished &&
            curGainL <= kSilenceThreshold && curGainR <= kSilenceThreshold &&
            targetGainL <= kSilenceThreshold && targetGainR <= kSilenceThreshold) {
            m_virtualVoicesMask |= slotBit;
            m_state->phase32_32[slot] = WrapLoopPhase32_32(
                m_state->phase32_32[slot] + step * kMixBlockFrames, frames, loopStart);
            m_state->currentGainL[slot] = 0.0f;
            m_state->currentGainR[slot] = 0.0f;
            continue;
        }
        m_virtualVoicesMask &= ~slotBit;

        const u8 busIdx = m_state->busIndex[slot] & 7U;
        ensureBus(busIdx);
        f32* busL = m_state->busPlaneL[busIdx];
        f32* busR = m_state->busPlaneR[busIdx];

        const f32 stepGainL = (targetGainL - curGainL) * kInvMixBlockFrames;
        const f32 stepGainR = (targetGainR - curGainR) * kInvMixBlockFrames;

        const bool useSvf = ((activeOccludedMask & slotBit) != 0ULL) && (snapshot.occludedCutoffHz < kVoiceSvfBypassHz);
        f32 ic1L = m_state->svfIc1L[slot], ic2L = m_state->svfIc2L[slot];
        f32 ic1R = m_state->svfIc1R[slot], ic2R = m_state->svfIc2R[slot];

        u64 phase = m_state->phase32_32[slot];
        const u8 ch = m_state->channels[slot];
        bool finished = fadeFinished;

        const auto accumSample = [&](u32 s, f32 sL, f32 sR) noexcept {
            if (useSvf) {
                sL = StepCytomicSvfLowPass(sL, ic1L, ic2L, occA1, occA2, occA3);
                sR = (ch >= 2U) ? StepCytomicSvfLowPass(sR, ic1R, ic2R, occA1, occA2, occA3) : sL;
            }
            curGainL += stepGainL;
            curGainR += stepGainR;
            busL[s] += sL * curGainL;
            busR[s] += sR * curGainR;
        };

        const u32 baseFrame = static_cast<u32>(phase >> 32U);
        // 1:1 contiguous block fast-path: bypass 64-bit phase math and 4-point Hermite interpolation.
        if (step == kPhaseOne && (phase & kPhaseFracMask) == 0ULL && baseFrame + kMixBlockFrames <= frames) {
            if (!useSvf) {
                if (ch == 1U) {
                    const f32* src = pcm + baseFrame;
                    for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                        curGainL += stepGainL;
                        curGainR += stepGainR;
                        const f32 v = src[s];
                        busL[s] += v * curGainL;
                        busR[s] += v * curGainR;
                    }
                } else {
                    const f32* src = pcm + static_cast<usize>(baseFrame) * ch;
                    for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                        curGainL += stepGainL;
                        curGainR += stepGainR;
                        busL[s] += src[s * ch] * curGainL;
                        busR[s] += src[s * ch + 1U] * curGainR;
                    }
                }
            } else if (ch == 1U) {
                const f32* src = pcm + baseFrame;
                for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                    const f32 v = StepCytomicSvfLowPass(src[s], ic1L, ic2L, occA1, occA2, occA3);
                    curGainL += stepGainL;
                    curGainR += stepGainR;
                    busL[s] += v * curGainL;
                    busR[s] += v * curGainR;
                }
            } else {
                const f32* src = pcm + static_cast<usize>(baseFrame) * ch;
                for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                    const f32 sL = StepCytomicSvfLowPass(src[s * ch], ic1L, ic2L, occA1, occA2, occA3);
                    const f32 sR = StepCytomicSvfLowPass(src[s * ch + 1U], ic1R, ic2R, occA1, occA2, occA3);
                    curGainL += stepGainL;
                    curGainR += stepGainR;
                    busL[s] += sL * curGainL;
                    busR[s] += sR * curGainR;
                }
            }
            phase += kPhaseOne * kMixBlockFrames;
            if (!looping && baseFrame + kMixBlockFrames >= frames) {
                finished = true;
            }
        } else {
            for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                const i64 baseIdx = static_cast<i64>(phase >> 32U);
                if (!looping && baseIdx >= static_cast<i64>(frames)) {
                    finished = true;
                    break;
                }
                const f32 frac = static_cast<f32>(phase & kPhaseFracMask) * kInvPhaseOne;
                f32 sL = 0.0f, sR = 0.0f;
                SampleHermiteStereo(pcm, frames, loopStart, ch, baseIdx, frac, looping, sL, sR);
                accumSample(s, sL, sR);
                phase += step;
            }
        }

        if (finished) {
            m_activeVoicesMask &= ~slotBit;
            m_virtualVoicesMask &= ~slotBit;
        } else {
            m_state->phase32_32[slot] = looping ? WrapLoopPhase32_32(phase, frames, loopStart) : phase;
            m_state->currentGainL[slot] = targetGainL;
            m_state->currentGainR[slot] = targetGainR;
            if (useSvf) {
                m_state->svfIc1L[slot] = ic1L;
                m_state->svfIc2L[slot] = ic2L;
                m_state->svfIc1R[slot] = ic1R;
                m_state->svfIc2R[slot] = ic2R;
            }
        }
    }

    constexpr u8 kMusicBus = static_cast<u8>(AudioBusId::Music);
    if (m_state->prevDeck.activeMask != 0U) {
        ensureBus(kMusicBus);
        MixMusicDeck(m_state->prevDeck, nullptr, true, m_state->busPlaneL[kMusicBus], m_state->busPlaneR[kMusicBus]);
    }
    if (m_state->activeDeck.activeMask != 0U) {
        ensureBus(kMusicBus);
        MixMusicDeck(
            m_state->activeDeck,
            snapshot.musicStemWeights.data(),
            false,
            m_state->busPlaneL[kMusicBus],
            m_state->busPlaneR[kMusicBus]);
    }

    // Fast silent-block early-exit when no voices or music stems produced signal.
    if (dirtyBusMask == 0U) {
        std::memset(outInterleavedStereo.data(), 0, outInterleavedStereo.size_bytes());
        m_state->sidechainEnvelope *= 0.95f;
        m_state->busCurrentGain[0] = snapshot.busGain[0];
        m_state->masterMuteGain = snapshot.masterMuted ? 0.0f : 1.0f;
        m_telemetry.mixedBlocksCount += 1ULL;
        m_telemetry.masterPeakL = 0.0f;
        m_telemetry.masterPeakR = 0.0f;
        m_telemetry.sidechainEnvelope = m_state->sidechainEnvelope;
        m_telemetry.activeVoiceCount = static_cast<u16>(std::popcount(m_activeVoicesMask & ~m_virtualVoicesMask));
        m_telemetry.virtualVoiceCount = static_cast<u16>(std::popcount(m_virtualVoicesMask));
        m_telemetry.activeMusicStems = static_cast<u16>(std::popcount(m_state->activeDeck.activeMask));
        return;
    }

    f32 sidechainPeak = 0.0f;
    if ((dirtyBusMask & kSidechainTriggerBusMask) != 0U) {
        for (u8 trigBus : {static_cast<u8>(AudioBusId::SfxCombat), static_cast<u8>(AudioBusId::Voice)}) {
            if ((dirtyBusMask & (1U << trigBus)) == 0U) continue;
            const f32* bL = m_state->busPlaneL[trigBus];
            const f32* bR = m_state->busPlaneR[trigBus];
            for (u32 s = 0U; s < kMixBlockFrames; s += 4U) {
                sidechainPeak = std::max(sidechainPeak, std::max(std::abs(bL[s]), std::abs(bR[s])));
            }
        }
    }
    const f32 envTarget = std::clamp(sidechainPeak, 0.0f, 1.0f);
    const f32 slewRate = (envTarget > m_state->sidechainEnvelope) ? 0.35f : 0.05f;
    m_state->sidechainEnvelope += (envTarget - m_state->sidechainEnvelope) * slewRate;
    const f32 duckMultiplier = std::clamp(1.0f - m_state->sidechainEnvelope * snapshot.sidechainDuckStrength, 0.15f, 1.0f);

    ensureBus(0U);
    f32* masterL = m_state->busPlaneL[0];
    f32* masterR = m_state->busPlaneR[0];

    for (u8 b = 1U; b < kAudioBusCount; ++b) {
        if ((dirtyBusMask & (1U << b)) == 0U) continue;
        const f32* bL = m_state->busPlaneL[b];
        const f32* bR = m_state->busPlaneR[b];

        f32 targetBusG = snapshot.busGain[b];
        if ((kSidechainDuckedBusMask & (1U << b)) != 0U) {
            targetBusG *= duckMultiplier;
        }
        f32 curBusG = m_state->busCurrentGain[b];
        const f32 stepBusG = (targetBusG - curBusG) * kInvMixBlockFrames;

        const FilterMode fMode = snapshot.busFilterMode[b];
        if (fMode == FilterMode::Bypass) {
            for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                curBusG += stepBusG;
                masterL[s] += bL[s] * curBusG;
                masterR[s] += bR[s] * curBusG;
            }
        } else {
            f32 a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            ComputeCytomicSvfCoeffs(snapshot.busCutoffHz[b], a1, a2, a3);
            f32 ic1L = m_state->busIc1L[b], ic2L = m_state->busIc2L[b];
            f32 ic1R = m_state->busIc1R[b], ic2R = m_state->busIc2R[b];

            if (fMode == FilterMode::LowPass) {
                for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                    curBusG += stepBusG;
                    masterL[s] += StepCytomicSvfLowPass(bL[s], ic1L, ic2L, a1, a2, a3) * curBusG;
                    masterR[s] += StepCytomicSvfLowPass(bR[s], ic1R, ic2R, a1, a2, a3) * curBusG;
                }
            } else {
                for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                    curBusG += stepBusG;
                    masterL[s] += StepCytomicSvf(bL[s], ic1L, ic2L, a1, a2, a3, fMode) * curBusG;
                    masterR[s] += StepCytomicSvf(bR[s], ic1R, ic2R, a1, a2, a3, fMode) * curBusG;
                }
            }
            m_state->busIc1L[b] = ic1L;
            m_state->busIc2L[b] = ic2L;
            m_state->busIc1R[b] = ic1R;
            m_state->busIc2R[b] = ic2R;
        }
        m_state->busCurrentGain[b] = targetBusG;
    }

    const f32 rawMasterTarget = snapshot.busGain[0];
    const f32 muteTarget = snapshot.masterMuted ? 0.0f : 1.0f;
    const f32 targetMasterG = rawMasterTarget * muteTarget;
    f32 curMasterG = m_state->busCurrentGain[0] * m_state->masterMuteGain;
    const f32 stepMasterG = (targetMasterG - curMasterG) * kInvMixBlockFrames;
    f32 peakL = 0.0f;
    f32 peakR = 0.0f;

    for (u32 s = 0U; s < kMixBlockFrames; ++s) {
        curMasterG += stepMasterG;
        const f32 outL = SoftClipTanhPade(masterL[s] * curMasterG);
        const f32 outR = SoftClipTanhPade(masterR[s] * curMasterG);
        outInterleavedStereo[s * 2U]      = outL;
        outInterleavedStereo[s * 2U + 1U] = outR;
        peakL = std::max(peakL, std::abs(outL));
        peakR = std::max(peakR, std::abs(outR));
    }
    m_state->busCurrentGain[0] = rawMasterTarget;
    m_state->masterMuteGain = muteTarget;

    m_telemetry.mixedBlocksCount += 1ULL;
    m_telemetry.masterPeakL = peakL;
    m_telemetry.masterPeakR = peakR;
    m_telemetry.sidechainEnvelope = m_state->sidechainEnvelope;
    m_telemetry.activeVoiceCount = static_cast<u16>(std::popcount(m_activeVoicesMask & ~m_virtualVoicesMask));
    m_telemetry.virtualVoiceCount = static_cast<u16>(std::popcount(m_virtualVoicesMask));
    m_telemetry.activeMusicStems = static_cast<u16>(std::popcount(m_state->activeDeck.activeMask));
}

u32 AudioMixer::ActiveAssetScopesMask() const noexcept {
    if (!m_state) return 0U;
    u32 mask = 0U;
    u64 vMask = m_activeVoicesMask;
    while (vMask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(vMask));
        vMask &= (vMask - 1ULL);
        mask |= (1U << (m_state->assetScope[slot] & 31U));
    }
    for (u8 st = 0U; st < kMaxMusicStems; ++st) {
        if ((m_state->activeDeck.activeMask & (1U << st)) != 0U) {
            mask |= (1U << (m_state->activeDeck.stemScope[st] & 31U));
        }
        if ((m_state->prevDeck.activeMask & (1U << st)) != 0U) {
            mask |= (1U << (m_state->prevDeck.stemScope[st] & 31U));
        }
    }
    return mask;
}

u64 AudioMixer::ComputeAuditHash() const noexcept {
    u64 h = kFnvOffsetBasis64;
    h = HashCombine64(h, m_activeVoicesMask);
    h = HashCombine64(h, m_virtualVoicesMask);
    h = HashCombine64(h, m_telemetry.mixedBlocksCount);
    h = HashCombine64(h, static_cast<u64>(m_telemetry.activeVoiceCount) |
                         (static_cast<u64>(m_telemetry.stolenVoiceCount) << 16U));
    if (m_state) {
        u64 vMask = m_activeVoicesMask;
        while (vMask != 0ULL) {
            const u8 s = static_cast<u8>(std::countr_zero(vMask));
            vMask &= (vMask - 1ULL);
            h = HashCombine64(h, m_state->phase32_32[s]);
            h = HashCombine64(h, static_cast<u64>(std::bit_cast<u32>(m_state->currentGainL[s])) |
                                 (static_cast<u64>(std::bit_cast<u32>(m_state->currentGainR[s])) << 32U));
        }
        for (u8 st = 0U; st < kMaxMusicStems; ++st) {
            if ((m_state->activeDeck.activeMask & (1U << st)) != 0U) {
                h = HashCombine64(h, m_state->activeDeck.stemPhase32_32[st]);
            }
        }
    }
    return h;
}

} // namespace en
