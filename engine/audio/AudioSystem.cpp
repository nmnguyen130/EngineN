#include "audio/AudioSystem.h"
#include "assets/AssetBank.h"

#include <cstring>

namespace en {

namespace {

inline constexpr u32 kMaxCueDefs = 256U;
inline constexpr u32 kMaxMusicTrackDefs = 32U;
inline constexpr u32 kMaxCmdRing = 256U;

[[nodiscard]] constexpr f32 EvaluateAttenuation(
    AttenuationCurve curve,
    f32 dist,
    f32 minDist,
    f32 maxDist) noexcept {
    if (curve == AttenuationCurve::None || dist <= minDist) return 1.0f;
    if (dist >= maxDist || maxDist <= minDist) return 0.0f;
    const f32 t = (dist - minDist) / (maxDist - minDist);
    switch (curve) {
        case AttenuationCurve::None:
            return 1.0f;
        case AttenuationCurve::Linear:
            return 1.0f - t;
        case AttenuationCurve::InverseClamped:
            return minDist / (minDist + 4.0f * (dist - minDist));
        case AttenuationCurve::Smoothstep:
            return 1.0f - (t * t * (3.0f - 2.0f * t));
    }
    return 1.0f - t;
}

[[nodiscard]] constexpr i16 QuantizeVelQ4(f32 velMetersPerSec) noexcept {
    return static_cast<i16>(std::clamp(velMetersPerSec * 16.0f, -32767.0f, 32767.0f));
}

} // namespace

struct alignas(64) AudioSystem::AudioBridgeState {
    AudioFrameSnapshot snapshot{};
    std::array<AudioCommand, kMaxCmdRing> cmdRing{};
    std::array<SoundCueDef, kMaxCueDefs> cueDefs{};
    std::array<u8, kMaxCueDefs> cueScopes{};
    std::array<u16, kMaxCueDefs> cueLastSpawnTick{};
    std::array<u8, kMaxCueDefs> cueLastVoiceSlot{};
    std::array<MusicTrackDef, kMaxMusicTrackDefs> musicDefs{};
    std::array<u8, kMaxMusicTrackDefs> musicScopes{};
    std::array<MainVoiceTracker, kMaxAudioVoices> trackers{};
    AudioSystemConfig cfg{};
    f32 invPanWidth{1.0f / 16.0f};
    f32 invSpeedOfSound{1.0f / 343.0f};
    f32 listenerVx{0.0f};
    f32 listenerVy{0.0f};
    u64 activeMainVoicesMask{0ULL};
    u64 prngState{0x853C49E6748FEA9BULL};
    u32 cmdWriteIdx{0U};
    u32 cmdReadIdx{0U};
    u16 cueDefCount{0U};
    u16 musicDefCount{0U};
    u16 nextGeneration{1U};
    u16 coalescedCount{0U};
};

AudioSystem::AudioSystem(AudioSystem&& other) noexcept
    : m_mixer(other.m_mixer),
      m_arena(std::move(other.m_arena)),
      m_bridge(other.m_bridge),
      m_spatialActiveMask(other.m_spatialActiveMask),
      m_listenerX(other.m_listenerX),
      m_listenerY(other.m_listenerY),
      m_pendingReleaseScopeMask(other.m_pendingReleaseScopeMask),
      m_currentTick(other.m_currentTick),
      m_focusMuted(other.m_focusMuted),
      m_initialized(other.m_initialized) {
    other.m_bridge = nullptr;
    other.m_initialized = 0U;
}

AudioSystem& AudioSystem::operator=(AudioSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_mixer = other.m_mixer;
        m_arena = std::move(other.m_arena);
        m_bridge = other.m_bridge;
        m_spatialActiveMask = other.m_spatialActiveMask;
        m_listenerX = other.m_listenerX;
        m_listenerY = other.m_listenerY;
        m_pendingReleaseScopeMask = other.m_pendingReleaseScopeMask;
        m_currentTick = other.m_currentTick;
        m_focusMuted = other.m_focusMuted;
        m_initialized = other.m_initialized;

        other.m_bridge = nullptr;
        other.m_initialized = 0U;
    }
    return *this;
}

