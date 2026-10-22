#include "audio/AudioMixer.h"

#include <cstring>

namespace en {

namespace {

inline constexpr u16 kReverbDirtyBit = (1U << 8U);
inline constexpr u32 kCombMaxLen = 1024U;
inline constexpr u32 kAllpassMaxLen = 256U;
inline constexpr u32 kCombLengthsL[4]{317U, 419U, 557U, 677U};
inline constexpr u32 kCombLengthsR[4]{337U, 443U, 577U, 701U};
inline constexpr u32 kAllpassLengthsL[2]{131U, 193U};
inline constexpr u32 kAllpassLengthsR[2]{149U, 211U};

inline void ComputeCytomicSvfCoeffs(f32 cutoffHz, f32& outA1, f32& outA2, f32& outA3) noexcept {
    const f32 clampedHz = std::clamp(cutoffHz, 40.0f, 20000.0f);
    const f32 w = kPi * (clampedHz * (1.0f / static_cast<f32>(kAudioSampleRate)));
    const f32 w2 = w * w;
    const f32 g = w * (1.0f + w2 * (0.3333333f + 0.1333333f * w2));
    outA1 = 1.0f / (1.0f + g * (g + 1.41421356f));
    outA2 = g * outA1;
    outA3 = g * outA2;
}

[[nodiscard]] inline i64 WrapFrameIndex(
    i64 frameIdx,
    u32 frameCount,
    u16 loopStartFrame,
    bool looping) noexcept {
    const i64 end = static_cast<i64>(frameCount);
    if (!looping) {
        return std::clamp<i64>(frameIdx, 0, end - 1);
    }
    const i64 start = std::min<i64>(static_cast<i64>(loopStartFrame), end - 1);
    const i64 span = std::max<i64>(1, end - start);
    if (frameIdx < start) {
        return std::max<i64>(0, frameIdx);
    }
    return start + ((frameIdx - start) % span);
}

inline void SampleFrameStereo(
    const f32* pcm,
    u32 frameCount,
    u16 loopStartFrame,
    u8 channels,
    i64 frameIdx,
    bool looping,
    f32& outL,
    f32& outR) noexcept {
    if (!looping && frameIdx >= static_cast<i64>(frameCount)) {
        outL = 0.0f;
        outR = 0.0f;
        return;
    }
    const usize idx = static_cast<usize>(WrapFrameIndex(frameIdx, frameCount, loopStartFrame, looping));
    if (channels >= 2U) {
        const usize base = idx * static_cast<usize>(channels);
        outL = pcm[base];
        outR = pcm[base + 1U];
    } else {
        outL = pcm[idx];
        outR = pcm[idx];
    }
}

inline void SampleHermiteStereo(
    const f32* pcm,
    u32 frameCount,
    u16 loopStartFrame,
    u8 channels,
    i64 baseIdx,
    f32 frac,
    bool looping,
    f32& outL,
    f32& outR) noexcept {
    if (channels == 1U && baseIdx >= 1 && baseIdx + 2 < static_cast<i64>(frameCount)) {
        const f32 s = AudioMixer::InterpolateHermite4(
            pcm[baseIdx - 1], pcm[baseIdx], pcm[baseIdx + 1], pcm[baseIdx + 2], frac);
        outL = s;
        outR = s;
        return;
    }
    if (channels == 2U && baseIdx >= 1 && baseIdx + 2 < static_cast<i64>(frameCount)) {
        const usize b0 = static_cast<usize>(baseIdx - 1) * 2U;
        outL = AudioMixer::InterpolateHermite4(pcm[b0], pcm[b0 + 2U], pcm[b0 + 4U], pcm[b0 + 6U], frac);
        outR = AudioMixer::InterpolateHermite4(pcm[b0 + 1U], pcm[b0 + 3U], pcm[b0 + 5U], pcm[b0 + 7U], frac);
        return;
    }
    f32 l0, r0, l1, r1, l2, r2, l3, r3;
    SampleFrameStereo(pcm, frameCount, loopStartFrame, channels, baseIdx - 1, looping, l0, r0);
    SampleFrameStereo(pcm, frameCount, loopStartFrame, channels, baseIdx,     looping, l1, r1);
    SampleFrameStereo(pcm, frameCount, loopStartFrame, channels, baseIdx + 1, looping, l2, r2);
    SampleFrameStereo(pcm, frameCount, loopStartFrame, channels, baseIdx + 2, looping, l3, r3);
    outL = AudioMixer::InterpolateHermite4(l0, l1, l2, l3, frac);
    outR = (channels >= 2U) ? AudioMixer::InterpolateHermite4(r0, r1, r2, r3, frac) : outL;
}

[[nodiscard]] inline u64 WrapLoopPhase32_32(
    u64 phase,
    u32 frameCount,
    u16 loopStartFrame) noexcept {
    if (frameCount == 0U) return 0ULL;
    const u64 endPhase = static_cast<u64>(frameCount) << 32U;
    if (phase < endPhase) return phase;
    const u32 safeStart = (loopStartFrame < frameCount) ? loopStartFrame : 0U;
    const u64 startPhase = static_cast<u64>(safeStart) << 32U;
    const u64 spanPhase = std::max<u64>(1ULL << 32U, endPhase - startPhase);
    return startPhase + ((phase - startPhase) % spanPhase);
}

} // namespace

struct alignas(64) AudioMixer::MixerArenaState {
    const f32* pcmPtr[kMaxAudioVoices]{};
    u64 phase32_32[kMaxAudioVoices]{};
    u64 baseRateStep32_32[kMaxAudioVoices]{};
    u32 frameCount[kMaxAudioVoices]{};
    f32 currentGainL[kMaxAudioVoices]{};
    f32 currentGainR[kMaxAudioVoices]{};
    f32 svfIc1L[kMaxAudioVoices]{};
    f32 svfIc2L[kMaxAudioVoices]{};
    f32 svfIc1R[kMaxAudioVoices]{};
    f32 svfIc2R[kMaxAudioVoices]{};
    u16 loopStartFrame[kMaxAudioVoices]{};
    u16 generation[kMaxAudioVoices]{};
    u16 fadeRemainingBlocks[kMaxAudioVoices]{};
    u16 currentPitch_q12[kMaxAudioVoices]{};
    u8 channels[kMaxAudioVoices]{};
    u8 busIndex[kMaxAudioVoices]{};
    VoiceFlags flags[kMaxAudioVoices]{};
    u8 assetScope[kMaxAudioVoices]{};

