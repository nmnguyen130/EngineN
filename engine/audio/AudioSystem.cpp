#include "audio/AudioSystem.h"
#include "assets/AssetBank.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <atomic>
#include <cstring>

namespace en {

namespace {

inline constexpr u32 kMaxCueDefs = 256U;
inline constexpr u32 kMaxMusicTrackDefs = 32U;
inline constexpr u32 kMaxCmdRing = 512U;
inline constexpr u32 kCueHashTableSize = 512U;

[[nodiscard]] constexpr f32 EvaluateAttenuation(
    AttenuationCurve curve,
    f32 dist,
    f32 minDist,
    f32 maxDist) noexcept {
    if (curve == AttenuationCurve::None || dist <= minDist) return 1.0f;
    if (dist >= maxDist || maxDist <= minDist) return 0.0f;
    const f32 t = (dist - minDist) / (maxDist - minDist);
    switch (curve) {
        case AttenuationCurve::None:           return 1.0f;
        case AttenuationCurve::Linear:         return 1.0f - t;
        case AttenuationCurve::InverseClamped: return minDist / (minDist + 4.0f * (dist - minDist));
        case AttenuationCurve::Smoothstep:     return 1.0f - (t * t * (3.0f - 2.0f * t));
    }
    return 1.0f - t;
}

[[nodiscard]] constexpr u32 HashCueSlot(u32 key) noexcept {
    const u32 x = key * 0x9E3779B9U;
    return (x ^ (x >> 16U)) & (kCueHashTableSize - 1U);
}

struct alignas(8) VoiceTracker {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 instanceGain{1.0f};
    f32 instancePitch{1.0f};
    u32 emitterId{kNullAudioEmitter};
    u16 generation{0U};
    u16 cueSlot{0U};
};
static_assert(sizeof(VoiceTracker) == 24);

} // namespace

struct alignas(64) AudioSystem::AudioBridgeState {
    AudioFrameSnapshot snapshot{};
    std::array<AudioFrameSnapshot, 2> publishedSnapshots{};
    std::array<AudioCommand, kMaxCmdRing> cmdRing{};
    std::array<SoundCueDef, kMaxCueDefs> cueDefs{};
    std::array<u16, kCueHashTableSize> cueHashSlots{};
    std::array<u8, kMaxCueDefs> cueScopes{};
    std::array<u16, kMaxCueDefs> cueLastSpawnTick{};
    std::array<MusicTrackDef, kMaxMusicTrackDefs> musicDefs{};
    std::array<u8, kMaxMusicTrackDefs> musicScopes{};
    std::array<VoiceTracker, kMaxAudioVoices> trackers{};
    SDL_AudioStream* deviceStream{nullptr};
    Rng64 rng{};
    u64 activeMainVoicesMask{0ULL};
    f32 invPanWidth{1.0f / 16.0f};
    std::atomic<u64> finishedVoicesMask{0ULL};
    std::atomic<u32> activeScopesMaskAtomic{0U};
    std::atomic<u32> cmdWriteIdx{0U};
    std::atomic<u32> cmdReadIdx{0U};
    std::atomic<u32> publishedSnapIdx{0U};
    std::atomic<u32> audioThreadLock{0U};
    u16 cueDefCount{0U};
    u16 musicDefCount{0U};
    u16 nextGeneration{1U};
    bool ownsSdlAudioSubSystem{false};

    [[nodiscard]] VoiceTracker* ResolveVoice(VoiceHandle h) noexcept {
        if (!h.IsValid() || h.index >= kMaxAudioVoices) return nullptr;
        VoiceTracker& tr = trackers[h.index];
        return (tr.generation == h.generation) ? &tr : nullptr;
    }

    [[nodiscard]] u16 FindCueSlot(SoundCueId id) const noexcept {
        if (!id.IsValid()) return 0xFFFFU;
        u32 slot = HashCueSlot(id.value);
        for (u32 probe = 0U; probe < kCueHashTableSize; ++probe) {
            const u16 idx = cueHashSlots[slot];
            if (idx == 0xFFFFU) return 0xFFFFU;
            if (cueDefs[idx].id == id) return idx;
            slot = (slot + 1U) & (kCueHashTableSize - 1U);
        }
        return 0xFFFFU;
    }

    void InsertCueSlot(SoundCueId id, u16 defIdx) noexcept {
        u32 slot = HashCueSlot(id.value);
        for (u32 probe = 0U; probe < kCueHashTableSize; ++probe) {
            if (cueHashSlots[slot] == 0xFFFFU || cueHashSlots[slot] == defIdx) {
                cueHashSlots[slot] = defIdx;
                return;
            }
            slot = (slot + 1U) & (kCueHashTableSize - 1U);
        }
    }