Result<void, AudioErrc> AudioSystem::Init(const AudioSystemConfig& cfg) noexcept {
    Shutdown();
    if (cfg.panWidthMeters <= 0.1f || cfg.speedOfSoundMetersPerSec <= 1.0f) {
        return Err(AudioErrc::InvalidConfig);
    }

    const usize required = AudioMixer::RequiredArenaBytes() + sizeof(AudioBridgeState) + 256U;
    const usize totalBytes = std::max<usize>(cfg.arenaBytes, required);
    if (!m_arena.Init(totalBytes)) {
        return Err(AudioErrc::OutOfMemory);
    }

    auto mixerSlice = m_arena.Carve<u8>(AudioMixer::RequiredArenaBytes());
    auto bridgeSlice = m_arena.Carve<AudioBridgeState>(1U);
    if (mixerSlice.empty() || bridgeSlice.empty()) {
        Shutdown();
        return Err(AudioErrc::ArenaExhausted);
    }

    if (auto r = m_mixer.Init(mixerSlice); !r) {
        Shutdown();
        return r;
    }

    m_bridge = bridgeSlice.data();
    *m_bridge = AudioBridgeState{};
    m_bridge->cfg = cfg;
    m_bridge->invPanWidth = 1.0f / cfg.panWidthMeters;
    m_bridge->invSpeedOfSound = 1.0f / cfg.speedOfSoundMetersPerSec;
    m_bridge->prngState = 0x853C49E6748FEA9BULL;
    m_bridge->nextGeneration = 1U;
    m_bridge->cueLastSpawnTick.fill(0xFFFFU);
    m_bridge->snapshot.voiceCutoffHz.fill(20000U);
    m_bridge->snapshot.targetPitch_q12.fill(4096U);

    m_spatialActiveMask = 0ULL;
    m_listenerX = 0.0f;
    m_listenerY = 0.0f;
    m_pendingReleaseScopeMask = 0U;
    m_currentTick = 0U;
    m_focusMuted = 0U;
    m_initialized = 1U;
    return {};
}

void AudioSystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_mixer = AudioMixer{};
    m_bridge = nullptr;
    m_spatialActiveMask = 0ULL;
    m_listenerX = 0.0f;
    m_listenerY = 0.0f;
    m_pendingReleaseScopeMask = 0U;
    m_currentTick = 0U;
    m_focusMuted = 0U;
    m_initialized = 0U;
}

Result<void, AudioErrc> AudioSystem::RegisterCueDef(const SoundCueDef& def, u8 owningScope) noexcept {
    if (!m_bridge || !def.id.IsValid()) return Err(AudioErrc::InvalidConfig);
    for (u16 i = 0U; i < m_bridge->cueDefCount; ++i) {
        if (m_bridge->cueDefs[i].id == def.id) {
            m_bridge->cueDefs[i] = def;
            m_bridge->cueScopes[i] = owningScope;
            return {};
        }
    }
    if (m_bridge->cueDefCount >= kMaxCueDefs) {
        return Err(AudioErrc::CueTableFull);
    }
    const u16 idx = m_bridge->cueDefCount++;
    m_bridge->cueDefs[idx] = def;
    m_bridge->cueScopes[idx] = owningScope;
    return {};
}

Result<void, AudioErrc> AudioSystem::RegisterMusicTrackDef(const MusicTrackDef& def, u8 owningScope) noexcept {
    if (!m_bridge || def.trackId == 0U) return Err(AudioErrc::InvalidConfig);
    for (u16 i = 0U; i < m_bridge->musicDefCount; ++i) {
        if (m_bridge->musicDefs[i].trackId == def.trackId) {
            m_bridge->musicDefs[i] = def;
            m_bridge->musicScopes[i] = owningScope;
            return {};
        }
    }
    if (m_bridge->musicDefCount >= kMaxMusicTrackDefs) {
        return Err(AudioErrc::CueTableFull);
    }
    const u16 idx = m_bridge->musicDefCount++;
    m_bridge->musicDefs[idx] = def;
    m_bridge->musicScopes[idx] = owningScope;
    return {};
}

Result<void, AudioErrc> AudioSystem::RegisterCueBank(const BlobRecord& cueBlob, u8 owningScope) noexcept {
    if (!m_bridge || !cueBlob.IsValid()) return Err(AudioErrc::CorruptedBankBlob);
    const std::span<const u8> bytes = cueBlob.Bytes();
    if (bytes.size() < sizeof(CookedAudioBankHeader)) {
        return Err(AudioErrc::CorruptedBankBlob);
    }

    CookedAudioBankHeader hdr{};
    std::memcpy(&hdr, bytes.data(), sizeof(CookedAudioBankHeader));
    const usize cuesBytes = static_cast<usize>(hdr.cueCount) * sizeof(SoundCueDef);
    const usize tracksBytes = static_cast<usize>(hdr.musicTrackCount) * sizeof(MusicTrackDef);
    if (hdr.magic != CookedAudioBankHeader::kMagic ||
        hdr.version != CookedAudioBankHeader::kVersion ||
        bytes.size() < sizeof(CookedAudioBankHeader) + cuesBytes + tracksBytes) {
        return Err(AudioErrc::CorruptedBankBlob);
    }

    const u8* ptr = bytes.data() + sizeof(CookedAudioBankHeader);
    for (u16 i = 0U; i < hdr.cueCount; ++i) {
        SoundCueDef def{};
        std::memcpy(&def, ptr + i * sizeof(SoundCueDef), sizeof(SoundCueDef));
        if (auto r = RegisterCueDef(def, owningScope); !r) return r;
    }

    ptr += cuesBytes;
    for (u16 i = 0U; i < hdr.musicTrackCount; ++i) {
        MusicTrackDef track{};
        std::memcpy(&track, ptr + i * sizeof(MusicTrackDef), sizeof(MusicTrackDef));
        if (auto r = RegisterMusicTrackDef(track, owningScope); !r) return r;
    }
    return {};
}

