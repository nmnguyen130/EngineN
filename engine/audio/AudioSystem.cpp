#include "audio/AudioSystem.h"
#include "core/Serialization.h"
#include <algorithm>
#include <cmath>

namespace en {

Result<void, AudioErrc> AudioEngine::Init(const AudioEngineConfig& cfg) noexcept {
    Shutdown();
    if (!m_arena.Init(cfg.arenaBytes)) return Err(AudioErrc::ArenaExhausted);
    if (!m_mixer.Init(m_arena) || !m_cueTable.Init(m_arena, 256)) {
        Shutdown();
        return Err(AudioErrc::ArenaExhausted);
    }
    m_snapshot = AudioFrameSnapshot{};
    m_snapshot.busGains.fill(1.0f);
    m_rawBusGains.fill(1.0f);
    m_busParents = {0xFFU, 0U, 3U, 0U, 3U, 0U, 0U, 0U};
    m_snapshot.busFilterCutoffHz.fill(20000.0f);
    m_invPanWidth = 1.0f / std::max(1.0f, cfg.panWidthMeters);
    m_pendingReleaseScopesMask = 0;
    m_initialized = true;
    return {};
}

void AudioEngine::Shutdown() noexcept {
    m_arena.Shutdown();
    m_pendingReleaseScopesMask = 0;
    m_initialized = false;
}

Result<void, AudioErrc> AudioEngine::RegisterCueDef(const SoundCueDef& def) noexcept {
    if (!def.cueId.IsValid()) return Err(AudioErrc::InvalidConfig);
    if (!m_cueTable.InsertOrAssign(def.cueId.value, def)) return Err(AudioErrc::CueTableFull);
    return {};
}

Result<void, AudioErrc> AudioEngine::RegisterCueBank(const BlobRecord& cueBlob) noexcept {
    if (!cueBlob.IsValid() || cueBlob.size < sizeof(CookedAudioBankHeader)) {
        return Err(AudioErrc::CorruptedBankBlob);
    }
    BinaryReader r(cueBlob.Bytes());
    CookedAudioBankHeader hdr{};
    if (!r.Read(hdr) || hdr.magic != kCookedAudioBankMagic) {
        return Err(AudioErrc::CorruptedBankBlob);
    }
    for (u16 i = 0; i < hdr.cueCount; ++i) {
        SoundCueDef def{};
        if (!r.Read(def)) return Err(AudioErrc::CorruptedBankBlob);
        (void)RegisterCueDef(def);
    }
    return {};
}

VoiceHandle AudioEngine::PlayCue(
    AssetId cueId, const AssetBank& assetBank, Vec2 worldPos, u32 emitterId, ScopeId scope) noexcept {
    const SoundCueDef* def = m_cueTable.Find(cueId.value);
    if (!def) return {};
    const AudioRecord& clip = assetBank.GetAudio(def->clipHandle);
    return m_mixer.SpawnVoice(*def, clip, worldPos, emitterId, scope);
}

void AudioEngine::SetBusGain(AudioBusId bus, f32 linearGain) noexcept {
    m_rawBusGains[static_cast<u8>(bus) & 7U] = std::clamp(linearGain, 0.0f, 2.0f);
    for (u8 i = 0; i < kAudioBusCount; ++i) {
        m_snapshot.busGains[i] = EffectiveBusGain(static_cast<AudioBusId>(i));
    }
}

void AudioEngine::SetBusParent(AudioBusId child, AudioBusId parent) noexcept {
    const u8 c = static_cast<u8>(child) & 7U;
    const u8 p = static_cast<u8>(parent) & 7U;
    if (c != 0U && c != p) {
        m_busParents[c] = p;
        for (u8 i = 0; i < kAudioBusCount; ++i) {
            m_snapshot.busGains[i] = EffectiveBusGain(static_cast<AudioBusId>(i));
        }
    }
}

f32 AudioEngine::EffectiveBusGain(AudioBusId bus) const noexcept {
    u8 cur = static_cast<u8>(bus) & 7U;
    f32 gain = 1.0f;
    for (u8 depth = 0; depth < kAudioBusCount && cur < kAudioBusCount; ++depth) {
        gain *= m_rawBusGains[cur];
        cur = m_busParents[cur];
    }
    return std::clamp(gain, 0.0f, 2.0f);
}

void AudioEngine::SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept {
    const u8 idx = static_cast<u8>(bus) & 7U;
    m_snapshot.busFilterMode[idx] = mode;
    m_snapshot.busFilterCutoffHz[idx] = std::clamp(cutoffHz, 80.0f, 22000.0f);
}

u32 AudioEngine::PollReleasableScopeMask() noexcept {
    if (m_pendingReleaseScopesMask == 0) return 0;
    u32 inUseMask = 0;
    for (const auto& v : m_mixer.Voices()) {
        if (v.active && v.scopeId < kMaxAssetScopes) {
            inUseMask |= (1U << v.scopeId);
        }
    }
    const u32 ready = m_pendingReleaseScopesMask & ~inUseMask;
    m_pendingReleaseScopesMask &= ~ready;
    return ready;
}

void AudioEngine::Update(const AudioUpdateDesc& desc) noexcept {
    if (desc.assetBank) {
        for (const AudioTriggerReq& req : desc.triggers) {
            (void)PlayCue(req.cueId, *desc.assetBank, {req.worldX, req.worldY}, req.emitterId);
        }
    }

    ++m_snapshot.sequence;
    m_snapshot.activeMask = 0;
    m_snapshot.frozenVoiceMask = 0;

    auto voices = m_mixer.Voices();
    for (u16 i = 0; i < kMaxAudioVoices; ++i) {
        auto& v = voices[i];
        if (!v.active) continue;
        m_snapshot.activeMask |= (1ULL << i);
        if (HasFlag(v.flags, VoiceFlags::PauseOnHitstop) &&
            ((desc.frozenChannelMask & (1U << static_cast<u8>(v.channel))) != 0)) {
            m_snapshot.frozenVoiceMask |= (1ULL << i);
            continue;
        }

        f32 atten = 1.0f;
        f32 pan = 0.0f;
        if (HasFlag(v.flags, VoiceFlags::Spatialized2D)) {
            const f32 dx = v.worldPos.x - desc.listenerX;
            const f32 dy = v.worldPos.y - desc.listenerY;
            const f32 dist = std::sqrt(dx * dx + dy * dy);
            atten = SmoothStep(v.maxDist, 0.5f, dist);
            pan = std::clamp(dx * m_invPanWidth, -1.0f, 1.0f);

            if (desc.occlusionQuery && !HasFlag(v.flags, VoiceFlags::BypassOcclusion)) {
                const f32 occ = desc.occlusionQuery(
                    desc.listenerX, desc.listenerY, v.worldPos.x, v.worldPos.y, desc.occlusionUserCtx);
                v.cutoffRatio = 0.45f - 0.35f * std::clamp(occ, 0.0f, 1.0f);
                atten *= (1.0f - 0.45f * std::clamp(occ, 0.0f, 1.0f));
            }
        }

        const f32 angle = (pan + 1.0f) * (kPi * 0.25f);
        f32 c = 0.7071f, s = 0.7071f;
        FastSinCos(angle, c, s);
        const f32 muteScale = desc.focusMuted ? 0.0f : 1.0f;
        m_snapshot.targetGainL[i] = v.baseGain * atten * c * muteScale;
        m_snapshot.targetGainR[i] = v.baseGain * atten * s * muteScale;
    }
}

void AudioEngine::PumpBlocks(u32 blockCount, std::span<f32> optionalCaptureOut) noexcept {
    std::array<f32, kAudioMixBlockFrames * 2> blockBuf{};
    usize outOffset = 0;
    for (u32 b = 0; b < blockCount; ++b) {
        m_mixer.MixBlock(m_snapshot, blockBuf);
        if (outOffset + blockBuf.size() <= optionalCaptureOut.size()) {
            std::memcpy(optionalCaptureOut.data() + outOffset, blockBuf.data(), blockBuf.size() * sizeof(f32));
            outOffset += blockBuf.size();
        }
    }
}

u32 AudioEngine::ExtractActiveEmitters(std::span<AudioEmitterView> outEmitters) const noexcept {
    u32 count = 0;
    for (const auto& v : m_mixer.Voices()) {
        if (!v.active || !HasFlag(v.flags, VoiceFlags::Spatialized2D)) continue;
        if (count < outEmitters.size()) {
            outEmitters[count] = AudioEmitterView{
                v.worldPos.x, v.worldPos.y, v.maxDist,
                std::clamp((0.45f - v.cutoffRatio) / 0.35f, 0.0f, 1.0f)};
        }
        ++count;
    }
    return count;
}

u64 AudioEngine::ComputeAuditHash() const noexcept {
    return HashCombine64(m_mixer.ComputeAuditHash(), m_snapshot.sequence);
}

} // namespace en