    [[nodiscard]] u16 FindMusicSlot(u32 trackId) const noexcept {
        for (u16 i = 0U; i < musicDefCount; ++i) {
            if (musicDefs[i].trackId == trackId) return i;
        }
        return 0xFFFFU;
    }
};

Result<void, AudioErrc> AudioSystem::Init(const AudioSystemConfig& cfg) noexcept {
    Shutdown();
    if (cfg.panWidthMeters <= 0.1f) {
        return Err(AudioErrc::InvalidConfig);
    }

    const usize required = AudioMixer::RequiredArenaBytes() + sizeof(AudioBridgeState) + 256U;
    if (!m_arena.Init(std::max<usize>(cfg.arenaBytes, required))) {
        return Err(AudioErrc::OutOfMemory);
    }

    auto mixerSlice = m_arena.Carve<u8>(AudioMixer::RequiredArenaBytes());
    auto bridgeSlice = m_arena.Carve<AudioBridgeState>(1U);
    if (mixerSlice.empty() || bridgeSlice.empty()) {
        Shutdown();
        return Err(AudioErrc::OutOfMemory);
    }

    if (auto r = m_mixer.Init(mixerSlice); !r) {
        Shutdown();
        return r;
    }

    m_bridge = bridgeSlice.data();
    m_bridge->invPanWidth = 1.0f / cfg.panWidthMeters;
    m_bridge->cueHashSlots.fill(0xFFFFU);
    m_bridge->cueLastSpawnTick.fill(0xFFFFU);
    m_bridge->snapshot.occludedCutoffHz = std::clamp(cfg.occludedLowPassHz, kMinCutoffHz, kMaxCutoffHz);
    m_bridge->snapshot.targetPitch.fill(1.0f);
    m_bridge->publishedSnapshots[0] = m_bridge->snapshot;
    m_bridge->publishedSnapshots[1] = m_bridge->snapshot;

    if (!cfg.headless) {
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0U && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            m_bridge->ownsSdlAudioSubSystem = true;
        }

        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = static_cast<int>(kAudioSampleRate);

        auto streamCb = [](void* userdata, SDL_AudioStream*, int additionalAmount, int) noexcept {
            if (userdata && additionalAmount > 0) {
                static_cast<AudioSystem*>(userdata)->PullDeviceStreamBytes(additionalAmount);
            }
        };

        m_bridge->deviceStream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
            &spec,
            streamCb,
            this);
        if (m_bridge->deviceStream) {
            (void)SDL_ResumeAudioStreamDevice(m_bridge->deviceStream);
        }
    }

    m_spatialActiveMask = 0ULL;
    m_listenerX = 0.0f;
    m_listenerY = 0.0f;
    m_pendingReleaseScopeMask = 0U;
    m_currentTick = 0U;
    m_focusMuted = false;
    m_snapshotDirty = false;
    return {};
}

void AudioSystem::Shutdown() noexcept {
    if (m_bridge) {
        if (m_bridge->deviceStream) {
            SDL_DestroyAudioStream(m_bridge->deviceStream);
            m_bridge->deviceStream = nullptr;
        }
        if (m_bridge->ownsSdlAudioSubSystem) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            m_bridge->ownsSdlAudioSubSystem = false;
        }
    }
    m_arena.Shutdown();
    m_mixer = AudioMixer{};
    m_bridge = nullptr;
    m_spatialActiveMask = 0ULL;
    m_listenerX = 0.0f;
    m_listenerY = 0.0f;
    m_pendingReleaseScopeMask = 0U;
    m_currentTick = 0U;
    m_focusMuted = false;
    m_snapshotDirty = false;
}

bool AudioSystem::HasAudioDeviceStream() const noexcept {
    return m_bridge && m_bridge->deviceStream != nullptr;
}

void AudioSystem::SetFocusMuted(bool muted) noexcept {
    m_focusMuted = muted;
    if (m_bridge) {
        m_bridge->snapshot.masterMuted = muted;
        m_snapshotDirty = true;
    }
}

void AudioSystem::PublishSnapshot() noexcept {
    if (!m_bridge) return;
    const u32 nextIdx = (m_bridge->publishedSnapIdx.load(std::memory_order_relaxed) + 1U) & 1U;
    m_bridge->publishedSnapshots[nextIdx] = m_bridge->snapshot;
    m_bridge->publishedSnapIdx.store(nextIdx, std::memory_order_release);
    m_snapshotDirty = false;
}