u32 AudioSystem::PollReleasableScopeMask() noexcept {
    if (!m_bridge || m_pendingReleaseScopeMask == 0U) return 0U;
    DrainCommandsToMixer();
    const u32 activeScopes = m_mixer.ActiveAssetScopesMask();
    const u32 releasable = m_pendingReleaseScopeMask & ~activeScopes;
    m_pendingReleaseScopeMask &= ~releasable;
    return releasable;
}

void AudioSystem::EnqueueCommand(const AudioCommand& cmd) noexcept {
    if (!m_bridge) return;
    const u32 nextWrite = (m_bridge->cmdWriteIdx + 1U) & (kMaxCmdRing - 1U);
    if (nextWrite == m_bridge->cmdReadIdx) {
        m_mixer.ApplyCommand(m_bridge->cmdRing[m_bridge->cmdReadIdx]);
        m_bridge->cmdReadIdx = (m_bridge->cmdReadIdx + 1U) & (kMaxCmdRing - 1U);
    }
    m_bridge->cmdRing[m_bridge->cmdWriteIdx] = cmd;
    m_bridge->cmdWriteIdx = nextWrite;
}

void AudioSystem::DrainCommandsToMixer() noexcept {
    if (!m_bridge) return;
    while (m_bridge->cmdReadIdx != m_bridge->cmdWriteIdx) {
        m_mixer.ApplyCommand(m_bridge->cmdRing[m_bridge->cmdReadIdx]);
        m_bridge->cmdReadIdx = (m_bridge->cmdReadIdx + 1U) & (kMaxCmdRing - 1U);
    }
    m_bridge->activeMainVoicesMask &= m_mixer.ActiveVoiceMask();
    m_spatialActiveMask &= m_bridge->activeMainVoicesMask;
}

void AudioSystem::ComputeSpatialVoiceParams(
    u8 slot,
    const SoundCueDef& def,
    f32 listenerX,
    f32 listenerY,
    f32 listenerVx,
    f32 listenerVy,
    f32 dopplerScale,
    u16& outGainL_q14,
    u16& outGainR_q14,
    u16& outPitch_q12) const noexcept {
    const MainVoiceTracker& tr = m_bridge->trackers[slot];
    const f32 instGain = static_cast<f32>(tr.instanceGain_q14) * (1.0f / 16384.0f);
    f32 instPitch = static_cast<f32>(tr.instancePitch_q12) * (1.0f / 4096.0f);

    if (!HasVoiceFlag(def.flags, VoiceFlags::Spatialized2D)) {
        const u16 g = static_cast<u16>(std::clamp(instGain * 0.70710678f, 0.0f, 4.0f) * 16384.0f + 0.5f);
        outGainL_q14 = g;
        outGainR_q14 = g;
        outPitch_q12 = static_cast<u16>(std::clamp(instPitch, 0.125f, 8.0f) * 4096.0f + 0.5f);
        return;
    }

    const f32 dx = tr.worldX - listenerX;
    const f32 dy = tr.worldY - listenerY;
    const f32 dist = std::sqrt(dx * dx + dy * dy);
    const f32 atten = EvaluateAttenuation(def.Attenuation(), dist, def.minDist, def.maxDist);

    // Near-field spatial spread: smoothly collapses stereo pan to center inside minDist to prevent speaker flip
    const f32 nearFieldSpread = (def.minDist > 1e-3f) ? std::min(1.0f, dist / def.minDist) : 1.0f;
    const f32 pan = std::clamp(dx * m_bridge->invPanWidth * nearFieldSpread, -1.0f, 1.0f);
    const f32 angle = 0.25f * kPi * (pan + 1.0f);
    f32 panL = 0.70710678f;
    f32 panR = 0.70710678f;
    FastSinCos(angle, panL, panR);

    const f32 gainL = instGain * atten * panL;
    const f32 gainR = instGain * atten * panR;
    outGainL_q14 = static_cast<u16>(std::clamp(gainL, 0.0f, 4.0f) * 16384.0f + 0.5f);
    outGainR_q14 = static_cast<u16>(std::clamp(gainR, 0.0f, 4.0f) * 16384.0f + 0.5f);

    if (HasVoiceFlag(def.flags, VoiceFlags::EnableDoppler) && dist > 1e-3f && dopplerScale > 0.0f) {
        const f32 invDist = 1.0f / dist;
        const f32 dirX = dx * invDist;
        const f32 dirY = dy * invDist;
        const f32 emVx = static_cast<f32>(tr.emitterVx_q4) * (1.0f / 16.0f);
        const f32 emVy = static_cast<f32>(tr.emitterVy_q4) * (1.0f / 16.0f);
        const f32 relRadialVel = (listenerVx - emVx) * dirX + (listenerVy - emVy) * dirY;
        const f32 dopplerFactor = std::clamp(1.0f + relRadialVel * m_bridge->invSpeedOfSound * dopplerScale, 0.5f, 2.0f);
        instPitch *= dopplerFactor;
    }
    outPitch_q12 = static_cast<u16>(std::clamp(instPitch, 0.125f, 8.0f) * 4096.0f + 0.5f);
}

