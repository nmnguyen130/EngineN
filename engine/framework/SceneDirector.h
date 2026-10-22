#pragma once

#include "framework/FrameworkTypes.h"

namespace en {

struct ViewportExtent;

// 576-byte (9 cache lines exact, zero padding) pure C++23 SoA pushdown state-stack and asynchronous scene transition coordinator.
// Packs all hot 120 Hz / 240 Hz bitmasks, scopes, wipe counters, 8-layer metadata, and active scene teardown hook into Cache Line 0 (offset 0..63).
// Invariant: sizeof(SceneDirector) == 576, RebakeStackMasks() executes in 1 top-down pass, and state/scene mutations occur only at CommitSafeFramePoint().
class alignas(64) SceneDirector {
public:
    [[nodiscard]] Result<void, FrameworkErrc> Init(AssetBank& assetBank) noexcept;
    void Shutdown(AssetBank& assetBank, AudioSystem& audio, SceneLoadContext* optionalLoadCtx = nullptr) noexcept;

    [[nodiscard]] constexpr Result<void, FrameworkErrc> PushState(const StateLayerDesc& desc) noexcept {
        return EnqueueStateCmd(desc, StateCmdKind::Push);
    }

    [[nodiscard]] constexpr Result<void, FrameworkErrc> PopState() noexcept {
        StateLayerDesc dummy{};
        return EnqueueStateCmd(dummy, StateCmdKind::Pop);
    }

    [[nodiscard]] constexpr Result<void, FrameworkErrc> SwitchState(const StateLayerDesc& desc) noexcept {
        return EnqueueStateCmd(desc, StateCmdKind::Switch);
    }

    [[nodiscard]] constexpr Result<void, FrameworkErrc> ClearAndPushState(const StateLayerDesc& desc) noexcept {
        return EnqueueStateCmd(desc, StateCmdKind::ClearAndPush);
    }

    [[nodiscard]] Result<void, FrameworkErrc> RequestSceneTransition(
        const SceneTransitionDesc& desc,
        AssetBank& assetBank) noexcept;

    // Re-queues the currently active SceneTransitionDesc as a FullReset reload for live authoring / state reset.
    [[nodiscard]] Result<void, FrameworkErrc> ReloadActiveScene(AssetBank& assetBank) noexcept;

    [[nodiscard]] Result<bool, FrameworkErrc> CommitSafeFramePoint(SceneLoadContext& loadCtx) noexcept;

    void StepTransitionWipeTick() noexcept;

    void DispatchSimTick(SimContext& simCtx, const SimStepView& step) const noexcept;
    void DispatchRenderExtract(RenderContext& renderCtx) const noexcept;

    void EmitTransitionOverlay(RenderList& outList, ViewportExtent viewportSize) const noexcept;

    [[nodiscard]] constexpr bool HasPendingWork() const noexcept {
        return m_cmdCount > 0U || m_phase != TransitionPhase::Idle;
    }
    [[nodiscard]] constexpr TransitionPhase Phase() const noexcept { return m_phase; }
    [[nodiscard]] constexpr u8 ActiveSceneScope() const noexcept { return m_activeScope; }
    [[nodiscard]] constexpr u8 PersistentScope() const noexcept { return m_persistentScope; }
    [[nodiscard]] constexpr AssetId ActiveSceneId() const noexcept { return m_activeSceneId; }
    [[nodiscard]] constexpr u8 StateDepth() const noexcept { return m_stackDepth; }
    [[nodiscard]] constexpr u8 ActiveSimMask() const noexcept { return m_activeSimMask; }
    [[nodiscard]] constexpr u8 ActiveRenderMask() const noexcept { return m_activeRenderMask; }
    [[nodiscard]] constexpr u16 TopInputMask() const noexcept { return m_topInputMask; }
    [[nodiscard]] constexpr u16 TransitionCount() const noexcept { return m_transitionCount; }
    [[nodiscard]] constexpr bool IsClockPausedByState() const noexcept { return m_pausedClockMask != 0U; }

    [[nodiscard]] constexpr u8 TopStateId() const noexcept {
        return (m_stackDepth > 0U) ? m_stateIds[m_stackDepth - 1U] : kInvalidStateId;
    }

    [[nodiscard]] constexpr bool HasState(u8 stateId) const noexcept {
        for (u8 i = 0U; i < m_stackDepth; ++i) {
            if (m_stateIds[i] == stateId) return true;
        }
        return false;
    }

