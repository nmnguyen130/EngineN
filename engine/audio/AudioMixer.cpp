#include "audio/AudioMixer.h"
#include <algorithm>
#include <cmath>

namespace en {

bool AudioDspAndMixer::Init(AlignedArena& arena) noexcept {
    m_voices = arena.Carve<VoiceSlot>(kMaxAudioVoices);
    m_reverbCombL = arena.Carve<f32>(2048);
    m_reverbCombR = arena.Carve<f32>(2048);
    if (m_voices.empty() || m_reverbCombL.empty() || m_reverbCombR.empty()) return false;
    Reset();
    return true;
}

void AudioDspAndMixer::Reset() noexcept {
    for (VoiceSlot& v : m_voices) {
        const u16 gen = v.generation == 0 ? 1U : v.generation;
        v = VoiceSlot{};
        v.generation = gen;
    }
    for (f32& s : m_reverbCombL) s = 0.0f;
    for (f32& s : m_reverbCombR) s = 0.0f;
    m_reverbCursor = 0;
    m_compEnvelope = 0.0f;
    m_telemetry = AudioTelemetrySnapshot{};
}

VoiceHandle AudioDspAndMixer::SpawnVoice(
    const SoundCueDef& def, const AudioRecord& clip, Vec2 worldPos,
    u32 emitterId, ScopeId scope) noexcept {
    if (!clip.IsValid()) return {};

    // Replace existing voice on same entity if OneVoicePerEntity is set.
    u16 chosenIdx = 0xFFFFU;
    if (HasFlag(def.flags, VoiceFlags::OneVoicePerEntity) && emitterId != kNullAudioEmitter) {
        for (u16 i = 0; i < kMaxAudioVoices; ++i) {
            if (m_voices[i].active && m_voices[i].emitterId == emitterId) {
                chosenIdx = i;
                break;
            }
        }
    }

    if (chosenIdx == 0xFFFFU) {
        for (u16 i = 0; i < kMaxAudioVoices; ++i) {
            if (!m_voices[i].active) {
                chosenIdx = i;
                break;
            }
        }
    }

    if (chosenIdx == 0xFFFFU) {
        if (def.stealPolicy == StealPolicy::Reject) return {};
        f32 quietest = 1e30f;
        chosenIdx = 0;
        for (u16 i = 0; i < kMaxAudioVoices; ++i) {
            const f32 energy = m_voices[i].curGainL + m_voices[i].curGainR;
            if (energy < quietest) {
                quietest = energy;
                chosenIdx = i;
            }
        }
        ++m_telemetry.stolenVoices;
    }

    VoiceSlot& slot = m_voices[chosenIdx];
    const u16 nextGen = static_cast<u16>(slot.generation + 1U == 0 ? 1U : slot.generation + 1U);
    slot.pcm = clip.pcm;
    slot.frameCount = clip.frameCount;
    slot.playhead = 0.0;
    slot.pitchStep = std::clamp(def.basePitch, 0.25f, 4.0f);
    slot.curGainL = def.baseGain * 0.7071f;
    slot.curGainR = def.baseGain * 0.7071f;
    slot.svfIc1EqL = 0.0f;
    slot.svfIc2EqL = 0.0f;
    slot.cutoffRatio = 0.45f;
    slot.emitterId = emitterId;
    slot.worldPos = worldPos;
    slot.maxDist = def.maxDistance;
    slot.baseGain = def.baseGain;
    slot.bus = def.bus;
    slot.channel = def.channel;
    slot.flags = def.flags;
    slot.scopeId = scope;
    slot.generation = nextGen;
    slot.active = true;
    return VoiceHandle{chosenIdx, nextGen};
}

void AudioDspAndMixer::StopVoice(VoiceHandle h) noexcept {
    if (h.index < kMaxAudioVoices && m_voices[h.index].generation == h.generation) {
        m_voices[h.index].active = false;
        ++m_voices[h.index].generation;
    }
}

void AudioDspAndMixer::MixBlock(
    const AudioFrameSnapshot& snapshot,
    std::span<f32, kAudioMixBlockFrames * 2> outStereo) noexcept {
    for (f32& s : outStereo) s = 0.0f;
    std::array<f32, kAudioMixBlockFrames> combatEnergy{};

    u16 activeCount = 0;
    constexpr f32 kInvBlock = 1.0f / static_cast<f32>(kAudioMixBlockFrames);

    for (u16 vIdx = 0; vIdx < kMaxAudioVoices; ++vIdx) {
        VoiceSlot& v = m_voices[vIdx];
        if (!v.active || !v.pcm || v.frameCount == 0) continue;
        if ((snapshot.frozenVoiceMask & (1ULL << vIdx)) != 0ULL) {
            ++activeCount;
            continue;
        }

        const f32 busGain = snapshot.busGains[static_cast<u8>(v.bus) & 7U];
        const f32 targetL = snapshot.targetGainL[vIdx] * busGain;
        const f32 targetR = snapshot.targetGainR[vIdx] * busGain;
        const f32 stepL = (targetL - v.curGainL) * kInvBlock;
        const f32 stepR = (targetR - v.curGainR) * kInvBlock;

        // Cytomic SVF low-pass coefficients for smooth occlusion/bus filtering.
        const f32 g = std::tan(kPi * std::clamp(v.cutoffRatio, 0.01f, 0.48f));
        const f32 k = 1.0f;
        const f32 a1 = 1.0f / (1.0f + g * (g + k));
        const f32 a2 = g * a1;

        for (u32 f = 0; f < kAudioMixBlockFrames; ++f) {
            const u32 idx0 = static_cast<u32>(v.playhead);
            if (idx0 >= v.frameCount) {
                if (HasFlag(v.flags, VoiceFlags::Looping)) {
                    v.playhead = std::fmod(v.playhead, static_cast<f64>(v.frameCount));
                } else {
                    v.active = false;
                    break;
                }
            }
            const u32 i0 = static_cast<u32>(v.playhead) % v.frameCount;
            const u32 im1 = (i0 > 0) ? (i0 - 1U) : 0U;
            const u32 i1 = (i0 + 1U < v.frameCount) ? (i0 + 1U) : i0;
            const u32 i2 = (i0 + 2U < v.frameCount) ? (i0 + 2U) : i1;
            const f32 frac = static_cast<f32>(v.playhead - std::floor(v.playhead));

            f32 sample = SampleHermite4(v.pcm[im1], v.pcm[i0], v.pcm[i1], v.pcm[i2], frac);
            if (v.cutoffRatio < 0.44f) {
                const f32 v3 = sample - v.svfIc2EqL;
                const f32 v1 = a1 * v.svfIc1EqL + a2 * v3;
                const f32 v2 = v.svfIc2EqL + a2 * v.svfIc1EqL + g * a2 * v3;
                v.svfIc1EqL = 2.0f * v1 - v.svfIc1EqL;
                v.svfIc2EqL = 2.0f * v2 - v.svfIc2EqL;
                sample = v2;
            }

            v.curGainL += stepL;
            v.curGainR += stepR;
            outStereo[f * 2U + 0] += sample * v.curGainL;
            outStereo[f * 2U + 1] += sample * v.curGainR;
            if (v.bus == AudioBusId::SfxCombat) {
                combatEnergy[f] += std::abs(sample) * (v.curGainL + v.curGainR);
            }
            v.playhead += static_cast<f64>(v.pitchStep);
        }
        if (v.active) ++activeCount;
    }

    // Apply optional Schroeder comb reverb and sidechain compression + master soft clipper.
    const f32 masterGain = snapshot.busGains[static_cast<u8>(AudioBusId::Master)];
    f32 peakL = 0.0f;
    f32 peakR = 0.0f;
    const u32 revLen = static_cast<u32>(m_reverbCombL.size());

    for (u32 f = 0; f < kAudioMixBlockFrames; ++f) {
        const f32 sc = combatEnergy[f];
        if (sc > m_compEnvelope) {
            m_compEnvelope += m_compCfg.attackCoeff * (sc - m_compEnvelope);
        } else {
            m_compEnvelope += m_compCfg.releaseCoeff * (sc - m_compEnvelope);
        }
        f32 compGain = 1.0f;
        if (m_compEnvelope > m_compCfg.thresholdLinear) {
            const f32 excess = m_compEnvelope - m_compCfg.thresholdLinear;
            compGain = 1.0f / (1.0f + excess * (m_compCfg.ratio - 1.0f));
        }

        f32 l = outStereo[f * 2U + 0] * compGain * masterGain;
        f32 r = outStereo[f * 2U + 1] * compGain * masterGain;

        if (m_reverbCfg.enabled && revLen > 0) {
            const f32 delayedL = m_reverbCombL[m_reverbCursor];
            const f32 delayedR = m_reverbCombR[m_reverbCursor];
            m_reverbCombL[m_reverbCursor] = l + delayedL * m_reverbCfg.roomSize * (1.0f - m_reverbCfg.damping);
            m_reverbCombR[m_reverbCursor] = r + delayedR * m_reverbCfg.roomSize * (1.0f - m_reverbCfg.damping);
            m_reverbCursor = (m_reverbCursor + 1U) % revLen;
            l += delayedL * m_reverbCfg.wetMix;
            r += delayedR * m_reverbCfg.wetMix;
        }

        l = PadeSoftClip(l);
        r = PadeSoftClip(r);
        outStereo[f * 2U + 0] = l;
        outStereo[f * 2U + 1] = r;
        peakL = std::max(peakL, std::abs(l));
        peakR = std::max(peakR, std::abs(r));
    }

    m_telemetry.peakLeft = peakL;
    m_telemetry.peakRight = peakR;
    m_telemetry.activeVoices = activeCount;
    ++m_telemetry.mixedBlocks;
}

u64 AudioDspAndMixer::ComputeAuditHash() const noexcept {
    u64 h = Mix64(static_cast<u64>(m_telemetry.activeVoices) |
                  (static_cast<u64>(m_telemetry.mixedBlocks) << 16U));
    for (const VoiceSlot& v : m_voices) {
        if (v.active) h = HashCombine64(h, v.generation);
    }
    return h;
}

} // namespace en