VoiceHandle AudioSystem::PlayCue(
    SoundCueId cueId,
    const AssetBank& assetBank,
    f32 worldX,
    f32 worldY,
    u32 emitterId,
    f32 emitterVx,
    f32 emitterVy) noexcept {
    if (!m_bridge || !cueId.IsValid()) return kNullVoiceHandle;

    u16 cueIdx = 0xFFFFU;
    for (u16 i = 0U; i < m_bridge->cueDefCount; ++i) {
        if (m_bridge->cueDefs[i].id == cueId) {
            cueIdx = i;
            break;
        }
    }
    if (cueIdx == 0xFFFFU) return kNullVoiceHandle;

    const SoundCueDef& def = m_bridge->cueDefs[cueIdx];
    const u8 scope = m_bridge->cueScopes[cueIdx];

    // 1. Cooldown & Energy-Summing Same-Tick Coalescer (persisted in tracker.instanceGain_q14)
    if (m_bridge->cueLastSpawnTick[cueIdx] != 0xFFFFU &&
        static_cast<u16>(m_currentTick - m_bridge->cueLastSpawnTick[cueIdx]) < def.cooldownTicks) {
        const u8 prevSlot = m_bridge->cueLastVoiceSlot[cueIdx] & 63U;
        if ((m_bridge->activeMainVoicesMask & (1ULL << prevSlot)) != 0ULL &&
            m_bridge->trackers[prevSlot].cueId == cueId) {
            MainVoiceTracker& prevTr = m_bridge->trackers[prevSlot];
            prevTr.instanceGain_q14 = static_cast<u16>(
                std::min<u32>(32768U, (static_cast<u32>(prevTr.instanceGain_q14) * 5U) >> 2U));
            const u32 boostedL = std::min<u32>(32768U, (static_cast<u32>(m_bridge->snapshot.targetGainL_q14[prevSlot]) * 5U) >> 2U);
            const u32 boostedR = std::min<u32>(32768U, (static_cast<u32>(m_bridge->snapshot.targetGainR_q14[prevSlot]) * 5U) >> 2U);
            m_bridge->snapshot.targetGainL_q14[prevSlot] = static_cast<u16>(boostedL);
            m_bridge->snapshot.targetGainR_q14[prevSlot] = static_cast<u16>(boostedR);
            ++m_bridge->coalescedCount;
            return VoiceHandle{prevSlot, prevTr.generation};
        }
    }

    // 2. Single-pass voice admission & stealing
    u8 targetSlot = 0xFFU;
    u8 activeForThisCue = 0U;
    u8 stealCandidate = 0xFFU;
    u32 bestStealScore = 0xFFFFFFFFU;
    u8 sameCueStealCandidate = 0xFFU;
    u32 bestSameCueScore = 0xFFFFFFFFU;
    bool reusedEntityVoice = false;

    const u64 freeMask = ~m_bridge->activeMainVoicesMask;
    if (freeMask != 0ULL) {
        targetSlot = static_cast<u8>(std::countr_zero(freeMask));
    }

    u64 activeMask = m_bridge->activeMainVoicesMask;
    while (activeMask != 0ULL) {
        const u8 s = static_cast<u8>(std::countr_zero(activeMask));
        activeMask &= (activeMask - 1ULL);
        const MainVoiceTracker& tr = m_bridge->trackers[s];

        if (HasVoiceFlag(def.flags, VoiceFlags::OneVoicePerEntity) &&
            emitterId != kNullAudioEmitter && tr.emitterId == emitterId && tr.cueId == cueId) {
            targetSlot = s;
            reusedEntityVoice = true;
            break;
        }
        const u32 score = (def.Steal() == StealPolicy::StealOldest)
                              ? static_cast<u32>(tr.spawnTick)
                              : static_cast<u32>(m_bridge->snapshot.targetGainL_q14[s]) +
                                static_cast<u32>(m_bridge->snapshot.targetGainR_q14[s]);
        if (tr.cueId == cueId) {
            ++activeForThisCue;
            if (score < bestSameCueScore) {
                bestSameCueScore = score;
                sameCueStealCandidate = s;
            }
        }
        if (tr.priority <= def.priority && score < bestStealScore) {
            bestStealScore = score;
            stealCandidate = s;
        }
    }

    if (!reusedEntityVoice) {
        if (def.maxConcurrency > 0U && activeForThisCue >= def.maxConcurrency) {
            if (def.Steal() == StealPolicy::Reject || sameCueStealCandidate == 0xFFU) {
                return kNullVoiceHandle;
            }
            targetSlot = sameCueStealCandidate;
        } else if (targetSlot == 0xFFU) {
            if (def.Steal() == StealPolicy::Reject || stealCandidate == 0xFFU) {
                return kNullVoiceHandle;
            }
            targetSlot = stealCandidate;
        }
    }

    // 3. Resolve PCM AudioRecord & sample randomized gain/pitch jitter
    m_bridge->prngState ^= m_bridge->prngState >> 12U;
    m_bridge->prngState ^= m_bridge->prngState << 25U;
    m_bridge->prngState ^= m_bridge->prngState >> 27U;
    const u64 randVal = m_bridge->prngState * 0x2545F4914F6CDD1DULL;

    AudioHandle chosenClip = def.clipVariants[0];
    if (def.clipVariants[1].IsValid() && (randVal & 1ULL) != 0ULL) {
        chosenClip = def.clipVariants[1];
    }
    const AudioRecord& audioRec = assetBank.GetAudio(chosenClip);
    if (!audioRec.pcm || audioRec.frameCount == 0U) return kNullVoiceHandle;

    const f32 bipolarA = (static_cast<f32>((randVal >> 8U) & 0xFFFFU) * (1.0f / 32767.5f)) - 1.0f;
    const f32 bipolarB = (static_cast<f32>((randVal >> 24U) & 0xFFFFU) * (1.0f / 32767.5f)) - 1.0f;
    const f32 randomizedGain = std::clamp(def.BaseGain() * (1.0f + bipolarA * def.GainJitter()), 0.0f, 4.0f);
    const f32 randomizedPitch = std::clamp(def.BasePitch() * (1.0f + bipolarB * def.PitchJitter()), 0.125f, 8.0f);

    u16 gen = m_bridge->nextGeneration++;
    if (gen == 0U) gen = m_bridge->nextGeneration++;

    MainVoiceTracker& tr = m_bridge->trackers[targetSlot];
    tr.cueId = cueId;
    tr.emitterId = emitterId;
    tr.worldX = worldX;
    tr.worldY = worldY;
    tr.emitterVx_q4 = QuantizeVelQ4(emitterVx);
    tr.emitterVy_q4 = QuantizeVelQ4(emitterVy);
    tr.instanceGain_q14 = static_cast<u16>(randomizedGain * 16384.0f + 0.5f);
    tr.instancePitch_q12 = static_cast<u16>(randomizedPitch * 4096.0f + 0.5f);
    tr.spawnTick = m_currentTick;
    tr.generation = gen;
    tr.cueTableSlot = static_cast<u8>(cueIdx);
    tr.assetScope = scope;
    tr.priority = def.priority;
    tr.occluded = 0U;

    m_bridge->activeMainVoicesMask |= (1ULL << targetSlot);
    if (HasVoiceFlag(def.flags, VoiceFlags::Spatialized2D)) {
        m_spatialActiveMask |= (1ULL << targetSlot);
    } else {
        m_spatialActiveMask &= ~(1ULL << targetSlot);
    }

    m_bridge->cueLastSpawnTick[cueIdx] = m_currentTick;
    m_bridge->cueLastVoiceSlot[cueIdx] = targetSlot;

    u16 gainL_q14 = 16384U;
    u16 gainR_q14 = 16384U;
    u16 pitch_q12 = 4096U;
    ComputeSpatialVoiceParams(
        targetSlot, def,
        m_listenerX, m_listenerY,
        m_bridge->listenerVx, m_bridge->listenerVy,
        1.0f, gainL_q14, gainR_q14, pitch_q12);

    m_bridge->snapshot.targetGainL_q14[targetSlot] = gainL_q14;
    m_bridge->snapshot.targetGainR_q14[targetSlot] = gainR_q14;
    m_bridge->snapshot.targetPitch_q12[targetSlot] = pitch_q12;
    m_bridge->snapshot.voiceCutoffHz[targetSlot] = 20000U;

    AudioCommand cmd{};
    cmd.pcmData = audioRec.pcm;
    cmd.frameCount = audioRec.frameCount;
    cmd.loopStartFrame = def.loopStartFrame;
    cmd.sampleRate = static_cast<u16>(std::min<u32>(65535U, audioRec.sampleRate));
    cmd.initialGainL_q14 = gainL_q14;
    cmd.initialGainR_q14 = gainR_q14;
    cmd.initialPitch_q12 = pitch_q12;
    cmd.generation = gen;
    cmd.fadeBlocks = 1U;
    cmd.type = AudioCmdType::SpawnVoice;
    cmd.bus = def.Bus();
    cmd.voiceSlot = targetSlot;
    cmd.flags = def.flags | (audioRec.looping ? VoiceFlags::Looping : VoiceFlags::None);
    cmd.channels = audioRec.channels;
    cmd.assetScope = scope;
    EnqueueCommand(cmd);

    return VoiceHandle{targetSlot, gen};
}