void AudioSystem::SyncFinishedVoicesFromMixer() noexcept {
    if (!m_bridge) return;
    const u64 finished = m_bridge->finishedVoicesMask.exchange(0ULL, std::memory_order_acquire);
    if (finished != 0ULL) {
        m_bridge->activeMainVoicesMask &= ~finished;
        m_spatialActiveMask &= ~finished;
        m_bridge->snapshot.occludedVoicesMask &= ~finished;
    }
}

Result<void, AudioErrc> AudioSystem::RegisterCueDef(const SoundCueDef& def, u8 owningScope) noexcept {
    if (!m_bridge || !def.id.IsValid()) {
        return Err(AudioErrc::InvalidConfig);
    }
    const u16 existing = m_bridge->FindCueSlot(def.id);
    if (existing != 0xFFFFU) {
        m_bridge->cueDefs[existing] = def;
        m_bridge->cueScopes[existing] = owningScope;
        return {};
    }
    if (m_bridge->cueDefCount >= kMaxCueDefs) {
        return Err(AudioErrc::CueTableFull);
    }
    const u16 idx = m_bridge->cueDefCount++;
    m_bridge->cueDefs[idx] = def;
    m_bridge->cueScopes[idx] = owningScope;
    m_bridge->InsertCueSlot(def.id, idx);
    return {};
}

