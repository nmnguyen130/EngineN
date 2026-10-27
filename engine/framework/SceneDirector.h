#pragma once

#include "framework/FrameworkTypes.h"

namespace en {

struct ViewportExtent;

// Coordinates the 8-layer pushdown state stack and asynchronous room/scene transitions.
// Non-owning coordinator; AssetBank, World, PhysicsSystem, GameplayPipeline, and AudioSystem are passed by reference.
// Invariant: m_stackDepth <= kMaxStateStackDepth and rebaked dispatch masks always match [0..m_stackDepth).
class alignas(64) SceneDirector {
public:
    [[nodiscard]] Result<void, FrameworkErrc> Init(AssetBank& assetBank) noexcept;
    void Shutdown(AssetBank& assetBank, AudioSystem& audio, SceneLoadContext* optionalLoadCtx = nullptr) noexcept;

    [[nodiscard]] constexpr Result<void, FrameworkErrc> PushState(const StateLayerDesc& desc) noexcept {
        return EnqueueStateCmd(desc, StateCmdKind::Push);
    }

    [[nodiscard]] constexpr Result<void, FrameworkErrc> PopState() noexcept {
        return EnqueueStateCmd({}, StateCmdKind::Pop);
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
            if (m_stateIds[i] == stateId) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr f32 TransitionProgress() const noexcept {
        if (m_phase == TransitionPhase::Idle || m_wipeDurationTicks == 0U) {
            return 0.0f;
        }
        const f32 raw = static_cast<f32>(m_wipeElapsedTicks) / static_cast<f32>(m_wipeDurationTicks);
        const f32 t = std::clamp(raw, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = 14695981039346656037ULL;
        for (u64 v : {m_activeSceneId.value,
                      static_cast<u64>(m_stackDepth),
                      static_cast<u64>(m_activeSimMask),
                      static_cast<u64>(m_activeRenderMask),
                      static_cast<u64>(m_pausedClockMask),
                      static_cast<u64>(m_topInputMask),
                      static_cast<u64>(m_transitionCount)}) {
            h = (h ^ v) * 1099511628211ULL;
        }
        for (u8 i = 0U; i < m_stackDepth; ++i) {
            h = (h ^ static_cast<u64>(m_stateIds[i])) * 1099511628211ULL;
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
        for (u64 v : {tickIndex, worldHash, physicsHash, gameplayHash, audioHash, renderHash, directorHash}) {
            h = (h ^ v) * 1099511628211ULL;
        }
        return h;
    }

private:
    [[nodiscard]] constexpr Result<void, FrameworkErrc> EnqueueStateCmd(
        const StateLayerDesc& desc,
        StateCmdKind kind) noexcept {
        if (m_cmdCount >= kMaxPendingStateCmds) {
            return Err(FrameworkErrc::StateStackOverflow);
        }
        StateLayerDesc& slot = m_pendingCmds[m_cmdCount++];
        slot = desc;
        slot.cmdKind = kind;
        return {};
    }

    void RebakeStackMasks() noexcept;
    [[nodiscard]] Result<bool, FrameworkErrc> ActivatePendingScene(SceneLoadContext& loadCtx) noexcept;

    u8 m_activeSimMask{0U};
    u8 m_activeRenderMask{0U};
    u8 m_pausedClockMask{0U};
    u8 m_stackDepth{0U};
    u8 m_cmdCount{0U};
    TransitionPhase m_phase{TransitionPhase::Idle};
    u8 m_persistentScope{0xFFU};
    u8 m_activeScope{0xFFU};
    u8 m_stagingScope{0xFFU};
    u8 m_wipeElapsedTicks{0U};
    u8 m_wipeDurationTicks{0U};
    TransitionWipeKind m_activeWipe{TransitionWipeKind::None};
    u16 m_topInputMask{0xFFFFU};
    u16 m_transitionCount{0U};
    std::array<StateLayerFlags, kMaxStateStackDepth> m_layerFlags{};
    std::array<u8, kMaxStateStackDepth> m_stateIds{
        kInvalidStateId, kInvalidStateId, kInvalidStateId, kInvalidStateId,
        kInvalidStateId, kInvalidStateId, kInvalidStateId, kInvalidStateId};
    std::array<u16, kMaxStateStackDepth> m_inputMasks{
        0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU, 0xFFFFU};
    AssetId m_activeSceneId{};
    void (*m_activeSceneDeactivateFn)(SceneLoadContext& ctx) noexcept{nullptr};

    std::array<void (*)(SimContext&, const SimStepView&, void*) noexcept, kMaxStateStackDepth> m_simFns{};
    std::array<void (*)(RenderContext&, void*) noexcept, kMaxStateStackDepth> m_renderFns{};
    std::array<void*, kMaxStateStackDepth> m_userStates{};
    std::array<void (*)(SceneLoadContext&, void*) noexcept, kMaxStateStackDepth> m_exitFns{};

    alignas(64) std::array<StateLayerDesc, kMaxPendingStateCmds> m_pendingCmds{};

    SceneTransitionDesc m_pendingScene{};
};
static_assert(sizeof(SceneDirector) == 576);

} // namespace en