void AudioSystem::SubmitTriggers(
    std::span<const AudioTriggerReq> triggers,
    const AssetBank& assetBank) noexcept {
    for (const AudioTriggerReq& req : triggers) {
        (void)PlayCue(req.cueId, assetBank, req.worldX, req.worldY, req.emitterId);
    }
}

void AudioSystem::StopVoice(VoiceHandle handle, u16 fadeOutBlocks) noexcept {
    if (!m_bridge || !handle.IsValid()) return;
    const u8 slot = static_cast<u8>(handle.index & 63U);
    if (m_bridge->trackers[slot].generation != handle.generation) return;

    AudioCommand cmd{};
    cmd.type = AudioCmdType::StopVoice;
    cmd.voiceSlot = slot;
    cmd.generation = handle.generation;
    cmd.fadeBlocks = fadeOutBlocks;
    EnqueueCommand(cmd);

    if (fadeOutBlocks <= 1U) {
        m_bridge->activeMainVoicesMask &= ~(1ULL << slot);
        m_spatialActiveMask &= ~(1ULL << slot);
    }
}

void AudioSystem::StopAllVoices(u16 fadeOutBlocks) noexcept {
    if (!m_bridge) return;
    u64 mask = m_bridge->activeMainVoicesMask;
    while (mask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        StopVoice(VoiceHandle{slot, m_bridge->trackers[slot].generation}, fadeOutBlocks);
    }
}