Result<void, AudioErrc> AudioSystem::RegisterMusicTrackDef(const MusicTrackDef& def, u8 owningScope) noexcept {
    if (!m_bridge || def.trackId == 0U) {
        return Err(AudioErrc::InvalidConfig);
    }
    const u16 existing = m_bridge->FindMusicSlot(def.trackId);
    if (existing != 0xFFFFU) {
        m_bridge->musicDefs[existing] = def;
        m_bridge->musicScopes[existing] = owningScope;
        return {};
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
    if (!m_bridge || !cueBlob.IsValid()) {
        return Err(AudioErrc::CorruptedBankBlob);
    }
    const std::span<const u8> bytes = cueBlob.Bytes();
    if (bytes.size() < sizeof(CookedAudioBankHeader)) {
        return Err(AudioErrc::CorruptedBankBlob);
    }

    const auto* hdr = reinterpret_cast<const CookedAudioBankHeader*>(bytes.data());
    const usize cuesBytes = static_cast<usize>(hdr->cueCount) * sizeof(SoundCueDef);
    const usize tracksBytes = static_cast<usize>(hdr->musicTrackCount) * sizeof(MusicTrackDef);
    if (hdr->magic != CookedAudioBankHeader::kMagic ||
        hdr->version != CookedAudioBankHeader::kVersion ||
        bytes.size() < sizeof(CookedAudioBankHeader) + cuesBytes + tracksBytes) {
        return Err(AudioErrc::CorruptedBankBlob);
    }

    const std::span<const SoundCueDef> cues{
        reinterpret_cast<const SoundCueDef*>(bytes.data() + sizeof(CookedAudioBankHeader)),
        hdr->cueCount};
    const std::span<const MusicTrackDef> tracks{
        reinterpret_cast<const MusicTrackDef*>(bytes.data() + sizeof(CookedAudioBankHeader) + cuesBytes),
        hdr->musicTrackCount};

    for (const SoundCueDef& def : cues) {
        if (auto r = RegisterCueDef(def, owningScope); !r) return r;
    }
    for (const MusicTrackDef& track : tracks) {
        if (auto r = RegisterMusicTrackDef(track, owningScope); !r) return r;
    }
    return {};
}

u32 AudioSystem::PollReleasableScopeMask() noexcept {
    if (!m_bridge || m_pendingReleaseScopeMask == 0U) return 0U;
    PumpHeadlessBlocks(0U);
    const u32 activeScopes = m_bridge->activeScopesMaskAtomic.load(std::memory_order_acquire);
    const u32 releasable = m_pendingReleaseScopeMask & ~activeScopes;
    m_pendingReleaseScopeMask &= ~releasable;
    return releasable;
}

void AudioSystem::EnqueueCommand(const AudioCommand& cmd) noexcept {
    if (!m_bridge) return;
    const u32 writeIdx = m_bridge->cmdWriteIdx.load(std::memory_order_relaxed);
    const u32 nextWrite = (writeIdx + 1U) & (kMaxCmdRing - 1U);
    if (nextWrite == m_bridge->cmdReadIdx.load(std::memory_order_acquire)) {
        return;
    }
    m_bridge->cmdRing[writeIdx] = cmd;
    m_bridge->cmdWriteIdx.store(nextWrite, std::memory_order_release);
}

void AudioSystem::ComputeSpatialVoiceParams(
    f32 instanceGain,
    f32 worldX,
    f32 worldY,
    const SoundCueDef& def,
    f32 listenerX,
    f32 listenerY,
    f32& outGainL,
    f32& outGainR) const noexcept {
    if (!HasFlag(def.flags, VoiceFlags::Spatialized2D)) {
        const f32 g = std::clamp(instanceGain * kEqualPowerCenterGain, 0.0f, kMaxVoiceGain);
        outGainL = g;
        outGainR = g;
        return;
    }

    const f32 dx = worldX - listenerX;
    const f32 dy = worldY - listenerY;
    const f32 distSq = dx * dx + dy * dy;

    // Early-exit distance culling before sqrt and trig polynomial evaluation.
    if (def.attenuation != AttenuationCurve::None &&
        (def.maxDist <= def.minDist || distSq >= def.maxDist * def.maxDist)) {
        outGainL = 0.0f;
        outGainR = 0.0f;
        return;
    }

    // Co-located emitter fast-path (player/camera-centered sounds): skip sqrt and FastSinCos.
    if (distSq <= kSilenceThreshold) {
        const f32 g = std::clamp(instanceGain * kEqualPowerCenterGain, 0.0f, kMaxVoiceGain);
        outGainL = g;
        outGainR = g;
        return;
    }

    const f32 dist = std::sqrt(distSq);
    const f32 atten = EvaluateAttenuation(def.attenuation, dist, def.minDist, def.maxDist);
    const f32 nearFieldSpread = (def.minDist > 1e-3f) ? std::min(1.0f, dist / def.minDist) : 1.0f;
    const f32 pan = std::clamp(dx * m_bridge->invPanWidth * nearFieldSpread, -1.0f, 1.0f);
    f32 panL = kEqualPowerCenterGain;
    f32 panR = kEqualPowerCenterGain;
    FastSinCos(0.25f * kPi * (pan + 1.0f), panL, panR);

    outGainL = std::clamp(instanceGain * atten * panL, 0.0f, kMaxVoiceGain);
    outGainR = std::clamp(instanceGain * atten * panR, 0.0f, kMaxVoiceGain);
}

VoiceHandle AudioSystem::PlayCue(
    SoundCueId cueId,
    const AssetBank& assetBank,
    f32 worldX,
    f32 worldY,
    u32 emitterId) noexcept {
    if (!m_bridge || !cueId.IsValid()) return kNullVoiceHandle;
    SyncFinishedVoicesFromMixer();

    const u16 cueIdx = m_bridge->FindCueSlot(cueId);
    if (cueIdx == 0xFFFFU) return kNullVoiceHandle;

    const SoundCueDef& def = m_bridge->cueDefs[cueIdx];
    const u8 scope = m_bridge->cueScopes[cueIdx];

    if (m_bridge->cueLastSpawnTick[cueIdx] != 0xFFFFU &&
        static_cast<u16>(m_currentTick - m_bridge->cueLastSpawnTick[cueIdx]) < def.cooldownTicks) {
        return kNullVoiceHandle;
    }

    AudioHandle chosenClip = def.clipVariants[0];
    if (def.clipVariants[1].IsValid() && (m_bridge->rng.NextU64() & 1ULL) != 0ULL) {
        chosenClip = def.clipVariants[1];
    }
    const AudioRecord& audioRec = assetBank.GetAudio(chosenClip);
    if (!audioRec.pcm || audioRec.frameCount == 0U) {
        return kNullVoiceHandle;
    }

    const VoiceFlags voiceFlags = def.flags | (audioRec.looping ? VoiceFlags::Looping : VoiceFlags::None);
    const f32 gainJit = (def.gainJitter > 0.0f) ? m_bridge->rng.RangeF32(-def.gainJitter, def.gainJitter) : 0.0f;
    const f32 pitchJit = (def.pitchJitter > 0.0f) ? m_bridge->rng.RangeF32(-def.pitchJitter, def.pitchJitter) : 0.0f;
    const f32 randomizedGain = std::clamp(def.baseGain * (1.0f + gainJit), 0.0f, kMaxVoiceGain);
    const f32 randomizedPitch = std::clamp(def.basePitch * (1.0f + pitchJit), kMinVoicePitch, kMaxVoicePitch);

    f32 gainL = 1.0f;
    f32 gainR = 1.0f;
    ComputeSpatialVoiceParams(randomizedGain, worldX, worldY, def, m_listenerX, m_listenerY, gainL, gainR);

    // Cull inaudible non-looping spatial one-shots before voice slot search or stealing.
    if (!HasFlag(voiceFlags, VoiceFlags::Looping) &&
        gainL <= kSilenceThreshold && gainR <= kSilenceThreshold) {
        return kNullVoiceHandle;
    }

    u8 targetSlot = 0xFFU;
    u8 activeForThisCue = 0U;
    u8 stealCandidate = 0xFFU;
    f32 bestStealGain = 1e9f;
    bool reusedEntityVoice = false;

    const u64 freeMask = ~m_bridge->activeMainVoicesMask;
    if (freeMask != 0ULL) {
        targetSlot = static_cast<u8>(std::countr_zero(freeMask));
    }

    u64 activeMask = m_bridge->activeMainVoicesMask;
    while (activeMask != 0ULL) {
        const u8 s = static_cast<u8>(std::countr_zero(activeMask));
        activeMask &= (activeMask - 1ULL);
        const VoiceTracker& tr = m_bridge->trackers[s];

        if (HasFlag(def.flags, VoiceFlags::OneVoicePerEntity) &&
            emitterId != kNullAudioEmitter && tr.emitterId == emitterId && tr.cueSlot == cueIdx) {
            targetSlot = s;
            reusedEntityVoice = true;
            break;
        }
        if (tr.cueSlot == cueIdx) {
            ++activeForThisCue;
        }
        const f32 gainSum = m_bridge->snapshot.targetGainL[s] + m_bridge->snapshot.targetGainR[s];
        if (m_bridge->cueDefs[tr.cueSlot].priority <= def.priority && gainSum < bestStealGain) {
            bestStealGain = gainSum;
            stealCandidate = s;
        }
    }

    if (!reusedEntityVoice) {
        if (def.maxConcurrency > 0U && activeForThisCue >= def.maxConcurrency) {
            return kNullVoiceHandle;
        }
        if (targetSlot == 0xFFU) {
            if (stealCandidate == 0xFFU) return kNullVoiceHandle;
            targetSlot = stealCandidate;
        }
    }

    u16 gen = m_bridge->nextGeneration++;
    if (gen == 0U) gen = m_bridge->nextGeneration++;

    VoiceTracker& tr = m_bridge->trackers[targetSlot];
    tr.worldX = worldX;
    tr.worldY = worldY;
    tr.instanceGain = randomizedGain;
    tr.instancePitch = randomizedPitch;
    tr.emitterId = emitterId;
    tr.generation = gen;
    tr.cueSlot = cueIdx;

    const u64 slotBit = (1ULL << targetSlot);
    m_bridge->activeMainVoicesMask |= slotBit;
    if (HasFlag(def.flags, VoiceFlags::Spatialized2D)) {
        m_spatialActiveMask |= slotBit;
    } else {
        m_spatialActiveMask &= ~slotBit;
    }
    m_bridge->snapshot.occludedVoicesMask &= ~slotBit;
    m_bridge->cueLastSpawnTick[cueIdx] = m_currentTick;

    m_bridge->snapshot.targetGainL[targetSlot] = gainL;
    m_bridge->snapshot.targetGainR[targetSlot] = gainR;
    m_bridge->snapshot.targetPitch[targetSlot] = randomizedPitch;
    m_snapshotDirty = true;

    AudioCommand cmd{};
    cmd.pcmData = audioRec.pcm;
    cmd.frameCount = audioRec.frameCount;
    cmd.loopStartFrame = def.loopStartFrame;
    cmd.initialGainL = gainL;
    cmd.initialGainR = gainR;
    cmd.initialPitch = randomizedPitch;
    cmd.sampleRate = audioRec.sampleRate;
    cmd.generation = gen;
    cmd.fadeBlocks = 1U;
    cmd.type = AudioCmdType::SpawnVoice;
    cmd.bus = def.bus;
    cmd.voiceSlot = targetSlot;
    cmd.flags = voiceFlags;
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
    if (!m_bridge || !m_bridge->ResolveVoice(handle)) return;
    const u8 slot = static_cast<u8>(handle.index);

    AudioCommand cmd{};
    cmd.type = AudioCmdType::StopVoice;
    cmd.voiceSlot = slot;
    cmd.generation = handle.generation;
    cmd.fadeBlocks = fadeOutBlocks;
    EnqueueCommand(cmd);

    if (fadeOutBlocks == 0U) {
        const u64 slotBit = (1ULL << slot);
        m_bridge->activeMainVoicesMask &= ~slotBit;
        m_spatialActiveMask &= ~slotBit;
        m_bridge->snapshot.occludedVoicesMask &= ~slotBit;
    }
}

void AudioSystem::StopAllVoices(u16 fadeOutBlocks) noexcept {
    if (!m_bridge) return;
    SyncFinishedVoicesFromMixer();
    u64 mask = m_bridge->activeMainVoicesMask;
    while (mask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        StopVoice(VoiceHandle{slot, m_bridge->trackers[slot].generation}, fadeOutBlocks);
    }
}

void AudioSystem::SetVoicePosition(VoiceHandle handle, f32 worldX, f32 worldY) noexcept {
    if (!m_bridge) return;
    if (VoiceTracker* tr = m_bridge->ResolveVoice(handle)) {
        tr->worldX = worldX;
        tr->worldY = worldY;
    }
}

void AudioSystem::UpdateEmitterTransform(u32 emitterId, f32 worldX, f32 worldY) noexcept {
    if (!m_bridge || emitterId == kNullAudioEmitter) return;
    u64 mask = m_bridge->activeMainVoicesMask;
    while (mask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        VoiceTracker& tr = m_bridge->trackers[slot];
        if (tr.emitterId == emitterId) {
            tr.worldX = worldX;
            tr.worldY = worldY;
        }
    }
}

void AudioSystem::SetVoiceGain(VoiceHandle handle, f32 linearGain) noexcept {
    if (!m_bridge) return;
    if (VoiceTracker* tr = m_bridge->ResolveVoice(handle)) {
        tr->instanceGain = std::clamp(linearGain, 0.0f, kMaxVoiceGain);
        m_bridge->snapshot.targetGainL[handle.index] = tr->instanceGain * kEqualPowerCenterGain;
        m_bridge->snapshot.targetGainR[handle.index] = tr->instanceGain * kEqualPowerCenterGain;
        m_snapshotDirty = true;
    }
}

void AudioSystem::SetVoicePitch(VoiceHandle handle, f32 pitchRatio) noexcept {
    if (!m_bridge) return;
    if (VoiceTracker* tr = m_bridge->ResolveVoice(handle)) {
        tr->instancePitch = std::clamp(pitchRatio, kMinVoicePitch, kMaxVoicePitch);
        m_bridge->snapshot.targetPitch[handle.index] = tr->instancePitch;
        m_snapshotDirty = true;
    }
}

void AudioSystem::SetBusGain(AudioBusId bus, f32 linearGain) noexcept {
    if (!m_bridge) return;
    m_bridge->snapshot.busGain[static_cast<u8>(bus) & 7U] = std::clamp(linearGain, 0.0f, kMaxVoiceGain);
    m_snapshotDirty = true;
}

void AudioSystem::SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept {
    if (!m_bridge) return;
    const u8 idx = static_cast<u8>(bus) & 7U;
    m_bridge->snapshot.busFilterMode[idx] = mode;
    m_bridge->snapshot.busCutoffHz[idx] = std::clamp(cutoffHz, kMinCutoffHz, kMaxCutoffHz);
    m_snapshotDirty = true;
}

bool AudioSystem::TransitionMusic(
    u32 trackId,
    const AssetBank& assetBank,
    u8 crossfadeBlocks) noexcept {
    if (!m_bridge || trackId == 0U) return false;
    const u16 foundIdx = m_bridge->FindMusicSlot(trackId);
    if (foundIdx == 0xFFFFU) return false;

    const MusicTrackDef& track = m_bridge->musicDefs[foundIdx];
    const u8 scope = m_bridge->musicScopes[foundIdx];

    AudioCommand commitCmd{};
    commitCmd.type = AudioCmdType::CommitMusicDeck;
    commitCmd.loopStartFrame = track.loopStartFrame;
    commitCmd.fadeBlocks = crossfadeBlocks;
    commitCmd.flags = track.loop ? VoiceFlags::Looping : VoiceFlags::None;
    EnqueueCommand(commitCmd);

    const u8 stemCount = std::min<u8>(track.stemCount, static_cast<u8>(kMaxMusicStems));
    for (u8 st = 0U; st < kMaxMusicStems; ++st) {
        m_bridge->snapshot.musicStemWeights[st] = track.defaultWeights[st];
        if (st >= stemCount || !track.stems[st].IsValid()) continue;

        const AudioRecord& rec = assetBank.GetAudio(track.stems[st]);
        if (!rec.pcm || rec.frameCount == 0U) continue;

        AudioCommand stemCmd{};
        stemCmd.pcmData = rec.pcm;
        stemCmd.frameCount = rec.frameCount;
        stemCmd.initialGainL = track.defaultWeights[st];
        stemCmd.sampleRate = rec.sampleRate;
        stemCmd.type = AudioCmdType::BindMusicStem;
        stemCmd.voiceSlot = st;
        stemCmd.channels = rec.channels;
        stemCmd.assetScope = scope;
        EnqueueCommand(stemCmd);
    }

    m_snapshotDirty = true;
    return true;
}

void AudioSystem::SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept {
    if (!m_bridge) return;
    for (u8 st = 0U; st < kMaxMusicStems; ++st) {
        m_bridge->snapshot.musicStemWeights[st] = std::clamp(weights[st], 0.0f, kMaxVoiceGain);
    }
    m_snapshotDirty = true;
}

void AudioSystem::Update(const AudioUpdateDesc& desc) noexcept {
    if (!m_bridge) return;
    SyncFinishedVoicesFromMixer();

    m_currentTick = static_cast<u16>(desc.tickIndex & 0xFFFFU);
    m_listenerX = desc.listenerX;
    m_listenerY = desc.listenerY;

    if (desc.focusMuteOverride >= 0) {
        m_focusMuted = (desc.focusMuteOverride > 0);
    }
    m_bridge->snapshot.masterMuted = m_focusMuted;

    if (desc.assetBank && !desc.triggers.empty()) {
        SubmitTriggers(desc.triggers, *desc.assetBank);
    }

    u64 pausedMask = 0ULL;
    u64 occludedMask = m_bridge->snapshot.occludedVoicesMask & m_spatialActiveMask;
    u64 activeMask = m_bridge->activeMainVoicesMask;
    while (activeMask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(activeMask));
        activeMask &= (activeMask - 1ULL);
        const u64 slotBit = (1ULL << slot);
        VoiceTracker& tr = m_bridge->trackers[slot];
        const SoundCueDef& def = m_bridge->cueDefs[tr.cueSlot];

        const u8 chIdx = static_cast<u8>(def.channel) & 7U;
        if (HasFlag(def.flags, VoiceFlags::PauseOnFreeze) &&
            (desc.frozenChannelMask & (1U << chIdx)) != 0U) {
            pausedMask |= slotBit;
            continue;
        }

        f32 gainL = 0.0f;
        f32 gainR = 0.0f;
        ComputeSpatialVoiceParams(
            tr.instanceGain, tr.worldX, tr.worldY, def, desc.listenerX, desc.listenerY, gainL, gainR);

        if ((m_spatialActiveMask & slotBit) != 0ULL) {
            // Skip occlusion raycast when voice is distance-culled beyond maxDist.
            if (gainL <= kSilenceThreshold && gainR <= kSilenceThreshold) {
                occludedMask &= ~slotBit;
            } else if (desc.occlusionQuery && !HasFlag(def.flags, VoiceFlags::BypassOcclusion) &&
                       ((slot & 3U) == (desc.tickIndex & 3U))) {
                if (desc.occlusionQuery(desc.listenerX, desc.listenerY, tr.worldX, tr.worldY, desc.occlusionUserCtx)) {
                    occludedMask |= slotBit;
                } else {
                    occludedMask &= ~slotBit;
                }
            }
        }

        f32 pitch = tr.instancePitch;
        if (HasFlag(def.flags, VoiceFlags::FollowTimeScale) && desc.channelScales) {
            pitch = std::clamp(pitch * std::clamp(desc.channelScales[chIdx], 0.125f, 4.0f), kMinVoicePitch, kMaxVoicePitch);
        }

        m_bridge->snapshot.targetGainL[slot] = gainL;
        m_bridge->snapshot.targetGainR[slot] = gainR;
        m_bridge->snapshot.targetPitch[slot] = pitch;
    }

    m_bridge->snapshot.pausedVoicesMask = pausedMask;
    m_bridge->snapshot.occludedVoicesMask = occludedMask;
    PublishSnapshot();
}

