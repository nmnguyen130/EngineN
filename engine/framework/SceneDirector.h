#pragma once

#include "framework/FrameworkTypes.h"
#include "renderer/RenderList.h"
#include "renderer/RendererTypes.h"

namespace en {

// State stack (play/pause/menu) + room transition with wipe.
// Non-owning; subsystems passed at call sites.
class alignas(64) SceneDirector {
public:
    [[nodiscard]] Result<void, FwErr> Init(AssetBank& assets) noexcept;
    void Shutdown(AssetBank& assets, AudioSystem& audio, LoadCtx* load = nullptr) noexcept;

    [[nodiscard]] constexpr Result<void, FwErr> Push(const State& s) noexcept {
        return Enq(s, StateOp::Push);
    }
    [[nodiscard]] constexpr Result<void, FwErr> Pop() noexcept {
        return Enq({}, StateOp::Pop);
    }
    [[nodiscard]] constexpr Result<void, FwErr> Swap(const State& s) noexcept {
        return Enq(s, StateOp::Swap);
    }
    [[nodiscard]] constexpr Result<void, FwErr> Reset(const State& s) noexcept {
        return Enq(s, StateOp::Reset);
    }

    [[nodiscard]] Result<void, FwErr> Go(const Room& room) noexcept;

    // Once per frame before sim. true = room committed this frame.
    [[nodiscard]] Result<bool, FwErr> Tick(LoadCtx& load) noexcept;

    void TickStates(SimCtx& ctx, const SimStepView& step) const noexcept;
    void DrawStates(DrawCtx& ctx) const noexcept;
    void DrawWipe(RenderList& list, Viewport vp) const noexcept;

    [[nodiscard]] constexpr bool Busy() const noexcept {
        return m_nCmd > 0 || m_phase != Phase::Idle;
    }
    [[nodiscard]] constexpr Phase GetPhase() const noexcept { return m_phase; }
    [[nodiscard]] constexpr u8 RoomScope() const noexcept { return m_room; }
    [[nodiscard]] constexpr u8 KeepScope() const noexcept { return m_keep; }
    [[nodiscard]] constexpr AssetId Scene() const noexcept { return m_scene; }
    [[nodiscard]] constexpr u8 Depth() const noexcept { return m_depth; }
    [[nodiscard]] constexpr u16 InMask() const noexcept { return m_inMask; }
    [[nodiscard]] constexpr bool Frozen() const noexcept { return m_freeze != 0; }
    [[nodiscard]] constexpr u8 Top() const noexcept {
        return m_depth ? m_id[m_depth - 1] : kNoState;
    }
    [[nodiscard]] constexpr u64 Hash() const noexcept {
        u64 h = 14695981039346656037ULL;
        for (u64 v : {m_scene.value, u64(m_depth), u64(m_sim), u64(m_draw),
                      u64(m_freeze), u64(m_inMask), u64(m_nTrans)}) {
            h = (h ^ v) * 1099511628211ULL;
        }
        for (u8 i = 0; i < m_depth; ++i) h = (h ^ u64(m_id[i])) * 1099511628211ULL;
        return h;
    }
    [[nodiscard]] static constexpr u64 Mix(
        u64 tick, u64 world, u64 phys, u64 game, u64 audio, u64 draw, u64 dir) noexcept {
        u64 h = 14695981039346656037ULL;
        for (u64 v : {tick, world, phys, game, audio, draw, dir})
            h = (h ^ v) * 1099511628211ULL;
        return h;
    }

private:
    [[nodiscard]] constexpr Result<void, FwErr> Enq(const State& s, StateOp op) noexcept {
        if (m_nCmd >= kMaxStateCmd) return Err(FwErr::StackFull);
        m_cmd[m_nCmd] = s;
        m_op[m_nCmd] = op;
        ++m_nCmd;
        return {};
    }

    void Rebake() noexcept;
    void Apply(LoadCtx& load) noexcept;
    [[nodiscard]] Result<bool, FwErr> Advance(LoadCtx& load) noexcept;

    u8    m_sim{0};
    u8    m_draw{0};
    u8    m_freeze{0};
    u8    m_depth{0};
    u8    m_nCmd{0};
    Phase m_phase{Phase::Idle};
    u8    m_keep{0xFF};
    u8    m_room{0xFF};
    u8    m_stage{0xFF};
    u8    m_wipeT{0};
    u8    m_wipeN{0};
    Wipe  m_wipe{Wipe::None};
    u16   m_inMask{0xFFFF};
    u16   m_nTrans{0};

    std::array<StateFlag, kMaxStates> m_flag{};
    std::array<u8, kMaxStates>        m_id{};
    std::array<u16, kMaxStates>       m_mask{};
    std::array<void (*)(SimCtx&, const SimStepView&, void*) noexcept, kMaxStates> m_tick{};
    std::array<void (*)(DrawCtx&, void*) noexcept, kMaxStates>                    m_drawFn{};
    std::array<void (*)(LoadCtx&, void*) noexcept, kMaxStates>                    m_exit{};
    std::array<void*, kMaxStates>     m_user{};

    std::array<State, kMaxStateCmd>   m_cmd{};
    std::array<StateOp, kMaxStateCmd> m_op{};

    Room    m_next{};
    AssetId m_scene{};
    void (*m_roomExit)(LoadCtx&) noexcept{};
};
static_assert(sizeof(SceneDirector) <= 512);

} // namespace en