void AudioSystem::SetVoicePosition(
    VoiceHandle handle,
    f32 worldX,
    f32 worldY,
    f32 emitterVx,
    f32 emitterVy) noexcept {
    if (!m_bridge || !handle.IsValid()) return;
    const u8 slot = static_cast<u8>(handle.index & 63U);
    MainVoiceTracker& tr = m_bridge->trackers[slot];
    if (tr.generation != handle.generation) return;
    tr.worldX = worldX;
    tr.worldY = worldY;
    tr.emitterVx_q4 = QuantizeVelQ4(emitterVx);
    tr.emitterVy_q4 = QuantizeVelQ4(emitterVy);
}

void AudioSystem::UpdateEmitterTransform(
    u32 emitterId,
    f32 worldX,
    f32 worldY,
    f32 emitterVx,
    f32 emitterVy) noexcept {
    if (!m_bridge || emitterId == kNullAudioEmitter) return;
    const i16 vxQ4 = QuantizeVelQ4(emitterVx);
    const i16 vyQ4 = QuantizeVelQ4(emitterVy);
    u64 mask = m_bridge->activeMainVoicesMask;
    while (mask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        MainVoiceTracker& tr = m_bridge->trackers[slot];
        if (tr.emitterId == emitterId) {
            tr.worldX = worldX;
            tr.worldY = worldY;
            tr.emitterVx_q4 = vxQ4;
            tr.emitterVy_q4 = vyQ4;
        }
    }
}

void AudioSystem::SetVoiceGain(VoiceHandle handle, f32 linearGain) noexcept {
    if (!m_bridge || !handle.IsValid()) return;
    const u8 slot = static_cast<u8>(handle.index & 63U);
    MainVoiceTracker& tr = m_bridge->trackers[slot];
    if (tr.generation != handle.generation) return;
    tr.instanceGain_q14 = static_cast<u16>(std::clamp(linearGain, 0.0f, 4.0f) * 16384.0f + 0.5f);
}

void AudioSystem::SetVoicePitch(VoiceHandle handle, f32 pitchRatio) noexcept {
    if (!m_bridge || !handle.IsValid()) return;
    const u8 slot = static_cast<u8>(handle.index & 63U);
    MainVoiceTracker& tr = m_bridge->trackers[slot];
    if (tr.generation != handle.generation) return;
    tr.instancePitch_q12 = static_cast<u16>(std::clamp(pitchRatio, 0.125f, 8.0f) * 4096.0f + 0.5f);
}