void AudioSystem::RunMixerBlocks(
    u32 blockCount,
    std::span<f32> optionalCaptureOut,
    bool pushToDevice) noexcept {
    constexpr i32 kBlockBytes = static_cast<i32>(kMixBlockFrames * 2U * sizeof(f32));
    alignas(64) std::array<f32, kMixBlockFrames * 2U> tempStereo{};

    u32 expected = 0U;
    if (!m_bridge->audioThreadLock.compare_exchange_strong(expected, 1U, std::memory_order_acquire)) {
        if (pushToDevice && m_bridge->deviceStream) {
            for (u32 b = 0U; b < blockCount; ++b) {
                (void)SDL_PutAudioStreamData(m_bridge->deviceStream, tempStereo.data(), kBlockBytes);
            }
        }
        return;
    }

    u64 newlyRetired = 0ULL;
    u64 prevMask = m_mixer.ActiveVoiceMask();

    u32 readIdx = m_bridge->cmdReadIdx.load(std::memory_order_relaxed);
    const u32 writeIdx = m_bridge->cmdWriteIdx.load(std::memory_order_acquire);
    while (readIdx != writeIdx) {
        m_mixer.ApplyCommand(m_bridge->cmdRing[readIdx]);
        readIdx = (readIdx + 1U) & (kMaxCmdRing - 1U);
    }
    m_bridge->cmdReadIdx.store(readIdx, std::memory_order_release);
    newlyRetired |= (prevMask & ~m_mixer.ActiveVoiceMask());

    if (blockCount > 0U) {
        const u32 snapIdx = m_bridge->publishedSnapIdx.load(std::memory_order_acquire) & 1U;
        const AudioFrameSnapshot& activeSnap = m_bridge->publishedSnapshots[snapIdx];
        usize captureOffset = 0U;

        for (u32 b = 0U; b < blockCount; ++b) {
            prevMask = m_mixer.ActiveVoiceMask();
            m_mixer.MixBlock(activeSnap, tempStereo);
            newlyRetired |= (prevMask & ~m_mixer.ActiveVoiceMask());

            if (pushToDevice && m_bridge->deviceStream) {
                (void)SDL_PutAudioStreamData(m_bridge->deviceStream, tempStereo.data(), kBlockBytes);
            }
            if (captureOffset + tempStereo.size() <= optionalCaptureOut.size()) {
                std::memcpy(
                    optionalCaptureOut.data() + captureOffset,
                    tempStereo.data(),
                    tempStereo.size() * sizeof(f32));
                captureOffset += tempStereo.size();
            }
        }
    }

    if (newlyRetired != 0ULL) {
        m_bridge->finishedVoicesMask.fetch_or(newlyRetired, std::memory_order_release);
    }
    m_bridge->activeScopesMaskAtomic.store(m_mixer.ActiveAssetScopesMask(), std::memory_order_release);
    m_bridge->audioThreadLock.store(0U, std::memory_order_release);
}

