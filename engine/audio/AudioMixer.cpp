#include "audio/AudioMixer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

namespace en::audio {

namespace {

inline constexpr f32 kPcm16ToFloat   = 1.0f / 32768.0f;
inline constexpr f32 kQ14ToFloat     = 1.0f / 16384.0f;
inline constexpr f32 kInvBlockFrames = 1.0f / static_cast<f32>(kMixBlockFrames);
inline constexpr f32 kFracQ32ToFloat = 1.0f / 4294967296.0f;
inline constexpr f32 kAntiDenormalDc = 1.0e-20f;
inline constexpr f32 kSvfResonanceK  = 1.41421356f; // Butterworth Q = 0.7071

[[nodiscard]] constexpr usize AlignOffset64(const u8* ptr) noexcept {
    const usize rem = std::bit_cast<usize>(ptr) & 63u;
    return rem == 0u ? 0u : (64u - rem);
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

constexpr void StepSvfSample(
    f32 input,
    f32 a1,
    f32 a2,
    f32 a3,
    f32& ic1eq,
    f32& ic2eq,
    f32& outV1,
    f32& outV2) noexcept {
    const f32 v0 = input + kAntiDenormalDc;
    const f32 v3 = v0 - ic2eq;
    outV1        = a1 * ic1eq + a2 * v3;
    outV2        = ic2eq + a2 * ic1eq + a3 * v3;
    ic1eq        = 2.0f * outV1 - ic1eq;
    ic2eq        = 2.0f * outV2 - ic2eq;
}

constexpr void UpdateBit(u64& mask, u64 bit, bool set) noexcept {
    mask = (mask & ~bit) | (set ? bit : 0ULL);
}

// Lazily clears a bus scratch plane only on its first write within the current block.
inline void EnsureBusInitialized(
    MixerArenaState& state,
    u8 busIdx,
    u8& ioDirtyBusMask,
    f32*& outBusL,
    f32*& outBusR) noexcept {
    const usize offset = static_cast<usize>(busIdx) * kMixBlockFrames;
    outBusL            = &state.busScratchL[offset];
    outBusR            = &state.busScratchR[offset];
    const u8 bit       = static_cast<u8>(1u << busIdx);
    if ((ioDirtyBusMask & bit) == 0u) {
        ioDirtyBusMask |= bit;
        std::memset(outBusL, 0, sizeof(f32) * kMixBlockFrames);
        std::memset(outBusR, 0, sizeof(f32) * kMixBlockFrames);
    }
}

} // namespace

Result<void, AudioErrc> AudioMixer::Init(std::span<u8> arenaSlice) noexcept {
    if (arenaSlice.size() < RequiredArenaBytes()) {
        return Err(AudioErrc::ArenaExhausted);
    }

    u8* alignedPtr = arenaSlice.data() + AlignOffset64(arenaSlice.data());
    m_state        = std::construct_at(reinterpret_cast<MixerArenaState*>(alignedPtr));
    Reset();
    return {};
}

void AudioMixer::Reset() noexcept {
    if (!m_state) {
        return;
    }
    const u32 preservedSnapSeq      = m_state->voices.lastSnapshotSeq;
    *m_state                        = MixerArenaState{};
    m_state->voices.lastSnapshotSeq = preservedSnapSeq;
    m_state->busDsp.curGain.fill(1.0f);
    m_deckCrossfadeAlpha  = 0.0f;
    m_deckCrossfadeStep   = 0.0f;
    m_pendingStealBusMask = 0u;
    m_telemetry           = AudioTelemetrySnapshot{};
    m_sampleCounter       = 0;
}

// Mixes a 256-sample linear fade-out of an overwritten voice directly into its target bus plane.
void AudioMixer::AccumulateStolenVoiceTail(u32 slot) noexcept {
    const auto& voices = m_state->voices;
    const i16* pcm     = voices.pcmPtr[slot];
    const u32  frames  = voices.frameCount[slot];
    const f32  gL      = voices.curGainL[slot];
    const f32  gR      = voices.curGainR[slot];
    if (!pcm || frames < 4u || (gL <= 1.0e-5f && gR <= 1.0e-5f)) {
        return;
    }

    const u8 busIdx = static_cast<u8>(voices.busAndChannel[slot] & 0x07u);
    f32* busL       = nullptr;
    f32* busR       = nullptr;
    EnsureBusInitialized(*m_state, busIdx, m_pendingStealBusMask, busL, busR);

    u64 phase          = voices.phaseQ32[slot];
    const u64 stepQ32  = (voices.baseStepQ32[slot] * voices.spawnPitchQ12[slot]) >> 12u;
    for (u32 i = 0; i < kMixBlockFrames; ++i) {
        const u32 idx = static_cast<u32>(phase >> 32u);
        if (idx >= frames) {
            break;
        }
        const f32 s = (static_cast<f32>(pcm[idx]) * kPcm16ToFloat) *
                      (1.0f - static_cast<f32>(i) * kInvBlockFrames);
        busL[i] += s * gL;
        busR[i] += s * gR;
        phase   += stepQ32;
    }
    ++m_telemetry.stolenVoicesTotal;
}

void AudioMixer::ApplyCommand(const AudioCommand& cmd) noexcept {
    if (!m_state) {
        return;
    }
    auto& voices = m_state->voices;

    switch (cmd.type) {
    case AudioCmdType::SpawnVoice: {
        const u32 slot = cmd.voiceSlot;
        if (slot >= kMaxVoices || !cmd.payload.spawn.pcm || cmd.payload.spawn.frameCount == 0u) {
            return;
        }
        const u64 bit = 1ULL << slot;
        if ((voices.activeMask & bit) != 0ULL) {
            AccumulateStolenVoiceTail(slot);
        }

        const VoiceFlags vf    = static_cast<VoiceFlags>(cmd.flags);
        voices.activeMask     |= bit;
        voices.freshSpawnMask |= bit;
        voices.stoppingMask   &= ~bit;
        UpdateBit(voices.loopMask, bit, HasVoiceFlag(vf, VoiceFlags::Looping));
        UpdateBit(voices.pauseOnHitstopMask, bit, HasVoiceFlag(vf, VoiceFlags::PauseOnHitstop));
        UpdateBit(voices.followTimeScaleMask, bit, HasVoiceFlag(vf, VoiceFlags::FollowTimeScale));

        voices.pcmPtr[slot]        = cmd.payload.spawn.pcm;
        voices.phaseQ32[slot]      = 0ULL;
        voices.baseStepQ32[slot]   = (static_cast<u64>(cmd.payload.spawn.sampleRate) << 32u) / kSampleRate;
        voices.frameCount[slot]    = cmd.payload.spawn.frameCount;
        voices.loopStart[slot]     = std::min(cmd.payload.spawn.loopStartFrame, cmd.payload.spawn.frameCount - 1u);
        voices.curGainL[slot]      = static_cast<f32>(cmd.payload.spawn.gainL_Q14) * kQ14ToFloat;
        voices.curGainR[slot]      = static_cast<f32>(cmd.payload.spawn.gainR_Q14) * kQ14ToFloat;
        voices.svfIc1eq[slot]      = 0.0f;
        voices.svfIc2eq[slot]      = 0.0f;
        voices.generation[slot]    = cmd.generation;
        voices.spawnPitchQ12[slot] = cmd.payload.spawn.pitchRatioQ12 != 0u ? cmd.payload.spawn.pitchRatioQ12 : 4096u;
        voices.busAndChannel[slot] = cmd.packedBusAndChannel;
        voices.scopeId[slot]       = cmd.assetScopeId & 0x1Fu;
        break;
    }
    case AudioCmdType::StopVoice: {
        const u32 slot = cmd.voiceSlot;
        if (slot >= kMaxVoices || voices.generation[slot] != cmd.generation) {
            return;
        }
        const u64 bit = 1ULL << slot;
        if (cmd.payload.stop.fadeOutBlocks == 0u) {
            voices.activeMask   &= ~bit;
            voices.stoppingMask &= ~bit;
        } else {
            voices.stoppingMask |= bit;
            voices.stopBlocksRemaining[slot] = cmd.payload.stop.fadeOutBlocks;
        }
        break;
    }
    case AudioCmdType::BindMusicStem: {
        const u8 deckIdx = (cmd.payload.music.deckAndStemIdx >> 7u) & 0x01u;
        const u8 stemIdx = cmd.payload.music.deckAndStemIdx & 0x03u;
        const bool reset = (cmd.payload.music.deckAndStemIdx & 0x40u) != 0u;

        if (deckIdx == 1u && reset && m_state->decks[1].IsActive()) {
            m_state->decks[0]    = m_state->decks[1];
            m_state->decks[1]    = MusicDeckRuntime{};
            m_deckCrossfadeAlpha = 0.0f;
            m_deckCrossfadeStep  = 0.0f;
        } else if (reset) {
            m_state->decks[deckIdx] = MusicDeckRuntime{};
        }

        MusicDeckRuntime& deck = m_state->decks[deckIdx];
        const u32 frames       = cmd.payload.music.frameCount;
        const u32 rawLoopEnd   = cmd.payload.music.loopEndFrame;
        deck.stemPcm[stemIdx]  = cmd.payload.music.pcm;
        deck.loopEndFrame      = (rawLoopEnd > 0u && rawLoopEnd <= frames) ? rawLoopEnd : frames;
        deck.loopStartFrame    = std::min(cmd.payload.music.loopStartFrame, deck.loopEndFrame > 0u ? deck.loopEndFrame - 1u : 0u);
        deck.tempoBpmQ8        = cmd.payload.music.tempoBpmQ8;
        deck.stemCount         = std::max<u8>(deck.stemCount, static_cast<u8>(stemIdx + 1u));
        deck.SetActive(deck.IsActive(), cmd.assetScopeId);
        break;
    }
    case AudioCmdType::CommitMusicDeck: {
        const u8 crossfadeBlocks = cmd.payload.music.crossfadeBlocks;
        MusicDeckRuntime& deckB  = m_state->decks[1];
        deckB.cursorFrame        = 0u;
        deckB.SetActive(true, deckB.ScopeId());
        if (!m_state->decks[0].IsActive() || crossfadeBlocks == 0u) {
            m_state->decks[0]    = deckB;
            deckB                = MusicDeckRuntime{};
            m_deckCrossfadeAlpha = 0.0f;
            m_deckCrossfadeStep  = 0.0f;
        } else {
            m_deckCrossfadeAlpha = 0.0f;
            m_deckCrossfadeStep  = 1.0f / static_cast<f32>(crossfadeBlocks);
        }
        break;
    }
    case AudioCmdType::PanicReset:
        Reset();
        break;
    }
}

u8 AudioMixer::MixActiveVoicesPlanar(
    const AudioFrameSnapshot& snapshot,
    u8 dirtyBusMask,
    u32& outScopeMask) noexcept {
    auto& voices  = m_state->voices;
    u32 scopeMask = 0u;

    if (snapshot.snapshotSeq != voices.lastSnapshotSeq) {
        voices.lastSnapshotSeq = snapshot.snapshotSeq;
        voices.freshSpawnMask  = 0ULL;
    }

    alignas(64) std::array<f32, kMixBlockFrames> mono{};

    for (u64 mask = voices.activeMask; mask != 0ULL; mask &= (mask - 1ULL)) {
        const u32 slot = static_cast<u32>(std::countr_zero(mask));
        const u64 bit  = 1ULL << slot;

        const u8   busAndCh   = voices.busAndChannel[slot];
        const u8   busIdx     = static_cast<u8>(busAndCh & 0x07u);
        const u8   channelId  = static_cast<u8>((busAndCh >> 3u) & 0x07u);
        const bool isFrozen   = (snapshot.frozenChannelMask & (1u << channelId)) != 0u;
        const bool holdPhase  = isFrozen && ((voices.pauseOnHitstopMask & bit) != 0ULL);
        const bool freshSpawn = (voices.freshSpawnMask & bit) != 0ULL;

        f32 gainL   = voices.curGainL[slot];
        f32 gainR   = voices.curGainR[slot];
        f32 targetL = freshSpawn ? gainL : (static_cast<f32>(snapshot.targetGainL_Q14[slot]) * kQ14ToFloat);
        f32 targetR = freshSpawn ? gainR : (static_cast<f32>(snapshot.targetGainR_Q14[slot]) * kQ14ToFloat);

        if (holdPhase || (voices.stoppingMask & bit) != 0ULL) {
            targetL = 0.0f;
            targetR = 0.0f;
        }

        if (holdPhase && gainL <= 1.0e-5f && gainR <= 1.0e-5f) {
            voices.curGainL[slot] = 0.0f;
            voices.curGainR[slot] = 0.0f;
            scopeMask |= (1u << voices.scopeId[slot]);
            continue;
        }

        u64 pitchScaleQ12 = (!freshSpawn && snapshot.targetPitchQ12[slot] != 0u)
                                ? snapshot.targetPitchQ12[slot]
                                : voices.spawnPitchQ12[slot];
        if ((voices.followTimeScaleMask & bit) != 0ULL) {
            const u64 chScale = snapshot.channelTimeScaleQ12[channelId];
            pitchScaleQ12     = (pitchScaleQ12 * (chScale != 0u ? chScale : 4096u)) >> 12u;
        }
        pitchScaleQ12     = std::clamp<u64>(pitchScaleQ12, 1024u, 8192u);
        const u64 stepQ32 = holdPhase ? 0ULL : ((voices.baseStepQ32[slot] * pitchScaleQ12) >> 12u);

        const i16* pcm    = voices.pcmPtr[slot];
        const u32  frames = voices.frameCount[slot];
        const u32  loopSt = voices.loopStart[slot];
        const bool loops  = (voices.loopMask & bit) != 0ULL;

        u64 phase     = voices.phaseQ32[slot];
        u32 generated = kMixBlockFrames;
        bool ended    = false;

        // Stage A: Resample up to 256 mono frames (branchless interior fast path vs boundary loop).
        const u32 startIdx = static_cast<u32>(phase >> 32u);
        const u32 endIdx   = static_cast<u32>((phase + stepQ32 * (kMixBlockFrames - 1u)) >> 32u);
        if (startIdx >= 1u && endIdx + 2u < frames) {
            for (u32 i = 0; i < kMixBlockFrames; ++i) {
                const u32 idx  = static_cast<u32>(phase >> 32u);
                const f32 frac = static_cast<f32>(phase & 0xFFFFFFFFULL) * kFracQ32ToFloat;
                mono[i]        = SampleHermite4(
                    static_cast<f32>(pcm[idx - 1u]),
                    static_cast<f32>(pcm[idx]),
                    static_cast<f32>(pcm[idx + 1u]),
                    static_cast<f32>(pcm[idx + 2u]),
                    frac) * kPcm16ToFloat;
                phase += stepQ32;
            }
        } else {
            const i16 guard2 = (loops && loopSt < frames) ? pcm[loopSt] : i16(0);
            const i16 guard3 = (loops && loopSt < frames) ? pcm[std::min(loopSt + 1u, frames - 1u)] : i16(0);
            for (u32 i = 0; i < kMixBlockFrames; ++i) {
                u32 idx = static_cast<u32>(phase >> 32u);
                if (idx >= frames) {
                    if (loops && loopSt < frames) {
                        const u32 loopLen = frames - loopSt;
                        idx               = loopSt + ((idx - loopSt) % loopLen);
                        phase             = (static_cast<u64>(idx) << 32u) | (phase & 0xFFFFFFFFULL);
                    } else {
                        generated = i;
                        ended     = true;
                        break;
                    }
                }
                const f32 frac = static_cast<f32>(phase & 0xFFFFFFFFULL) * kFracQ32ToFloat;
                mono[i]        = SampleHermite4(
                    static_cast<f32>((idx > 0u) ? pcm[idx - 1u] : pcm[0]),
                    static_cast<f32>(pcm[idx]),
                    static_cast<f32>((idx + 1u < frames) ? pcm[idx + 1u] : guard2),
                    static_cast<f32>((idx + 2u < frames) ? pcm[idx + 2u] : guard3),
                    frac) * kPcm16ToFloat;
                phase += stepQ32;
            }
        }

        // Stage B: Optional Cytomic SVF low-pass over generated mono samples.
        const f32 g = freshSpawn ? 0.0f : (static_cast<f32>(snapshot.targetSvfG_Q14[slot]) * kQ14ToFloat);
        if (g > 0.001f) {
            const f32 a1 = 1.0f / (1.0f + g * (g + kSvfResonanceK));
            const f32 a2 = g * a1;
            const f32 a3 = g * a2;
            f32 ic1eq    = voices.svfIc1eq[slot];
            f32 ic2eq    = voices.svfIc2eq[slot];
            for (u32 i = 0; i < generated; ++i) {
                f32 v1 = 0.0f;
                StepSvfSample(mono[i], a1, a2, a3, ic1eq, ic2eq, v1, mono[i]);
            }
            voices.svfIc1eq[slot] = ic1eq;
            voices.svfIc2eq[slot] = ic2eq;
        }

        // Stage C: SIMD-vectorizable stereo gain ramp accumulation into bus scratch planes.
        f32* busL = nullptr;
        f32* busR = nullptr;
        EnsureBusInitialized(*m_state, busIdx, dirtyBusMask, busL, busR);

        const f32 stepL = (targetL - gainL) * kInvBlockFrames;
        const f32 stepR = (targetR - gainR) * kInvBlockFrames;
        for (u32 i = 0; i < generated; ++i) {
            busL[i] += mono[i] * gainL;
            busR[i] += mono[i] * gainR;
            gainL   += stepL;
            gainR   += stepR;
        }

        voices.phaseQ32[slot] = phase;
        voices.curGainL[slot] = gainL;
        voices.curGainR[slot] = gainR;

        if (ended) {
            voices.activeMask   &= ~bit;
            voices.stoppingMask &= ~bit;
        } else if ((voices.stoppingMask & bit) != 0ULL) {
            if (voices.stopBlocksRemaining[slot] <= 1u) {
                voices.activeMask   &= ~bit;
                voices.stoppingMask &= ~bit;
            } else {
                --voices.stopBlocksRemaining[slot];
                scopeMask |= (1u << voices.scopeId[slot]);
            }
        } else {
            scopeMask |= (1u << voices.scopeId[slot]);
        }
    }

    outScopeMask = scopeMask;
    return dirtyBusMask;
}

u8 AudioMixer::MixMusicDecksPlanar(
    const AudioFrameSnapshot& snapshot,
    u8 dirtyBusMask,
    u32& ioScopeMask) noexcept {
    auto& decks = m_state->decks;
    if (!decks[0].IsActive() && !decks[1].IsActive()) {
        return dirtyBusMask;
    }

    f32 gainDeckA = 1.0f;
    f32 gainDeckB = 0.0f;
    EvalConstantPowerPan((m_deckCrossfadeAlpha * 2.0f) - 1.0f, gainDeckA, gainDeckB);
    const std::array<f32, 2> deckGains{gainDeckA, gainDeckB};

    f32* musicL = nullptr;
    f32* musicR = nullptr;

    for (u32 d = 0; d < 2u; ++d) {
        MusicDeckRuntime& deck = decks[d];
        if (!deck.IsActive() || deck.loopEndFrame == 0u) {
            continue;
        }
        ioScopeMask |= (1u << deck.ScopeId());
        EnsureBusInitialized(*m_state, static_cast<u8>(AudioBusId::Music), dirtyBusMask, musicL, musicR);

        const f32 deckGain  = deckGains[d];
        const u32 loopStart = deck.loopStartFrame < deck.loopEndFrame ? deck.loopStartFrame : 0u;
        const u32 loopEnd   = deck.loopEndFrame;
        u32 nextDeckCursor  = deck.cursorFrame;

        for (u8 s = 0; s < deck.stemCount; ++s) {
            const i16* pcm = deck.stemPcm[s];
            if (!pcm) {
                continue;
            }
            const f32 targetW = static_cast<f32>(snapshot.stemWeightsQ14[s]) * kQ14ToFloat * deckGain;
            f32       curW    = deck.curStemWeight[s];
            const f32 stepW   = (targetW - curW) * kInvBlockFrames;

            u32 cursor    = deck.cursorFrame;
            u32 remaining = kMixBlockFrames;
            u32 outIdx    = 0u;
            while (remaining > 0u) {
                if (cursor >= loopEnd) {
                    cursor = loopStart;
                }
                const u32 chunk = std::min(remaining, loopEnd - cursor);
                for (u32 i = 0; i < chunk; ++i) {
                    const f32 sample = static_cast<f32>(pcm[cursor + i]) * kPcm16ToFloat * curW;
                    musicL[outIdx + i] += sample;
                    musicR[outIdx + i] += sample;
                    curW               += stepW;
                }
                cursor    += chunk;
                outIdx    += chunk;
                remaining -= chunk;
            }
            deck.curStemWeight[s] = curW;
            nextDeckCursor        = cursor;
        }
        deck.cursorFrame = nextDeckCursor;
    }

    if (decks[1].IsActive() && m_deckCrossfadeStep > 0.0f) {
        m_deckCrossfadeAlpha += m_deckCrossfadeStep;
        if (m_deckCrossfadeAlpha >= 1.0f) {
            decks[0]             = decks[1];
            decks[1]             = MusicDeckRuntime{};
            m_deckCrossfadeAlpha = 0.0f;
            m_deckCrossfadeStep  = 0.0f;
        }
    }
    return dirtyBusMask;
}

void AudioMixer::ProcessBusesAndMaster(
    const AudioFrameSnapshot& snapshot,
    u8 dirtyBusMask,
    std::span<f32, kMixBlockFrames * 2> outStereo) noexcept {
    auto& busDsp = m_state->busDsp;

    // 1. Measure combat peak energy only when SfxPlayer (2) or SfxCombat (3) received audio this block.
    f32 combatPeak = 0.0f;
    for (u32 b : {static_cast<u32>(AudioBusId::SfxPlayer), static_cast<u32>(AudioBusId::SfxCombat)}) {
        if ((dirtyBusMask & (1u << b)) == 0u) {
            continue;
        }
        const f32* bL = &m_state->busScratchL[b * kMixBlockFrames];
        const f32* bR = &m_state->busScratchR[b * kMixBlockFrames];
        for (u32 i = 0; i < kMixBlockFrames; ++i) {
            combatPeak = std::max(combatPeak, std::max(std::abs(bL[i]), std::abs(bR[i])));
        }
    }
    const f32 envTarget = combatPeak > 0.35f ? (combatPeak - 0.35f) : 0.0f;
    const f32 envRate   = envTarget > busDsp.sidechainEnvelope ? 0.35f : 0.03f;
    busDsp.sidechainEnvelope += (envTarget - busDsp.sidechainEnvelope) * envRate;
    const f32 duckGain = std::clamp(1.0f - (busDsp.sidechainEnvelope * 0.65f), 0.55f, 1.0f);

    // 2. Process Sub-Buses 1..7 and sum into Master Bus 0 (skipping completely idle buses in O(1)).
    f32* masterL = nullptr;
    f32* masterR = nullptr;

    for (usize b = 1; b < kAudioBusCount; ++b) {
        f32 targetGain = static_cast<f32>(snapshot.busTargetGainQ14[b]) * kQ14ToFloat;
        if (b == static_cast<usize>(AudioBusId::Music) || b == static_cast<usize>(AudioBusId::Ambience)) {
            targetGain *= duckGain;
        }

        const bool busDirty = (dirtyBusMask & (1u << b)) != 0u;
        const bool hasTail  = (std::abs(busDsp.svfIc1eqL[b]) + std::abs(busDsp.svfIc2eqL[b]) +
                               std::abs(busDsp.svfIc1eqR[b]) + std::abs(busDsp.svfIc2eqR[b])) > 1.0e-6f;
        if (!busDirty && !hasTail) {
            busDsp.curGain[b] = targetGain;
            continue;
        }

        EnsureBusInitialized(*m_state, static_cast<u8>(AudioBusId::Master), dirtyBusMask, masterL, masterR);

        f32 curGain       = busDsp.curGain[b];
        const f32 gainStp = (targetGain - curGain) * kInvBlockFrames;

        const FilterMode mode = static_cast<FilterMode>((snapshot.busFilterModesBits >> (b * 2u)) & 0x03u);
        const f32 g           = static_cast<f32>(snapshot.busSvfG_Q14[b]) * kQ14ToFloat;
        const bool applySvf   = (mode != FilterMode::Bypass && g > 0.001f);

        if (!applySvf) {
            if (busDirty) {
                const f32* bL = &m_state->busScratchL[b * kMixBlockFrames];
                const f32* bR = &m_state->busScratchR[b * kMixBlockFrames];
                for (u32 i = 0; i < kMixBlockFrames; ++i) {
                    masterL[i] += bL[i] * curGain;
                    masterR[i] += bR[i] * curGain;
                    curGain    += gainStp;
                }
            } else {
                curGain = targetGain;
            }
            busDsp.svfIc1eqL[b] = busDsp.svfIc2eqL[b] = busDsp.svfIc1eqR[b] = busDsp.svfIc2eqR[b] = 0.0f;
            busDsp.curGain[b]   = curGain;
            continue;
        }

        const f32 a1 = 1.0f / (1.0f + g * (g + kSvfResonanceK));
        const f32 a2 = g * a1;
        const f32 a3 = g * a2;
        f32 ic1L     = busDsp.svfIc1eqL[b];
        f32 ic2L     = busDsp.svfIc2eqL[b];
        f32 ic1R     = busDsp.svfIc1eqR[b];
        f32 ic2R     = busDsp.svfIc2eqR[b];

        const f32* bL = busDirty ? &m_state->busScratchL[b * kMixBlockFrames] : nullptr;
        const f32* bR = busDirty ? &m_state->busScratchR[b * kMixBlockFrames] : nullptr;
        for (u32 i = 0; i < kMixBlockFrames; ++i) {
            f32 sL  = bL ? bL[i] : 0.0f;
            f32 sR  = bR ? bR[i] : 0.0f;
            f32 v1L = 0.0f, v2L = 0.0f, v1R = 0.0f, v2R = 0.0f;
            StepSvfSample(sL, a1, a2, a3, ic1L, ic2L, v1L, v2L);
            StepSvfSample(sR, a1, a2, a3, ic1R, ic2R, v1R, v2R);

            if (mode == FilterMode::LowPass) {
                sL = v2L;
                sR = v2R;
            } else if (mode == FilterMode::BandPass) {
                sL = v1L;
                sR = v1R;
            } else {
                sL = sL - kSvfResonanceK * v1L - v2L;
                sR = sR - kSvfResonanceK * v1R - v2R;
            }
            masterL[i] += sL * curGain;
            masterR[i] += sR * curGain;
            curGain    += gainStp;
        }

        busDsp.curGain[b]   = curGain;
        busDsp.svfIc1eqL[b] = ic1L;
        busDsp.svfIc2eqL[b] = ic2L;
        busDsp.svfIc1eqR[b] = ic1R;
        busDsp.svfIc2eqR[b] = ic2R;
    }

    // 3. Master bus gain ramp, focus mute fade, Padé soft-clipper, and final stereo interleave.
    const f32 masterTarget = snapshot.masterMuteFlag != 0u
                                 ? 0.0f
                                 : (static_cast<f32>(snapshot.busTargetGainQ14[0]) * kQ14ToFloat);

    if ((dirtyBusMask & (1u << static_cast<u8>(AudioBusId::Master))) == 0u) {
        std::memset(outStereo.data(), 0, outStereo.size_bytes());
        busDsp.curGain[0]       = masterTarget;
        m_telemetry.masterPeakL = 0.0f;
        m_telemetry.masterPeakR = 0.0f;
        return;
    }

    f32 masterGain       = busDsp.curGain[0];
    const f32 masterStep = (masterTarget - masterGain) * kInvBlockFrames;
    f32 peakL            = 0.0f;
    f32 peakR            = 0.0f;

    for (u32 i = 0; i < kMixBlockFrames; ++i) {
        const f32 outL         = SoftClipPade(masterL[i] * masterGain);
        const f32 outR         = SoftClipPade(masterR[i] * masterGain);
        outStereo[i * 2u]      = outL;
        outStereo[i * 2u + 1u] = outR;
        peakL                  = std::max(peakL, std::abs(outL));
        peakR                  = std::max(peakR, std::abs(outR));
        masterGain            += masterStep;
    }

    busDsp.curGain[0]       = masterGain;
    m_telemetry.masterPeakL = peakL;
    m_telemetry.masterPeakR = peakR;
}

void AudioMixer::MixBlock(
    const AudioFrameSnapshot& snapshot,
    std::span<f32, kMixBlockFrames * 2> outInterleavedStereo) noexcept {
    if (!m_state) {
        std::memset(outInterleavedStereo.data(), 0, outInterleavedStereo.size_bytes());
        return;
    }

    u32 scopeMask   = 0u;
    u8 dirtyBusMask = std::exchange(m_pendingStealBusMask, 0u);
    dirtyBusMask    = MixActiveVoicesPlanar(snapshot, dirtyBusMask, scopeMask);
    dirtyBusMask    = MixMusicDecksPlanar(snapshot, dirtyBusMask, scopeMask);
    ProcessBusesAndMaster(snapshot, dirtyBusMask, outInterleavedStereo);

    m_telemetry.activeVoiceMask    = m_state->voices.activeMask;
    m_telemetry.activeScopePinMask = scopeMask;
    m_sampleCounter               += kMixBlockFrames;
}

u64 AudioMixer::ComputeAuditHash() const noexcept {
    if (!m_state) {
        return 0ULL;
    }
    const auto& voices = m_state->voices;
    const auto& decks  = m_state->decks;
    u64 h              = MixSplitMix64(voices.activeMask ^ m_sampleCounter);
    for (u64 m = voices.activeMask; m != 0ULL; m &= (m - 1ULL)) {
        const u32 slot = static_cast<u32>(std::countr_zero(m));
        h ^= MixSplitMix64(voices.phaseQ32[slot] ^ (static_cast<u64>(voices.generation[slot]) << 16u));
    }
    h ^= MixSplitMix64(static_cast<u64>(decks[0].cursorFrame) | (static_cast<u64>(decks[1].cursorFrame) << 32u));
    return h;
}

} // namespace en::audio
