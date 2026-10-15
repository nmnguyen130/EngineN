#pragma once

#include "audio/AudioMixer.h"
#include "audio/AudioTypes.h"
#include "core/time/TimeViews.h"

#include <array>
#include <atomic>
#include <span>

struct SDL_AudioStream;

namespace en::assets { class AssetBank; class AssetSystem; }
namespace en::world { class World; }
namespace en::gameplay { class SpatialIndex2D; struct SimCue; }
namespace en::platform { enum class WindowFlags : u8; struct WindowState; }

namespace en::audio {

struct AudioSystemConfig {
    u32 arenaBytes{192 * 1024};
    f32 initialMasterGain{1.0f};
    f32 initialMusicGain{0.8f};
    f32 initialSfxGain{1.0f};
    f32 panWidthMeters{14.0f};
    bool muteOnFocusLoss{true};
    bool headlessMode{false};
};

struct alignas(8) CueHashSlot {
    u32 key{0};
    u16 cueIdx{0};
    u16 psl{0}; // 0 == empty slot; probe sequence length + 1 when occupied
};
static_assert(sizeof(CueHashSlot) == 8);

// Coordinates Robin Hood cue lookup, 3-stage voice admission, raycast occlusion, and SDL3 output.
// Move-only RAII owner of the contiguous arena slab and SDL_AudioStream handle.
// Invariant: Continuous parameters sync via wait-free triple buffering; discrete edges use SPSC ring.
class AudioSystem {
public:
    AudioSystem() = default;
    ~AudioSystem() noexcept;

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&& other) noexcept;
    AudioSystem& operator=(AudioSystem&& other) noexcept;

    [[nodiscard]] Result<void, AudioErrc> Init(const AudioSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterCueDef(
        const SoundCueDef& def,
        assets::ScopeId owningScope = 0) noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterMusicTrackDef(
        const MusicTrackDef& def) noexcept;

    [[nodiscard]] Result<void, AudioErrc> RegisterCueBank(
        const assets::BlobRecord& cueBlob,
        assets::ScopeId owningScope) noexcept;

    void RequestScopeRelease(assets::ScopeId scope, assets::AssetSystem& assetSystem) noexcept;
    void OnWindowEvent(platform::WindowFlags flags, const platform::WindowState& winState) noexcept;

    void SetListenerPosition(f32 worldX, f32 worldY) noexcept {
        m_listenerX = worldX;
        m_listenerY = worldY;
    }

    void ConsumeSimCues(
        std::span<const gameplay::SimCue> cues,
        const world::World& world,
        const assets::AssetBank& assetBank) noexcept;

    VoiceHandle PlayCue(
        SoundCueId cueId,
        const assets::AssetBank& assetBank,
        f32 worldX = 0.0f,
        f32 worldY = 0.0f,
        world::EntityId emitter = world::kNullEntity) noexcept;

    void StopVoice(VoiceHandle handle, u16 fadeOutBlocks = 1) noexcept;
    void SetBusGain(AudioBusId bus, f32 linearGain) noexcept;
    void SetBusFilter(AudioBusId bus, FilterMode mode, f32 cutoffHz) noexcept;

    void TransitionMusic(u32 trackId, const assets::AssetBank& assetBank, u8 crossfadeBlocks = 94) noexcept;
    void SetMusicStemWeights(std::span<const f32, kMaxMusicStems> weights) noexcept;

    void Update(
        const world::World& world,
        const gameplay::SpatialIndex2D& spatial,
        const assets::AssetBank& assetBank,
        time::RenderTimeView timeView,
        u8 frozenChannelMask,
        std::span<const f32, 8> channelScales) noexcept;

    void PumpHeadlessBlocks(u32 blockCount, std::span<f32> optionalCaptureOut = {}) noexcept;

    [[nodiscard]] AudioTelemetrySnapshot Telemetry() const noexcept;
    [[nodiscard]] u64 ComputeAuditHash() const noexcept { return m_mixer.ComputeAuditHash(); }

private:
    struct MainVoiceTracker {
        u32 cueId{0};
        world::EntityId emitter{world::kNullEntity};
        u64 spawnTick{0};
        f32 worldX{0.0f};
        f32 worldY{0.0f};
        f32 baseGain{1.0f};
        f32 basePitch{1.0f};
        f32 minDist{2.5f};
        f32 maxDist{22.0f};
        f32 occlusionFactor{1.0f};
        u16 generation{0};
        u8  priority{0};
        u8  curve{0};
        u8  flags{0};
        bool active{false};
    };

    struct SharedBridgeState;

    [[nodiscard]] const SoundCueDef* FindCueDef(SoundCueId id, u16& outIndex) const noexcept;
    bool EnqueueCommand(const AudioCommand& cmd) noexcept;
    void ComputeVoiceSpatialTargets(u32 slot, AudioFrameSnapshot& snap) const noexcept;
    void PublishFrameSnapshot(const AudioFrameSnapshot& snap) noexcept;
    void ExecuteAudioCallback(SDL_AudioStream* stream, int additionalBytes) noexcept;

    static void AudioStreamCallback(
        void* userdata,
        SDL_AudioStream* stream,
        int additionalAmount,
        int totalAmount) noexcept;

    AudioSystemConfig m_cfg{};
    SDL_AudioStream* m_stream{nullptr};
    u8* m_arenaSlab{nullptr};
    SharedBridgeState* m_bridge{nullptr};
    AudioMixer m_mixer{};

    f32 m_listenerX{0.0f};
    f32 m_listenerY{0.0f};
    u64 m_currentTick{0};
    u32 m_rngState{0xA341316Cu};
    u32 m_pendingUnpinScopeMask{0};
    u8  m_nextStealPoolIdx{0};
    bool m_mutedByFocus{false};
};

} // namespace en::audio