    [[nodiscard]] constexpr f32 TransitionProgress() const noexcept {
        if (m_phase == TransitionPhase::Idle || m_wipeDurationTicks == 0U) return 0.0f;
        const f32 raw = static_cast<f32>(m_wipeElapsedTicks) / static_cast<f32>(m_wipeDurationTicks);
        const f32 t = std::clamp(raw, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = 14695981039346656037ULL;
        const auto mix = [&](u64 v) constexpr noexcept {
            h ^= v;
            h *= 1099511628211ULL;
        };
        mix(m_activeSceneId.value);
        mix(static_cast<u64>(m_stackDepth));
        mix(static_cast<u64>(m_activeSimMask));
        mix(static_cast<u64>(m_activeRenderMask));
        mix(static_cast<u64>(m_pausedClockMask));
        mix(static_cast<u64>(m_topInputMask));
        mix(static_cast<u64>(m_transitionCount));
        for (u8 i = 0U; i < m_stackDepth; ++i) {
            mix(static_cast<u64>(m_stateIds[i]));
        }
        return h;
    }

    [[nodiscard]] static constexpr u64 CombineAuditHashes(
        u64 tickIndex,
        u64 worldHash,
        u64 physicsHash,
        u64 gameplayHash,
        u64 audioHash,
        u64 renderHash,
        u64 directorHash) noexcept {
        u64 h = 14695981039346656037ULL;
        const auto mix = [&](u64 v) constexpr noexcept {
            h ^= v;
            h *= 1099511628211ULL;
        };
        mix(tickIndex);
        mix(worldHash);
        mix(physicsHash);
        mix(gameplayHash);
        mix(audioHash);
        mix(renderHash);
        mix(directorHash);
        return h;
    }

private:
    [[nodiscard]] constexpr Result<void, FrameworkErrc> EnqueueStateCmd(
        const StateLayerDesc& desc,
        StateCmdKind kind) noexcept {
        if (m_cmdCount >= kMaxPendingStateCmds) {
            return Err(FrameworkErrc::StateStackOverflow);
        }
        StateLayerDesc staged = desc;
        staged.cmdKind = kind;
        m_pendingCmds[m_cmdCount++] = staged;
        return {};
    }

    void RebakeStackMasks() noexcept;
    [[nodiscard]] Result<bool, FrameworkErrc> ActivatePendingScene(SceneLoadContext& loadCtx) noexcept;

    // Cache Line 0 (offset 0..63, 64 bytes exact): Hot bitmasks, scope IDs, wipe progress, 8-layer metadata, and active teardown hook
    u8 m_activeSimMask{0U};                                             // 1B (0)
    u8 m_activeRenderMask{0U};                                          // 1B (1)
    u8 m_pausedClockMask{0U};                                           // 1B (2)
    u8 m_stackDepth{0U};                                                // 1B (3)
    u8 m_cmdCount{0U};                                                  // 1B (4)
    TransitionPhase m_phase{TransitionPhase::Idle};                     // 1B (5)
    u8 m_persistentScope{0xFFU};                                        // 1B (6)
    u8 m_activeScope{0xFFU};                                            // 1B (7)
    u8 m_stagingScope{0xFFU};                                           // 1B (8)
    u8 m_wipeElapsedTicks{0U};                                          // 1B (9)
    u8 m_wipeDurationTicks{0U};                                         // 1B (10)
    TransitionWipeKind m_activeWipe{TransitionWipeKind::None};          // 1B (11)
    u16 m_topInputMask{0xFFFFU};                                        // 2B (12..13)
    u16 m_transitionCount{0U};                                          // 2B (14..15)
    std::array<StateLayerFlags, kMaxStateStackDepth> m_layerFlags{};    // 8B (16..23)
    std::array<u8, kMaxStateStackDepth> m_stateIds{};                   // 8B (24..31)
    std::array<u16, kMaxStateStackDepth> m_inputMasks{};                // 16B (32..47)
    AssetId m_activeSceneId{};                                          // 8B (48..55)
    void (*m_activeSceneDeactivateFn)(SceneLoadContext& ctx) noexcept{nullptr}; // 8B (56..63)

    // Cache Lines 1..3 (offset 64..255, 192 bytes exact): Hot SoA callback and state-data tables
    std::array<void (*)(SimContext&, const SimStepView&, void*) noexcept, kMaxStateStackDepth> m_simFns{};
    std::array<void (*)(RenderContext&, void*) noexcept, kMaxStateStackDepth> m_renderFns{};
    std::array<void*, kMaxStateStackDepth> m_userStates{};

    // Cache Line 4 (offset 256..319, 64 bytes exact): Cold exit callbacks
    std::array<void (*)(SceneLoadContext&, void*) noexcept, kMaxStateStackDepth> m_exitFns{};

    // Cache Lines 5..7 (offset 320..511, 192 bytes exact): 4 x 48B pending state command queue
    alignas(64) std::array<StateLayerDesc, kMaxPendingStateCmds> m_pendingCmds{};

    // Cache Line 8 (offset 512..575, 64 bytes exact): Staged / last-committed SceneTransitionDesc
    SceneTransitionDesc m_pendingScene{};
};
static_assert(sizeof(SceneDirector) == 576);

} // namespace en