void AudioSystem::SetBusGain(AudioBusId bus, f32 linearGain) noexcept {
    if (!m_bridge) return;
    const u8 idx = static_cast<u8>(bus) & 7U;
    m_bridge->snapshot.busGain_q14[idx] =
        static_cast<u16>(std::clamp(linearGain, 0.0f, 4.0f) * 16384.0f + 0.5f);
}

void AudioSystem::SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept {
    if (!m_bridge) return;
    const u8 idx = static_cast<u8>(bus) & 7U;
    m_bridge->snapshot.busFilterMode[idx] = mode;
    m_bridge->snapshot.busCutoffHz[idx] =
        static_cast<u16>(std::clamp(cutoffHz, 40.0f, 20000.0f) + 0.5f);
}

void AudioSystem::SetReverbParams(
    f32 roomSize,
    f32 damping,
    f32 wetGain,
    u8 busSendMask) noexcept {
    if (!m_bridge) return;
    m_bridge->snapshot.reverbRoomSize_q8 = static_cast<u8>(std::clamp(roomSize, 0.0f, 1.0f) * 255.0f + 0.5f);
    m_bridge->snapshot.reverbDamping_q8 = static_cast<u8>(std::clamp(damping, 0.0f, 1.0f) * 255.0f + 0.5f);
    m_bridge->snapshot.reverbWetGain_q14 = static_cast<u16>(std::clamp(wetGain, 0.0f, 2.0f) * 16384.0f + 0.5f);
    m_bridge->snapshot.reverbBusSendMask = busSendMask;
}

bool AudioSystem::TransitionMusic(
    u32 trackId,
    const AssetBank& assetBank,
    u8 crossfadeBlocks) noexcept {
    if (!m_bridge || trackId == 0U) return false;
    u16 foundIdx = 0xFFFFU;
    for (u16 i = 0U; i < m_bridge->musicDefCount; ++i) {
        if (m_bridge->musicDefs[i].trackId == trackId) {
            foundIdx = i;
            break;
        }
    }
    if (foundIdx == 0xFFFFU) return false;

    const MusicTrackDef& track = m_bridge->musicDefs[foundIdx];
    const u8 scope = m_bridge->musicScopes[foundIdx];
    for (u8 st = 0U; st < kMaxMusicStems; ++st) {
        AudioCommand stemCmd{};
        stemCmd.type = AudioCmdType::BindMusicStem;
        stemCmd.voiceSlot = st;
        stemCmd.assetScope = scope;
        stemCmd.initialGainL_q14 = track.defaultWeights_q14[st];
        m_bridge->snapshot.musicStemWeights_q14[st] = track.defaultWeights_q14[st];

        if (st < track.stemCount && track.stems[st].IsValid()) {
            const AudioRecord& rec = assetBank.GetAudio(track.stems[st]);
            stemCmd.pcmData = rec.pcm;
            stemCmd.frameCount = rec.frameCount;
            stemCmd.sampleRate = static_cast<u16>(std::min<u32>(65535U, rec.sampleRate));
            stemCmd.channels = rec.channels;
        }
        EnqueueCommand(stemCmd);
    }

    AudioCommand commitCmd{};
    commitCmd.type = AudioCmdType::CommitMusicDeck;
    commitCmd.loopStartFrame = track.loopStartFrame;
    commitCmd.fadeBlocks = crossfadeBlocks;
    if (track.loop != 0U) commitCmd.flags = VoiceFlags::Looping;
    EnqueueCommand(commitCmd);
    return true;
}

void AudioSystem::SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept {
    if (!m_bridge) return;
    for (u8 st = 0U; st < kMaxMusicStems; ++st) {
        m_bridge->snapshot.musicStemWeights_q14[st] =
            static_cast<u16>(std::clamp(weights[st], 0.0f, 4.0f) * 16384.0f + 0.5f);
    }
}

