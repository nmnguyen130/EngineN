#pragma once

#include "framework/FrameworkTypes.h"

#include <array>

namespace en::framework {

enum class TransitionPhase : u8 {
    Idle = 0,
    WaitingManifestBlob,
    StreamingSceneAssets
};

enum class StateCmdKind : u8 {
    Push = 0,
    Pop,
    Switch,
    ClearAndPush
};

// Pure C++23 Layer-1 Structure-of-Arrays (SoA) state stack and non-blocking scene transition coordinator.
// Ownership: Owns persistent/active/staging ScopeIds inside AssetSystem; zero OS or heap resources.
// Invariant: Cache Line 0 holds all hot 120 Hz/240 Hz dispatch masks, scopes, and state metadata (sizeof == 832).
class alignas(64) SceneDirector {
public:
    SceneDirector() noexcept = default;

    [[nodiscard]] Result<void, FrameworkErrc> Init(assets::AssetSystem& assets) noexcept;
    void Shutdown(
        assets::AssetSystem& assets,
        audio::AudioSystem& audio,
        SceneLoadContext* optionalLoadCtx = nullptr) noexcept;

    [[nodiscard]] constexpr Result<void, FrameworkErrc> PushState(const StateLayerDesc& desc) noexcept {
        return EnqueueStateCmd(StateCmdKind::Push, desc);
    }
    [[nodiscard]] constexpr Result<void, FrameworkErrc> PopState() noexcept {
        return EnqueueStateCmd(StateCmdKind::Pop);
    }
    [[nodiscard]] constexpr Result<void, FrameworkErrc> SwitchState(const StateLayerDesc& desc) noexcept {
        return EnqueueStateCmd(StateCmdKind::Switch, desc);
    }
    [[nodiscard]] constexpr Result<void, FrameworkErrc> ClearAndPushState(const StateLayerDesc& desc) noexcept {
        return EnqueueStateCmd(StateCmdKind::ClearAndPush, desc);
    }

    [[nodiscard]] Result<void, FrameworkErrc> RequestSceneTransition(
        const SceneTransitionDesc& desc,
        assets::AssetSystem& assets) noexcept;

    // Safe Frame Mutation Point (invoked before TimeManager::BeginFrame).
    // Advances non-blocking scene streaming, activates ready scenes, and commits queued state commands.
    [[nodiscard]] Result<bool, FrameworkErrc> CommitSafeFramePoint(
        SceneLoadContext& loadCtx) noexcept;

    // O(popcount) branchless intrinsic dispatch using std::countr_zero over pre-baked bitmasks.
    void DispatchSimTick(SimContext& simCtx) const noexcept;
    void DispatchRenderExtract(RenderContext& renderCtx) const noexcept;

    [[nodiscard]] constexpr bool HasPendingWork() const noexcept {
        return m_phase != TransitionPhase::Idle || m_cmdCount != 0;
    }
    [[nodiscard]] constexpr TransitionPhase Phase() const noexcept { return m_phase; }
    [[nodiscard]] constexpr assets::ScopeId ActiveSceneScope() const noexcept { return m_activeScope; }
    [[nodiscard]] constexpr assets::ScopeId PersistentScope() const noexcept { return m_persistentScope; }
    [[nodiscard]] constexpr assets::AssetId ActiveSceneId() const noexcept { return m_activeSceneId; }
    [[nodiscard]] constexpr u8 StateDepth() const noexcept { return m_stackDepth; }
    [[nodiscard]] constexpr u8 ActiveSimMask() const noexcept { return m_activeSimMask; }
    [[nodiscard]] constexpr u8 ActiveRenderMask() const noexcept { return m_activeRenderMask; }
    [[nodiscard]] constexpr u16 TopInputMask() const noexcept { return m_topInputMask; }
    [[nodiscard]] constexpr bool IsClockPausedByState() const noexcept { return m_pausedClockMask != 0; }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept;

    [[nodiscard]] static constexpr u64 CombineAuditHashes(
        u64 tickIndex,
        u64 worldHash,
        u64 gameplayHash,
        u64 assetHash,
        u64 audioHash,
        u64 renderHash,
        u64 directorHash) noexcept;

private:
    [[nodiscard]] constexpr Result<void, FrameworkErrc> EnqueueStateCmd(
        StateCmdKind kind,
        StateLayerDesc desc = {}) noexcept {
        if (m_cmdCount >= kMaxPendingStateCmds) {
            return Err(FrameworkErrc::StateStackOverflow);
        }
        desc.cmdKind = static_cast<u8>(kind);
        m_cmdQueue[m_cmdCount++] = desc;
        return {};
    }

