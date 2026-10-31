#pragma once

#include "core/EventBus.h"
#include "framework/FrameworkTypes.h"
#include "renderer/RenderList.h"

namespace en {

// 4-layer state stack (play/pause/menu) and room transition state machine with background JobSystem streaming.
// Value type; supports proactive adjacent room pre-warming (PreloadRoom), async verification, and screen wipes.
class SceneDirector {
public:
    struct AsyncRoomPrep {
        const AssetBank* assets{nullptr};
        AssetId          roomId{};
        AssetId          collisionMap{};
        AssetId          prefab{};
        BlobHandle       collisionBlob{};
        BlobHandle       prefabBlob{};
        u8               dispatched{0};
        u8               completed{0};
        u16              pad{0};
    };
    static_assert(sizeof(AsyncRoomPrep) == 48);

    [[nodiscard]] Result<void, FwErrc> Init(AssetBank& assets) noexcept;
    void Shutdown(AssetBank& assets, LoadCtx* load = nullptr) noexcept;

    [[nodiscard]] constexpr Result<void, FwErrc> Push(const SceneState& s) noexcept {
        return Enqueue(s, StateOp::Push);
    }
    [[nodiscard]] constexpr Result<void, FwErrc> Pop() noexcept {
        return Enqueue({}, StateOp::Pop);
    }
    [[nodiscard]] constexpr Result<void, FwErrc> Swap(const SceneState& s) noexcept {
        return Enqueue(s, StateOp::Swap);
    }
    [[nodiscard]] constexpr Result<void, FwErrc> Reset(const SceneState& s) noexcept {
        return Enqueue(s, StateOp::Reset);
    }

    // Pre-warms and CRC-verifies an adjacent room's collision map and prefab in the background via JobSystem
    // before the player touches a doorway portal, enabling zero-stall seamless room transitions.
    [[nodiscard]] Result<void, FwErrc> PreloadRoom(
        const RoomDesc& room, AssetBank& assets, JobSystem* jobs = nullptr) noexcept;

    [[nodiscard]] Result<void, FwErrc> TransitionToRoom(const RoomDesc& room) noexcept;

    void ConsumeEvents(EventBus& bus) noexcept;

    [[nodiscard]] Result<bool, FwErrc> Tick(LoadCtx& load) noexcept;

    void TickStates(SimCtx& ctx, const TickStep& step) const noexcept;
    void DrawStates(DrawCtx& ctx) const noexcept;
    void DrawWipe(RenderList& list, Viewport vp) const noexcept;

    [[nodiscard]] constexpr bool IsBusy() const noexcept {
        return m_cmdCount > 0U || m_phase != TransitionPhase::Idle;
    }
    [[nodiscard]] constexpr bool IsPreloaded(AssetId roomId) const noexcept {
        return roomId.IsValid() && m_asyncPrep.roomId == roomId && m_asyncPrep.completed != 0U;
    }
    [[nodiscard]] constexpr TransitionPhase Phase() const noexcept { return m_phase; }
    [[nodiscard]] constexpr u8 RoomScope() const noexcept { return m_roomScope; }
    [[nodiscard]] constexpr u8 PersistentScope() const noexcept { return m_persistentScope; }
    [[nodiscard]] constexpr AssetId CurrentRoom() const noexcept { return m_currentRoom; }
    [[nodiscard]] constexpr u8 Depth() const noexcept { return m_depth; }
    [[nodiscard]] constexpr u16 InputMask() const noexcept { return m_inputMask; }
    [[nodiscard]] constexpr bool IsFrozen() const noexcept { return m_freeze != 0U; }
    [[nodiscard]] constexpr u16 TransitionCount() const noexcept { return m_transitionCount; }
    [[nodiscard]] constexpr u8 TopStateId() const noexcept {
        return m_depth > 0U ? m_ids[m_depth - 1U] : kNoState;
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = HashCombine(0xCBF29CE484222325ULL, m_currentRoom.value);
        h     = HashCombine(h, static_cast<u64>(m_depth) | (static_cast<u64>(m_transitionCount) << 16U));
        for (u8 i = 0U; i < m_depth; ++i) h = HashCombine(h, m_ids[i]);
        return h;
    }

private:
    static void PrepareRoomAssetsJob(void* userCtx) noexcept;

    [[nodiscard]] constexpr Result<void, FwErrc> Enqueue(const SceneState& s, StateOp op) noexcept {
        if (m_cmdCount >= kMaxStateCmds) return Err(FwErrc::StackFull);
        m_cmds[m_cmdCount] = s;
        m_ops[m_cmdCount]  = op;
        ++m_cmdCount;
        return {};
    }

    void RebakeMasks() noexcept;
    void ApplyStateCommands(LoadCtx& load) noexcept;
    [[nodiscard]] Result<bool, FwErrc> AdvanceRoomTransition(LoadCtx& load) noexcept;

    u8              m_simMask{0};
    u8              m_drawMask{0};
    u8              m_freeze{0};
    u8              m_depth{0};
    u8              m_cmdCount{0};
    TransitionPhase m_phase{TransitionPhase::Idle};
    u8              m_persistentScope{0xFF};
    u8              m_roomScope{0xFF};
    u8              m_stagingScope{0xFF};
    u8              m_wipeTick{0};
    u8              m_wipeDuration{0};
    WipeKind        m_wipe{WipeKind::None};
    u16             m_inputMask{0xFFFF};
    u16             m_transitionCount{0};

    std::array<StateFlags, kMaxStates> m_flags{};
    std::array<u8, kMaxStates>         m_ids{};
    std::array<u16, kMaxStates>        m_masks{};
    std::array<void (*)(SimCtx&, const TickStep&, void*) noexcept, kMaxStates> m_tickFns{};
    std::array<void (*)(DrawCtx&, void*) noexcept, kMaxStates>                 m_drawFns{};
    std::array<void (*)(LoadCtx&, void*) noexcept, kMaxStates>                 m_exitFns{};
    std::array<void*, kMaxStates>      m_userData{};

    std::array<SceneState, kMaxStateCmds> m_cmds{};
    std::array<StateOp, kMaxStateCmds>    m_ops{};

    RoomDesc      m_nextRoom{};
    AsyncRoomPrep m_asyncPrep{};
    AssetId       m_currentRoom{};
    void (*m_roomExitFn)(LoadCtx&) noexcept{nullptr};
};
static_assert(sizeof(SceneDirector) <= 592);

} // namespace en