void AudioSystem::PullDeviceStreamBytes(i32 additionalBytes) noexcept {
    if (!m_bridge || additionalBytes <= 0) return;
    constexpr i32 kBlockBytes = static_cast<i32>(kMixBlockFrames * 2U * sizeof(f32));
    const u32 blocksNeeded = static_cast<u32>((additionalBytes + kBlockBytes - 1) / kBlockBytes);
    RunMixerBlocks(blocksNeeded, {}, true);
}

void AudioSystem::PumpHeadlessBlocks(
    u32 blockCount,
    std::span<f32> optionalCaptureOut) noexcept {
    if (!m_bridge) return;
    if (blockCount > 0U && m_snapshotDirty) {
        PublishSnapshot();
    }
    RunMixerBlocks(blockCount, optionalCaptureOut, false);
    if (blockCount > 0U) {
        SyncFinishedVoicesFromMixer();
    }
}

u32 AudioSystem::ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept {
    if (!m_bridge) return 0U;
    const u64 finished = m_bridge->finishedVoicesMask.load(std::memory_order_acquire);
    const u64 occludedMask = m_bridge->snapshot.occludedVoicesMask;
    u32 count = 0U;
    u64 mask = m_spatialActiveMask & ~finished;
    while (mask != 0ULL) {
        const u8 slot = static_cast<u8>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        const VoiceTracker& tr = m_bridge->trackers[slot];
        const SoundCueDef& def = m_bridge->cueDefs[tr.cueSlot];
        if (count < outEmitters.size()) {
            outEmitters[count] = AudioEmitterView{
                tr.worldX,
                tr.worldY,
                def.maxDist,
                ((occludedMask & (1ULL << slot)) != 0ULL) ? 1.0f : 0.0f};
        }
        ++count;
    }
    return count;
}

