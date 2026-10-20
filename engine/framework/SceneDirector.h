#pragma once

#include "framework/FrameworkTypes.h"

namespace en {

// Versioned binary checkpoint & world-flag serializer with CRC32 integrity verification.
class SaveGameManager final {
public:
    [[nodiscard]] static Result<usize, FrameworkErrc> SerializeSaveSlot(
        const SaveSlotHeader& meta,
        const GameplayPipeline& gameplay,
        std::span<u8> outBuffer) noexcept;

    [[nodiscard]] static Result<SaveSlotHeader, FrameworkErrc> DeserializeSaveSlot(
        std::span<const u8> inBuffer,
        GameplayPipeline& outGameplay) noexcept;
};

// Structure-of-Arrays (SoA) state stack machine and non-blocking '.escn' scene transition coordinator.
// Value type; packs all hot 120 Hz/240 Hz dispatch masks in Cache Line 0.
// Invariant: RebakeStackMasks() computes active sim/render/pause/input masks in a single top-down pass.
class alignas(64) SceneDirector {
public:
    [[nodiscard]] Result<void, FrameworkErrc> Init(AssetSystem& assets) noexcept;
    void Shutdown(AssetSystem& assets, AudioEngine& audio, SceneLoadContext* optionalLoadCtx = nullptr) noexcept;

    [[nodiscard]] Result<void, FrameworkErrc> PushState(const StateLayerDesc& desc) noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> PopState() noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> SwitchState(const StateLayerDesc& desc) noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> ClearAndPushState(const StateLayerDesc& desc) noexcept;

    [[nodiscard]] Result<void, FrameworkErrc> RequestSceneTransition(
        const SceneTransitionDesc& desc, AssetSystem& assets) noexcept;
    [[nodiscard]] Result<bool, FrameworkErrc> CommitSafeFramePoint(SceneLoadContext& loadCtx) noexcept;

    void DispatchSimTick(SimContext& simCtx) const noexcept;
    void DispatchRenderExtract(RenderContext& renderCtx) const noexcept;

    [[nodiscard]] constexpr bool HasPendingWork() const noexcept {
        return m_pendingCmdCount > 0 || m_hasPendingScene;
    }
    [[nodiscard]] constexpr ScopeId ActiveSceneScope() const noexcept { return m_activeScope; }
    [[nodiscard]] constexpr ScopeId PersistentScope() const noexcept { return m_persistentScope; }
    [[nodiscard]] constexpr AssetId ActiveSceneId() const noexcept { return m_activeSceneId; }
    [[nodiscard]] constexpr u8 StateDepth() const noexcept { return m_stackDepth; }
    [[nodiscard]] constexpr u8 ActiveSimMask() const noexcept { return m_activeSimMask; }
    [[nodiscard]] constexpr u8 ActiveRenderMask() const noexcept { return m_activeRenderMask; }
    [[nodiscard]] constexpr u16 TopInputMask() const noexcept { return m_topInputMask; }
    [[nodiscard]] constexpr bool IsClockPausedByState() const noexcept { return m_pausedClockMask != 0; }

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;
    [[nodiscard]] static constexpr u64 CombineAuditHashes(
        u64 tickIndex, u64 worldHash, u64 gameplayHash, u64 assetHash,
        u64 audioHash, u64 renderHash, u64 directorHash) noexcept {
        u64 h = Mix64(tickIndex ^ worldHash);
        h = HashCombine64(h, gameplayHash);
        h = HashCombine64(h, assetHash);
        h = HashCombine64(h, audioHash);
        h = HashCombine64(h, renderHash);
        return HashCombine64(h, directorHash);
    }

private:
    enum class CmdKind : u8 { Push = 0, Pop = 1, Switch = 2, ClearAndPush = 3 };
    struct PendingCmd {
        StateLayerDesc desc{};
        CmdKind kind{CmdKind::Push};
    };

    void RebakeStackMasks() noexcept;

    u8 m_activeSimMask{0};
    u8 m_activeRenderMask{0};
    u8 m_pausedClockMask{0};
    u8 m_stackDepth{0};
    u8 m_pendingCmdCount{0};
    ScopeId m_persistentScope{0};
    ScopeId m_activeScope{kInvalidScope};
    ScopeId m_stagingScope{kInvalidScope};
    u16 m_topInputMask{0xFFFFU};
    bool m_hasPendingScene{false};

    std::array<StateLayerFlags, kMaxStateStackDepth> m_layerFlags{};
    std::array<StateId, kMaxStateStackDepth> m_stateIds{};
    std::array<u16, kMaxStateStackDepth> m_inputMasks{};
    AssetId m_activeSceneId{};

    std::array<void (*)(SimContext&) noexcept, kMaxStateStackDepth> m_simFns{};
    std::array<void (*)(RenderContext&) noexcept, kMaxStateStackDepth> m_renderFns{};
    std::array<void (*)(SceneLoadContext&, void*) noexcept, kMaxStateStackDepth> m_exitFns{};
    std::array<void*, kMaxStateStackDepth> m_userStates{};

    std::array<PendingCmd, 8> m_pendingCmds{};
    SceneTransitionDesc m_pendingScene{};
    void (*m_activeSceneDeactivateFn)(SceneLoadContext&) noexcept {nullptr};
};

} // namespace en
