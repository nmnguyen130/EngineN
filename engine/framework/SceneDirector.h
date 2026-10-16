#pragma once

#include "framework/FrameworkTypes.h"

#include <array>

namespace en::framework {

enum class TransitionPhase : u8 {
    Idle = 0,
    WaitingManifestBlob,
    StreamingSceneAssets,
    ReadyToActivate
};

enum class StateCmdKind : u8 {
    Push = 0,
    Pop,
    Switch,
    ClearAndPush
};

struct alignas(16) PendingStateCmd {
    StateLayerDesc desc{};
    StateCmdKind kind{StateCmdKind::Push};
    u8 reserved[15]{};
};
static_assert(sizeof(PendingStateCmd) == 64);

// Pure C++23 Layer-1 Structure-of-Arrays (SoA) state stack and non-blocking scene transition coordinator.
// Ownership: Owns persistent/active/staging ScopeIds inside AssetSystem; zero OS or heap resources.
// Invariant: Hot 120 Hz dispatch masks (m_activeSimMask, m_activeRenderMask) are pre-baked on state mutation.
class alignas(64) SceneDirector {
public:
    SceneDirector() noexcept = default;

    [[nodiscard]] Result<void, FrameworkErrc> Init(assets::AssetSystem& assets) noexcept;
    void Shutdown(assets::AssetSystem& assets, audio::AudioSystem& audio) noexcept;

    [[nodiscard]] Result<void, FrameworkErrc> PushState(const StateLayerDesc& desc) noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> PopState() noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> SwitchState(const StateLayerDesc& desc) noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> ClearAndPushState(const StateLayerDesc& desc) noexcept;

    [[nodiscard]] Result<void, FrameworkErrc> RequestSceneTransition(
        const SceneTransitionDesc& desc,
        assets::AssetSystem& assets) noexcept;

    // Safe Frame Mutation Point (invoked before TimeManager::BeginFrame).
    // Advances non-blocking scene streaming, activates ready scenes, and commits queued state commands.
    [[nodiscard]] Result<bool, FrameworkErrc> CommitSafeFramePoint(
        SceneLoadContext& loadCtx,
        input::InputPipeline& input,
        time::TimeManager& time) noexcept;

    // O(popcount) branchless intrinsic dispatch using std::countr_zero over pre-baked bitmasks.
    void DispatchSimTick(SimContext& simCtx) const noexcept;
    void DispatchRenderExtract(RenderContext& renderCtx) const noexcept;

    [[nodiscard]] constexpr TransitionPhase Phase() const noexcept { return m_phase; }
    [[nodiscard]] constexpr assets::ScopeId ActiveSceneScope() const noexcept { return m_activeScope; }
    [[nodiscard]] constexpr assets::ScopeId PersistentScope() const noexcept { return m_persistentScope; }
    [[nodiscard]] constexpr assets::AssetId ActiveSceneId() const noexcept { return m_activeSceneId; }
    [[nodiscard]] constexpr u8 StateDepth() const noexcept { return m_stackDepth; }
    [[nodiscard]] constexpr bool IsClockPausedByState() const noexcept { return m_pausedClockMask != 0; }

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    [[nodiscard]] static u64 CombineAuditHashes(
        u64 tickIndex,
        u64 worldHash,
        u64 gameplayHash,
        u64 assetHash,
        u64 audioHash,
        u64 renderHash,
        u64 directorHash) noexcept;

private:
    void RebakeStackMasks(input::InputPipeline& input, time::TimeManager& time) noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> AdvanceSceneStreaming(SceneLoadContext& loadCtx) noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> PinManifestPayload(
        const CookedSceneManifestHeader& hdr,
        std::span<const CookedSceneAssetEntry> entries,
        assets::AssetSystem& assets) noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> ActivatePendingScene(SceneLoadContext& loadCtx) noexcept;
    void AbortStagingScope(assets::AssetSystem& assets) noexcept;

    // Cache Lines 0..2 (192B): Hot 120 Hz / 240 Hz SoA Dispatch Tables
    std::array<StateSimTickFn, kMaxStateStackDepth> m_simFns{};
    std::array<StateRenderFn, kMaxStateStackDepth> m_renderFns{};
    std::array<void*, kMaxStateStackDepth> m_userStates{};

    // Cache Line 3 (64B): Hot Bitmasks, Scope Handles, and Active Scene Metadata
    u8 m_stackDepth{0};
    u8 m_activeSimMask{0};
    u8 m_activeRenderMask{0};
    u8 m_pausedClockMask{0};
    u8 m_cmdCount{0};
    TransitionPhase m_phase{TransitionPhase::Idle};
    assets::ScopeId m_persistentScope{assets::kInvalidScope};
    assets::ScopeId m_activeScope{assets::kInvalidScope};
    assets::ScopeId m_stagingScope{assets::kInvalidScope};
    u8 m_reservedHot[3]{0, 0, 0};
    u16 m_topInputMask{0xFFFF};
    std::array<StateLayerFlags, kMaxStateStackDepth> m_layerFlags{};
    std::array<StateId, kMaxStateStackDepth> m_stateIds{};
    std::array<u16, kMaxStateStackDepth> m_inputMasks{};
    assets::AssetId m_activeSceneId{};
    assets::BlobHandle m_stagedManifestBlob{};
    assets::BlobHandle m_stagedCollisionBlob{};
    assets::BlobHandle m_stagedPrefabBlob{};
    assets::BlobHandle m_stagedCueBankBlob{};

    // Cold Lifecycle Callbacks & Pending Transition Queue
    std::array<StateLifecycleFn, kMaxStateStackDepth> m_enterFns{};
    std::array<StateLifecycleFn, kMaxStateStackDepth> m_exitFns{};
    SceneTransitionDesc m_pendingScene{};
    std::array<PendingStateCmd, kMaxPendingStateCmds> m_cmdQueue{};
};

} // namespace en::framework