u64 AudioSystem::ComputeAuditHash() const noexcept {
    u64 h = kFnvOffsetBasis64;
    if (!m_bridge) return h;

    h = HashCombine64(h, m_bridge->rng.State());
    h = HashCombine64(h, m_bridge->nextGeneration);
    h = HashCombine64(h, m_bridge->snapshot.occludedVoicesMask);
    h = HashCombine64(h, static_cast<u64>(std::bit_cast<u32>(m_listenerX)) |
                         (static_cast<u64>(std::bit_cast<u32>(m_listenerY)) << 32U));

    for (u8 i = 0U; i < kAudioBusCount; ++i) {
        h = HashCombine64(h, static_cast<u64>(std::bit_cast<u32>(m_bridge->snapshot.busGain[i])) |
                             (static_cast<u64>(std::bit_cast<u32>(m_bridge->snapshot.busCutoffHz[i])) << 32U));
        h = HashCombine64(h, static_cast<u64>(m_bridge->snapshot.busFilterMode[i]));
    }
    for (u8 st = 0U; st < kMaxMusicStems; ++st) {
        h = HashCombine64(h, std::bit_cast<u32>(m_bridge->snapshot.musicStemWeights[st]));
    }
    return HashCombine64(h, m_mixer.ComputeAuditHash());
}

} // namespace en