    const f32* musicStemsPcm[kMaxMusicStems]{};
    u32 musicStemFrames[kMaxMusicStems]{};
    f32 musicStemCurrentGain[kMaxMusicStems]{};
    u8 musicStemChannels[kMaxMusicStems]{};
    u8 musicStemScope[kMaxMusicStems]{};
    u64 musicMasterPhase32_32{0ULL};
    u64 musicMasterStep32_32{0ULL};
    f32 musicDeckFadeGain{1.0f};
    f32 musicDeckFadeStep{0.0f};
    u16 musicLoopStartFrame{0U};
    u8 musicActiveMask{0U};
    u8 musicLoop{1U};

    f32 busIc1L[kAudioBusCount]{};
    f32 busIc2L[kAudioBusCount]{};
    f32 busIc1R[kAudioBusCount]{};
    f32 busIc2R[kAudioBusCount]{};
    f32 busCurrentGain[kAudioBusCount]{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    f32 sidechainEnvelope{0.0f};

    // Stereo Schroeder / Freeverb auxiliary reverb state (4 comb + 2 allpass per channel)
    f32 combBufL[4][kCombMaxLen]{};
    f32 combBufR[4][kCombMaxLen]{};
    f32 combStoreL[4]{};
    f32 combStoreR[4]{};
    u16 combIdxL[4]{};
    u16 combIdxR[4]{};
    f32 allpassBufL[2][kAllpassMaxLen]{};
    f32 allpassBufR[2][kAllpassMaxLen]{};
    u16 allpassIdxL[2]{};
    u16 allpassIdxR[2]{};

    alignas(64) f32 busPlaneL[kAudioBusCount][kMixBlockFrames]{};
    alignas(64) f32 busPlaneR[kAudioBusCount][kMixBlockFrames]{};
    alignas(64) f32 reverbSendL[kMixBlockFrames]{};
    alignas(64) f32 reverbSendR[kMixBlockFrames]{};
};

Result<void, AudioErrc> AudioMixer::Init(std::span<u8> arenaSlice) noexcept {
    if (arenaSlice.size() < sizeof(MixerArenaState) ||
        (reinterpret_cast<usize>(arenaSlice.data()) & 63U) != 0U) {
        return Err(AudioErrc::ArenaExhausted);
    }
    m_state = reinterpret_cast<MixerArenaState*>(arenaSlice.data());
    Reset();
    return {};
}

void AudioMixer::Reset() noexcept {
    if (m_state) {
        *m_state = MixerArenaState{};
    }
    m_activeVoicesMask = 0ULL;
    m_virtualVoicesMask = 0ULL;
    m_activeScopesCache = 0U;
    m_dirtyBusMask = 0U;
    m_pendingStealBusMask = 0U;
    m_telemetry = AudioTelemetrySnapshot{};
}

void AudioMixer::EnsureBusInitialized(u8 busIdx) noexcept {
    const u16 bit = static_cast<u16>(1U << (busIdx & 7U));
    if ((m_dirtyBusMask & bit) == 0U) {
        std::memset(m_state->busPlaneL[busIdx], 0, kMixBlockFrames * sizeof(f32));
        std::memset(m_state->busPlaneR[busIdx], 0, kMixBlockFrames * sizeof(f32));
        m_dirtyBusMask |= bit;
    }
}

void AudioMixer::EnsureReverbSendInitialized() noexcept {
    if ((m_dirtyBusMask & kReverbDirtyBit) == 0U) {
        std::memset(m_state->reverbSendL, 0, kMixBlockFrames * sizeof(f32));
        std::memset(m_state->reverbSendR, 0, kMixBlockFrames * sizeof(f32));
        m_dirtyBusMask |= kReverbDirtyBit;
    }
}

void AudioMixer::AccumulateStolenVoiceTail(u8 slot) noexcept {
    if (!m_state || slot >= kMaxAudioVoices || (m_virtualVoicesMask & (1ULL << slot)) != 0ULL) return;
    const f32* pcm = m_state->pcmPtr[slot];
    const u32 frames = m_state->frameCount[slot];
    if (!pcm || frames == 0U) return;

    const f32 startGainL = m_state->currentGainL[slot];
    const f32 startGainR = m_state->currentGainR[slot];
    if (startGainL < 1e-5f && startGainR < 1e-5f) return;

    const u8 busIdx = m_state->busIndex[slot] & 7U;
    EnsureBusInitialized(busIdx);
    m_pendingStealBusMask |= static_cast<u16>(1U << busIdx);

    f32* dstL = m_state->busPlaneL[busIdx];
    f32* dstR = m_state->busPlaneR[busIdx];
    const u8 ch = std::max<u8>(1U, m_state->channels[slot]);
    const u16 loopStart = m_state->loopStartFrame[slot];
    const bool looping = HasVoiceFlag(m_state->flags[slot], VoiceFlags::Looping);
    u64 phase = m_state->phase32_32[slot];
    const u64 step = (m_state->baseRateStep32_32[slot] * m_state->currentPitch_q12[slot]) >> 12U;

    constexpr f32 kInvBlock = 1.0f / static_cast<f32>(kMixBlockFrames);
    for (u32 s = 0U; s < kMixBlockFrames; ++s) {
        const f32 fade = 1.0f - static_cast<f32>(s) * kInvBlock;
        const i64 baseIdx = static_cast<i64>(phase >> 32U);
        const f32 frac = static_cast<f32>(phase & 0xFFFFFFFFULL) * (1.0f / 4294967296.0f);
        f32 l0, r0, l1, r1;
        SampleFrameStereo(pcm, frames, loopStart, ch, baseIdx,     looping, l0, r0);
        SampleFrameStereo(pcm, frames, loopStart, ch, baseIdx + 1, looping, l1, r1);
        const f32 sL = l0 + (l1 - l0) * frac;
        const f32 sR = r0 + (r1 - r0) * frac;
        dstL[s] += sL * (startGainL * fade);
        dstR[s] += sR * (startGainR * fade);
        phase += step;
    }
    ++m_telemetry.stolenVoiceCount;
}

void AudioMixer::AccumulateMusicDeckTail() noexcept {
    if (!m_state || m_state->musicActiveMask == 0U) return;
    constexpr u8 kMusicBus = static_cast<u8>(AudioBusId::Music);
    EnsureBusInitialized(kMusicBus);
    m_pendingStealBusMask |= static_cast<u16>(1U << kMusicBus);

    f32* musL = m_state->busPlaneL[kMusicBus];
    f32* musR = m_state->busPlaneR[kMusicBus];
    const u64 masterPhase = m_state->musicMasterPhase32_32;
    const u64 masterStep = m_state->musicMasterStep32_32;
    const u16 loopStart = m_state->musicLoopStartFrame;
    const bool loopMusic = (m_state->musicLoop != 0U);
    constexpr f32 kInvBlock = 1.0f / static_cast<f32>(kMixBlockFrames);

    for (u8 stem = 0U; stem < kMaxMusicStems; ++stem) {
        if ((m_state->musicActiveMask & (1U << stem)) == 0U) continue;
        const f32* pcm = m_state->musicStemsPcm[stem];
        const u32 frames = m_state->musicStemFrames[stem];
        const f32 startG = m_state->musicStemCurrentGain[stem] * m_state->musicDeckFadeGain;
        if (!pcm || frames == 0U || startG < 1e-5f) continue;

        const u8 ch = m_state->musicStemChannels[stem];
        u64 phase = masterPhase;
        for (u32 s = 0U; s < kMixBlockFrames; ++s) {
            const f32 fade = 1.0f - static_cast<f32>(s) * kInvBlock;
            const i64 baseIdx = static_cast<i64>(phase >> 32U);
            const f32 frac = static_cast<f32>(phase & 0xFFFFFFFFULL) * (1.0f / 4294967296.0f);
            f32 l0, r0, l1, r1;
            SampleFrameStereo(pcm, frames, loopStart, ch, baseIdx,     loopMusic, l0, r0);
            SampleFrameStereo(pcm, frames, loopStart, ch, baseIdx + 1, loopMusic, l1, r1);
            musL[s] += (l0 + (l1 - l0) * frac) * (startG * fade);
            musR[s] += (r0 + (r1 - r0) * frac) * (startG * fade);
            phase += masterStep;
        }
    }
}

void AudioMixer::ApplyCommand(const AudioCommand& cmd) noexcept {
    if (!m_state) return;

    switch (cmd.type) {
        case AudioCmdType::SpawnVoice: {
            const u8 slot = cmd.voiceSlot & 63U;
            const u64 bit = (1ULL << slot);
            if ((m_activeVoicesMask & bit) != 0ULL) {
                AccumulateStolenVoiceTail(slot);
            }
            if (!cmd.pcmData || cmd.frameCount == 0U) {
                m_activeVoicesMask &= ~bit;
                m_virtualVoicesMask &= ~bit;
                return;
            }

            m_state->pcmPtr[slot] = cmd.pcmData;
            m_state->phase32_32[slot] = 0ULL;
            const u32 safeRate = std::max<u32>(8000U, cmd.sampleRate);
            m_state->baseRateStep32_32[slot] = (static_cast<u64>(safeRate) << 32U) / kAudioSampleRate;
            m_state->frameCount[slot] = cmd.frameCount;
            m_state->currentGainL[slot] = static_cast<f32>(cmd.initialGainL_q14) * (1.0f / 16384.0f);
            m_state->currentGainR[slot] = static_cast<f32>(cmd.initialGainR_q14) * (1.0f / 16384.0f);
            m_state->svfIc1L[slot] = 0.0f;
            m_state->svfIc2L[slot] = 0.0f;
            m_state->svfIc1R[slot] = 0.0f;
            m_state->svfIc2R[slot] = 0.0f;
            m_state->loopStartFrame[slot] = cmd.loopStartFrame;
            m_state->generation[slot] = cmd.generation;
            m_state->fadeRemainingBlocks[slot] = 0U;
            m_state->currentPitch_q12[slot] = std::max<u16>(1U, cmd.initialPitch_q12);
            m_state->channels[slot] = std::max<u8>(1U, cmd.channels);
            m_state->busIndex[slot] = static_cast<u8>(cmd.bus) & 7U;
            m_state->flags[slot] = cmd.flags;
            m_state->assetScope[slot] = cmd.assetScope;

            m_activeVoicesMask |= bit;
            if (HasVoiceFlag(cmd.flags, VoiceFlags::Looping) &&
                cmd.initialGainL_q14 == 0U && cmd.initialGainR_q14 == 0U) {
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
            if (cmd.fadeBlocks <= 1U) {
                AccumulateStolenVoiceTail(slot);
                m_activeVoicesMask &= ~bit;
                m_virtualVoicesMask &= ~bit;
                m_telemetry.activeVoiceCount = static_cast<u16>(std::popcount(m_activeVoicesMask & ~m_virtualVoicesMask));
                m_telemetry.virtualVoiceCount = static_cast<u16>(std::popcount(m_virtualVoicesMask));
            } else {
                m_state->fadeRemainingBlocks[slot] = cmd.fadeBlocks;
            }
            break;
        }
        case AudioCmdType::BindMusicStem: {
            const u8 stemIdx = cmd.voiceSlot & 3U;
            if (stemIdx == 0U && m_state->musicActiveMask != 0U) {
                AccumulateMusicDeckTail();
            }
            m_state->musicStemsPcm[stemIdx] = cmd.pcmData;
            m_state->musicStemFrames[stemIdx] = cmd.frameCount;
            m_state->musicStemChannels[stemIdx] = std::max<u8>(1U, cmd.channels);
            m_state->musicStemScope[stemIdx] = cmd.assetScope;
            m_state->musicStemCurrentGain[stemIdx] = static_cast<f32>(cmd.initialGainL_q14) * (1.0f / 16384.0f);
            if (cmd.pcmData && cmd.frameCount > 0U) {
                m_state->musicActiveMask |= static_cast<u8>(1U << stemIdx);
                const u32 safeRate = std::max<u32>(8000U, cmd.sampleRate);
                m_state->musicMasterStep32_32 = (static_cast<u64>(safeRate) << 32U) / kAudioSampleRate;
            } else {
                m_state->musicActiveMask &= static_cast<u8>(~(1U << stemIdx));
            }
            break;
        }
        case AudioCmdType::CommitMusicDeck: {
            m_state->musicMasterPhase32_32 = 0ULL;
            m_state->musicLoopStartFrame = cmd.loopStartFrame;
            m_state->musicLoop = HasVoiceFlag(cmd.flags, VoiceFlags::Looping) ? 1U : 0U;
            if (cmd.fadeBlocks > 1U) {
                m_state->musicDeckFadeGain = 0.0f;
                m_state->musicDeckFadeStep = 1.0f / static_cast<f32>(cmd.fadeBlocks);
            } else {
                m_state->musicDeckFadeGain = 1.0f;
                m_state->musicDeckFadeStep = 0.0f;
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
    if (!m_state || snapshot.masterMuted != 0U) {
        std::memset(outInterleavedStereo.data(), 0, outInterleavedStereo.size_bytes());
        return;
    }

    m_dirtyBusMask = m_pendingStealBusMask;
    m_pendingStealBusMask = 0U;

    constexpr f32 kInvBlock = 1.0f / static_cast<f32>(kMixBlockFrames);
    const bool reverbEnabled = (snapshot.reverbWetGain_q14 > 0U);

    // 1. Synthesize all active polyphonic voices (or fast-advance Virtual Voices in O(1))
    u64 voiceMask = m_activeVoicesMask & ~snapshot.pausedVoicesMask;
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
        f32 targetGainL = static_cast<f32>(snapshot.targetGainL_q14[slot]) * (1.0f / 16384.0f);
        f32 targetGainR = static_cast<f32>(snapshot.targetGainR_q14[slot]) * (1.0f / 16384.0f);

        bool fadeFinished = false;
        if (m_state->fadeRemainingBlocks[slot] > 0U) {
            const u16 rem = m_state->fadeRemainingBlocks[slot]--;
            const f32 fadeRatio = static_cast<f32>(rem - 1U) / static_cast<f32>(rem);
            targetGainL *= fadeRatio;
            targetGainR *= fadeRatio;
            if (rem == 1U) fadeFinished = true;
        }

        const u16 pitch_q12 = (snapshot.targetPitch_q12[slot] > 0U)
                                  ? snapshot.targetPitch_q12[slot]
                                  : m_state->currentPitch_q12[slot];
        m_state->currentPitch_q12[slot] = pitch_q12;
        const u64 step = (m_state->baseRateStep32_32[slot] * static_cast<u64>(pitch_q12)) >> 12U;
        const VoiceFlags vFlags = m_state->flags[slot];
        const bool looping = HasVoiceFlag(vFlags, VoiceFlags::Looping);
        const u16 loopStart = m_state->loopStartFrame[slot];

        // Virtual Voice fast-path: out-of-earshot looping voices advance phase in O(1) without DSP synthesis
        if (looping && !fadeFinished && curGainL < 1e-6f && curGainR < 1e-6f &&
            targetGainL < 1e-6f && targetGainR < 1e-6f) {
            m_virtualVoicesMask |= slotBit;
            m_state->phase32_32[slot] = WrapLoopPhase32_32(
                m_state->phase32_32[slot] + step * kMixBlockFrames, frames, loopStart);
            m_state->currentGainL[slot] = 0.0f;
            m_state->currentGainR[slot] = 0.0f;
            continue;
        }
        m_virtualVoicesMask &= ~slotBit;

        const u8 busIdx = m_state->busIndex[slot] & 7U;
        EnsureBusInitialized(busIdx);
        f32* busL = m_state->busPlaneL[busIdx];
        f32* busR = m_state->busPlaneR[busIdx];

        const bool sendToReverb = reverbEnabled && HasVoiceFlag(vFlags, VoiceFlags::ReverbSend);
        if (sendToReverb) EnsureReverbSendInitialized();

        const f32 stepGainL = (targetGainL - curGainL) * kInvBlock;
        const f32 stepGainR = (targetGainR - curGainR) * kInvBlock;

        const u16 cutoffHz = snapshot.voiceCutoffHz[slot];
        const bool useSvf = (cutoffHz > 0U && cutoffHz < 18500U);
        f32 a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        if (useSvf) {
            ComputeCytomicSvfCoeffs(static_cast<f32>(cutoffHz), a1, a2, a3);
        }
        f32 ic1L = m_state->svfIc1L[slot], ic2L = m_state->svfIc2L[slot];
        f32 ic1R = m_state->svfIc1R[slot], ic2R = m_state->svfIc2R[slot];

        u64 phase = m_state->phase32_32[slot];
        const u8 ch = m_state->channels[slot];
        bool finished = fadeFinished;

        for (u32 s = 0U; s < kMixBlockFrames; ++s) {
            const i64 baseIdx = static_cast<i64>(phase >> 32U);
            if (!looping && baseIdx >= static_cast<i64>(frames)) {
                finished = true;
                break;
            }

            const f32 frac = static_cast<f32>(phase & 0xFFFFFFFFULL) * (1.0f / 4294967296.0f);
            f32 sL = 0.0f, sR = 0.0f;
            SampleHermiteStereo(pcm, frames, loopStart, ch, baseIdx, frac, looping, sL, sR);

            if (useSvf) {
                sL = StepCytomicSvf(sL, ic1L, ic2L, a1, a2, a3, FilterMode::LowPass);
                sR = (ch >= 2U) ? StepCytomicSvf(sR, ic1R, ic2R, a1, a2, a3, FilterMode::LowPass) : sL;
            }

            curGainL += stepGainL;
            curGainR += stepGainR;
            const f32 outL = sL * curGainL;
            const f32 outR = sR * curGainR;
            busL[s] += outL;
            busR[s] += outR;
            if (sendToReverb) {
                m_state->reverbSendL[s] += outL * 0.5f;
                m_state->reverbSendR[s] += outR * 0.5f;
            }
            phase += step;
        }

        if (finished) {
            m_activeVoicesMask &= ~slotBit;
            m_virtualVoicesMask &= ~slotBit;
        } else {
            m_state->phase32_32[slot] = looping ? WrapLoopPhase32_32(phase, frames, loopStart) : phase;
            m_state->currentGainL[slot] = targetGainL;
            m_state->currentGainR[slot] = targetGainR;
            m_state->svfIc1L[slot] = ic1L;
            m_state->svfIc2L[slot] = ic2L;
            m_state->svfIc1R[slot] = ic1R;
            m_state->svfIc2R[slot] = ic2R;
        }
    }

    // 2. Synthesize 4-stem sample-locked interactive music deck into AudioBusId::Music (bus 1)
    if (m_state->musicActiveMask != 0U) {
        constexpr u8 kMusicBus = static_cast<u8>(AudioBusId::Music);
        EnsureBusInitialized(kMusicBus);
        f32* musL = m_state->busPlaneL[kMusicBus];
        f32* musR = m_state->busPlaneR[kMusicBus];
        const u64 masterPhase = m_state->musicMasterPhase32_32;
        const u64 masterStep = m_state->musicMasterStep32_32;
        const u16 loopStart = m_state->musicLoopStartFrame;
        const bool loopMusic = (m_state->musicLoop != 0U);

        const f32 deckStartFade = m_state->musicDeckFadeGain;
        const f32 deckEndFade = std::min(1.0f, deckStartFade + m_state->musicDeckFadeStep);
        m_state->musicDeckFadeGain = deckEndFade;

        u32 maxStemFrames = 0U;
        for (u8 stem = 0U; stem < kMaxMusicStems; ++stem) {
            if ((m_state->musicActiveMask & (1U << stem)) == 0U) continue;
            const f32* pcm = m_state->musicStemsPcm[stem];
            const u32 frames = m_state->musicStemFrames[stem];
            if (!pcm || frames == 0U) continue;
            maxStemFrames = std::max(maxStemFrames, frames);

            f32 curG = m_state->musicStemCurrentGain[stem] * deckStartFade;
            const f32 rawTargetG = static_cast<f32>(snapshot.musicStemWeights_q14[stem]) * (1.0f / 16384.0f);
            const f32 targetG = rawTargetG * deckEndFade;
            const f32 stepG = (targetG - curG) * kInvBlock;
            const u8 ch = m_state->musicStemChannels[stem];
            u64 phase = masterPhase;

            for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                const i64 baseIdx = static_cast<i64>(phase >> 32U);
                const f32 frac = static_cast<f32>(phase & 0xFFFFFFFFULL) * (1.0f / 4294967296.0f);
                f32 sL = 0.0f, sR = 0.0f;
                SampleHermiteStereo(pcm, frames, loopStart, ch, baseIdx, frac, loopMusic, sL, sR);
                curG += stepG;
                musL[s] += sL * curG;
                musR[s] += sR * curG;
                phase += masterStep;
            }
            m_state->musicStemCurrentGain[stem] = rawTargetG;
        }
        const u64 nextMasterPhase = masterPhase + masterStep * kMixBlockFrames;
        m_state->musicMasterPhase32_32 = (loopMusic && maxStemFrames > 0U)
                                             ? WrapLoopPhase32_32(nextMasterPhase, maxStemFrames, loopStart)
                                             : nextMasterPhase;
    }

    // 3. Measure SfxCombat & Voice bus peak energy for Sidechain Envelope Ducking on Music & Ambience
    f32 sidechainPeak = 0.0f;
    for (u8 trigBus : {static_cast<u8>(AudioBusId::SfxCombat), static_cast<u8>(AudioBusId::Voice)}) {
        if ((m_dirtyBusMask & (1U << trigBus)) == 0U) continue;
        const f32* bL = m_state->busPlaneL[trigBus];
        const f32* bR = m_state->busPlaneR[trigBus];
        for (u32 s = 0U; s < kMixBlockFrames; s += 4U) {
            sidechainPeak = std::max(sidechainPeak, std::max(std::abs(bL[s]), std::abs(bR[s])));
        }
    }
    const f32 envTarget = std::clamp(sidechainPeak, 0.0f, 1.0f);
    const f32 slewRate = (envTarget > m_state->sidechainEnvelope) ? 0.35f : 0.05f;
    m_state->sidechainEnvelope += (envTarget - m_state->sidechainEnvelope) * slewRate;
    const f32 duckStrength = static_cast<f32>(snapshot.sidechainDuckStrength_q14) * (1.0f / 16384.0f);
    const f32 duckMultiplier = std::clamp(1.0f - m_state->sidechainEnvelope * duckStrength, 0.15f, 1.0f);

    // 4. Sum sub-buses (1..7) with per-bus Cytomic SVF filtering into Master bus (0) and optional Aux Reverb send
    EnsureBusInitialized(0U);
    f32* masterL = m_state->busPlaneL[0];
    f32* masterR = m_state->busPlaneR[0];

    for (u8 b = 1U; b < kAudioBusCount; ++b) {
        if ((m_dirtyBusMask & (1U << b)) == 0U) continue;
        f32* bL = m_state->busPlaneL[b];
        f32* bR = m_state->busPlaneR[b];

        f32 targetBusG = static_cast<f32>(snapshot.busGain_q14[b]) * (1.0f / 16384.0f);
        if (b == static_cast<u8>(AudioBusId::Music) || b == static_cast<u8>(AudioBusId::Ambience)) {
            targetBusG *= duckMultiplier;
        }
        f32 curBusG = m_state->busCurrentGain[b];
        const f32 stepBusG = (targetBusG - curBusG) * kInvBlock;

        const FilterMode fMode = snapshot.busFilterMode[b];
        if (fMode != FilterMode::Bypass) {
            f32 a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            ComputeCytomicSvfCoeffs(static_cast<f32>(snapshot.busCutoffHz[b]), a1, a2, a3);
            f32 ic1L = m_state->busIc1L[b], ic2L = m_state->busIc2L[b];
            f32 ic1R = m_state->busIc1R[b], ic2R = m_state->busIc2R[b];

            for (u32 s = 0U; s < kMixBlockFrames; ++s) {
                bL[s] = StepCytomicSvf(bL[s], ic1L, ic2L, a1, a2, a3, fMode);
                bR[s] = StepCytomicSvf(bR[s], ic1R, ic2R, a1, a2, a3, fMode);
            }
            m_state->busIc1L[b] = ic1L;
            m_state->busIc2L[b] = ic2L;
            m_state->busIc1R[b] = ic1R;
            m_state->busIc2R[b] = ic2R;
        }

        const bool busReverbSend = reverbEnabled && ((snapshot.reverbBusSendMask & (1U << b)) != 0U);
        if (busReverbSend) EnsureReverbSendInitialized();

        for (u32 s = 0U; s < kMixBlockFrames; ++s) {
            curBusG += stepBusG;
            const f32 outL = bL[s] * curBusG;
            const f32 outR = bR[s] * curBusG;
            masterL[s] += outL;
            masterR[s] += outR;
            if (busReverbSend) {
                m_state->reverbSendL[s] += outL * 0.4f;
                m_state->reverbSendR[s] += outR * 0.4f;
            }
        }
        m_state->busCurrentGain[b] = targetBusG;
    }

    // 5. Process Stereo Schroeder / Freeverb Aux Reverb (4 lowpass-comb + 2 allpass per channel) if active
    f32 reverbPeak = 0.0f;
    if (reverbEnabled && (m_dirtyBusMask & kReverbDirtyBit) != 0U) {
        const f32 wetGain = static_cast<f32>(snapshot.reverbWetGain_q14) * (1.0f / 16384.0f);
        const f32 feedback = 0.70f + 0.26f * (static_cast<f32>(snapshot.reverbRoomSize_q8) * (1.0f / 255.0f));
        const f32 damp1 = 0.15f + 0.65f * (static_cast<f32>(snapshot.reverbDamping_q8) * (1.0f / 255.0f));
        const f32 damp2 = 1.0f - damp1;

        for (u32 s = 0U; s < kMixBlockFrames; ++s) {
            const f32 inL = m_state->reverbSendL[s] * 0.25f;
            const f32 inR = m_state->reverbSendR[s] * 0.25f;
            f32 accL = 0.0f;
            f32 accR = 0.0f;

            for (u32 c = 0U; c < 4U; ++c) {
                const u16 idxL = m_state->combIdxL[c];
                const f32 outCombL = m_state->combBufL[c][idxL];
                m_state->combStoreL[c] = outCombL * damp2 + m_state->combStoreL[c] * damp1;
                m_state->combBufL[c][idxL] = inL + m_state->combStoreL[c] * feedback;
                m_state->combIdxL[c] = static_cast<u16>((idxL + 1U >= kCombLengthsL[c]) ? 0U : (idxL + 1U));
                accL += outCombL;

                const u16 idxR = m_state->combIdxR[c];
                const f32 outCombR = m_state->combBufR[c][idxR];
                m_state->combStoreR[c] = outCombR * damp2 + m_state->combStoreR[c] * damp1;
                m_state->combBufR[c][idxR] = inR + m_state->combStoreR[c] * feedback;
                m_state->combIdxR[c] = static_cast<u16>((idxR + 1U >= kCombLengthsR[c]) ? 0U : (idxR + 1U));
                accR += outCombR;
            }

            for (u32 a = 0U; a < 2U; ++a) {
                const u16 apIdxL = m_state->allpassIdxL[a];
                const f32 bufOutL = m_state->allpassBufL[a][apIdxL];
                m_state->allpassBufL[a][apIdxL] = accL + bufOutL * 0.5f;
                accL = bufOutL - accL * 0.5f;
                m_state->allpassIdxL[a] = static_cast<u16>((apIdxL + 1U >= kAllpassLengthsL[a]) ? 0U : (apIdxL + 1U));

                const u16 apIdxR = m_state->allpassIdxR[a];
                const f32 bufOutR = m_state->allpassBufR[a][apIdxR];
                m_state->allpassBufR[a][apIdxR] = accR + bufOutR * 0.5f;
                accR = bufOutR - accR * 0.5f;
                m_state->allpassIdxR[a] = static_cast<u16>((apIdxR + 1U >= kAllpassLengthsR[a]) ? 0U : (apIdxR + 1U));
            }

            const f32 wetL = accL * wetGain;
            const f32 wetR = accR * wetGain;
            masterL[s] += wetL;
            masterR[s] += wetR;
            reverbPeak = std::max(reverbPeak, std::max(std::abs(wetL), std::abs(wetR)));
        }
    }

    // 6. Apply Master bus gain, rational Padé [3/2] tanh soft-clipping limiter, and interleave to output
    const f32 targetMasterG = static_cast<f32>(snapshot.busGain_q14[0]) * (1.0f / 16384.0f);
    f32 curMasterG = m_state->busCurrentGain[0];
    const f32 stepMasterG = (targetMasterG - curMasterG) * kInvBlock;
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
    m_state->busCurrentGain[0] = targetMasterG;

    m_telemetry.mixedBlocksCount += 1ULL;
    m_telemetry.masterPeakL = peakL;
    m_telemetry.masterPeakR = peakR;
    m_telemetry.sidechainEnvelope = m_state->sidechainEnvelope;
    m_telemetry.reverbPeak = reverbPeak;
    m_telemetry.activeVoiceCount = static_cast<u16>(std::popcount(m_activeVoicesMask & ~m_virtualVoicesMask));
    m_telemetry.virtualVoiceCount = static_cast<u16>(std::popcount(m_virtualVoicesMask));
    m_telemetry.activeMusicStems = static_cast<u16>(std::popcount(m_state->musicActiveMask));
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
        if ((m_state->musicActiveMask & (1U << st)) != 0U) {
            mask |= (1U << (m_state->musicStemScope[st] & 31U));
        }
    }
    return mask;
}

u64 AudioMixer::ComputeAuditHash() const noexcept {
    u64 h = 14695981039346656037ULL;
    const auto mixBytes = [&](const void* data, usize len) noexcept {
        const auto* bytes = static_cast<const u8*>(data);
        for (usize i = 0U; i < len; ++i) {
            h ^= static_cast<u64>(bytes[i]);
            h *= 1099511628211ULL;
        }
    };
    mixBytes(&m_activeVoicesMask, sizeof(m_activeVoicesMask));
    mixBytes(&m_virtualVoicesMask, sizeof(m_virtualVoicesMask));
    mixBytes(&m_telemetry.mixedBlocksCount, sizeof(m_telemetry.mixedBlocksCount));
    mixBytes(&m_telemetry.activeVoiceCount, sizeof(m_telemetry.activeVoiceCount));
    mixBytes(&m_telemetry.stolenVoiceCount, sizeof(m_telemetry.stolenVoiceCount));
    if (m_state) {
        mixBytes(m_state->phase32_32, sizeof(m_state->phase32_32));
        mixBytes(m_state->currentGainL, sizeof(m_state->currentGainL));
        mixBytes(m_state->currentGainR, sizeof(m_state->currentGainR));
        mixBytes(&m_state->musicMasterPhase32_32, sizeof(m_state->musicMasterPhase32_32));
    }
    return h;
}

} // namespace en