void AudioSystem::Update(const AudioUpdateDesc& desc) noexcept {
    if (!m_bridge) return;

    m_currentTick = static_cast<u16>(desc.tickIndex & 0xFFFFU);
    m_bridge->listenerVx = (desc.listenerX - m_listenerX) * static_cast<f32>(kSimHz);
    m_bridge->listenerVy = (desc.listenerY - m_listenerY) * static_cast<f32>(kSimHz);
    m_listenerX = desc.listenerX;
    m_listenerY = desc.listenerY;

    if (desc.focusMuteOverride >= 0) {
        m_focusMuted = (desc.focusMuteOverride > 0) ? 1U : 0U;
    }
    m_bridge->snapshot.masterMuted = m_focusMuted;

    if (desc.assetBank && !desc.triggers.empty()) {
        SubmitTriggers(desc.triggers, *desc.assetBank);
    }

    const f32 dopplerScale = static_cast<f32>(desc.dopplerScale_q8) * (1.0f / 256.0f);
    u64 pausedMask = 0ULL;

    u64 activeMask = m_bridge->activeMainVoicesMask;
    while (activeMask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(activeMask));
        activeMask &= (activeMask - 1ULL);
        MainVoiceTracker& tr = m_bridge->trackers[slot];
        const SoundCueDef& def = m_bridge->cueDefs[tr.cueTableSlot];

        const u8 chIdx = static_cast<u8>(def.Channel()) & 7U;
        if (HasVoiceFlag(def.flags, VoiceFlags::PauseOnFreeze) &&
            (desc.frozenChannelMask & (1U << chIdx)) != 0U) {
            pausedMask |= (1ULL << slot);
            continue;
        }

        u16 gainL = m_bridge->snapshot.targetGainL_q14[slot];
        u16 gainR = m_bridge->snapshot.targetGainR_q14[slot];
        u16 pitch = tr.instancePitch_q12;

        if ((m_spatialActiveMask & (1ULL << slot)) != 0ULL) {
            ComputeSpatialVoiceParams(
                slot, def,
                desc.listenerX, desc.listenerY,
                m_bridge->listenerVx, m_bridge->listenerVy,
                dopplerScale, gainL, gainR, pitch);

            if (desc.occlusionQuery && !HasVoiceFlag(def.flags, VoiceFlags::BypassOcclusion) &&
                ((slot & 3U) == (desc.tickIndex & 3U))) {
                const bool blocked = desc.occlusionQuery(
                    desc.listenerX, desc.listenerY, tr.worldX, tr.worldY, desc.occlusionUserCtx);
                tr.occluded = blocked ? 1U : 0U;
            }
            m_bridge->snapshot.voiceCutoffHz[slot] =
                (tr.occluded != 0U) ? m_bridge->cfg.occludedLowPassHz : 20000U;
        } else {
            const u16 g = static_cast<u16>(std::clamp(static_cast<f32>(tr.instanceGain_q14) * 0.70710678f, 0.0f, 65535.0f));
            gainL = g;
            gainR = g;
        }

        if (HasVoiceFlag(def.flags, VoiceFlags::FollowTimeScale) && desc.channelScales) {
            const f32 scale = std::clamp(desc.channelScales[chIdx], 0.125f, 4.0f);
            pitch = static_cast<u16>(std::clamp(static_cast<f32>(pitch) * scale, 512.0f, 32768.0f));
        }

        m_bridge->snapshot.targetGainL_q14[slot] = gainL;
        m_bridge->snapshot.targetGainR_q14[slot] = gainR;
        m_bridge->snapshot.targetPitch_q12[slot] = pitch;
    }

    m_bridge->snapshot.pausedVoicesMask = pausedMask;
}

void AudioSystem::PumpHeadlessBlocks(
    u32 blockCount,
    std::span<f32> optionalCaptureOut) noexcept {
    if (!m_bridge) return;
    DrainCommandsToMixer();

    alignas(64) std::array<f32, kMixBlockFrames * 2U> tempStereo{};
    usize captureOffset = 0U;

    for (u32 b = 0U; b < blockCount; ++b) {
        m_mixer.MixBlock(m_bridge->snapshot, tempStereo);
        if (captureOffset + tempStereo.size() <= optionalCaptureOut.size()) {
            std::memcpy(
                optionalCaptureOut.data() + captureOffset,
                tempStereo.data(),
                tempStereo.size() * sizeof(f32));
            captureOffset += tempStereo.size();
        }
    }

    m_bridge->activeMainVoicesMask &= m_mixer.ActiveVoiceMask();
    m_spatialActiveMask &= m_bridge->activeMainVoicesMask;
}

u32 AudioSystem::ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept {
    if (!m_bridge) return 0U;
    u32 count = 0U;
    u64 mask = m_spatialActiveMask;
    while (mask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        const MainVoiceTracker& tr = m_bridge->trackers[slot];
        const SoundCueDef& def = m_bridge->cueDefs[tr.cueTableSlot];
        if (count < outEmitters.size()) {
            outEmitters[count] = AudioEmitterView{
                tr.worldX, tr.worldY, def.maxDist, (tr.occluded != 0U) ? 1.0f : 0.0f};
        }
        ++count;
    }
    return count;
}

u64 AudioSystem::ComputeAuditHash() const noexcept {
    u64 h = m_mixer.ComputeAuditHash();
    if (m_bridge) {
        h ^= m_bridge->activeMainVoicesMask;
        h *= 1099511628211ULL;
        h ^= static_cast<u64>(m_bridge->coalescedCount);
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace en