    constexpr void RebakeStackMasks() noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> PinScenePayloads(
        const u8* rawEntries,
        u16 entryCount,
        assets::AssetSystem& assets) noexcept;
    [[nodiscard]] Result<bool, FrameworkErrc> StepSceneTransition(SceneLoadContext& loadCtx) noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> ActivatePendingScene(SceneLoadContext& loadCtx) noexcept;
    void AbortStagingScope(assets::AssetSystem& assets) noexcept;

    // Cache Line 0 (offset 0..63, 64B exact): Hot Bitmasks, Scopes, Layer Flags, State IDs, Input Masks, Active Scene
    u8 m_activeSimMask{0};
    u8 m_activeRenderMask{0};
    u8 m_pausedClockMask{0};
    u8 m_stackDepth{0};
    u8 m_cmdCount{0};
    TransitionPhase m_phase{TransitionPhase::Idle};
    assets::ScopeId m_persistentScope{assets::kInvalidScope};
    assets::ScopeId m_activeScope{assets::kInvalidScope};
    assets::ScopeId m_stagingScope{assets::kInvalidScope};
    u8 m_reserved0{0};
    u16 m_topInputMask{0xFFFF};
    assets::BlobHandle m_stagedManifestBlob{};
    std::array<StateLayerFlags, kMaxStateStackDepth> m_layerFlags{};
    std::array<StateId, kMaxStateStackDepth> m_stateIds{};
    std::array<u16, kMaxStateStackDepth> m_inputMasks{};
    assets::AssetId m_activeSceneId{};

    // Cache Lines 1..3 (offset 64..255, 192B exact): Hot 120 Hz / 240 Hz SoA Dispatch Tables
    std::array<StateSimTickFn, kMaxStateStackDepth> m_simFns{};
    std::array<StateRenderFn, kMaxStateStackDepth> m_renderFns{};
    std::array<void*, kMaxStateStackDepth> m_userStates{};

    // Cache Line 4 (offset 256..319, 64B exact): Cold Exit Callbacks
    std::array<StateLifecycleFn, kMaxStateStackDepth> m_exitFns{};

    // Cache Line 5 (offset 320..383, 64B exact): Pending Scene Transition Descriptor
    SceneTransitionDesc m_pendingScene{};

    // Cache Lines 6..12 (offset 384..831, 448B): Pending State Queue & Staged Scene Handles
    std::array<StateLayerDesc, kMaxPendingStateCmds> m_cmdQueue{};
    assets::BlobHandle m_stagedCollisionBlob{};
    assets::BlobHandle m_stagedPrefabBlob{};
    assets::BlobHandle m_stagedCueBankBlob{};
    SceneDeactivateFn m_activeSceneDeactivateFn{nullptr};
};
static_assert(sizeof(SceneDirector) == 832);
static_assert(alignof(SceneDirector) == 64);

constexpr u64 SceneDirector::ComputeAuditHash() const noexcept {
    auto mix = [](u64 x) constexpr noexcept -> u64 {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    };

    u64 h = mix(m_activeSceneId.value);
    h ^= mix(
        (static_cast<u64>(m_stackDepth) << 40) |
        (static_cast<u64>(m_activeSimMask) << 32) |
        (static_cast<u64>(m_activeRenderMask) << 24) |
        (static_cast<u64>(m_pausedClockMask) << 16) |
        static_cast<u64>(m_topInputMask));

    for (u8 i = 0; i < m_stackDepth; ++i) {
        h ^= mix((static_cast<u64>(m_stateIds[i]) << 8) | static_cast<u64>(m_layerFlags[i]));
    }
    return h;
}

constexpr u64 SceneDirector::CombineAuditHashes(
    u64 tickIndex,
    u64 worldHash,
    u64 gameplayHash,
    u64 assetHash,
    u64 audioHash,
    u64 renderHash,
    u64 directorHash) noexcept {
    auto mix = [](u64 x) constexpr noexcept -> u64 {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    };
    auto fold = [&mix](u64 seed, u64 val) constexpr noexcept -> u64 {
        return seed ^ (mix(val) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };

    u64 seed = 0x517cc1b727220a95ULL;
    seed = fold(seed, tickIndex);
    seed = fold(seed, worldHash);
    seed = fold(seed, gameplayHash);
    seed = fold(seed, assetHash);
    seed = fold(seed, audioHash);
    seed = fold(seed, renderHash);
    seed = fold(seed, directorHash);
    return seed;
}

} // namespace en::framework
