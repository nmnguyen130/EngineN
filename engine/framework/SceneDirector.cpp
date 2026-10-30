#include "framework/SceneDirector.h"

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "gameplay/GameplaySystem.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

namespace en {

Result<void, FwErrc> SceneDirector::Init(AssetBank& assets) noexcept {
    *this = SceneDirector{};
    auto keep = assets.BeginScope();
    if (!keep) return Err(FwErrc::ScopeExhausted);
    m_persistentScope = *keep;

    auto room = assets.BeginScope();
    if (!room) {
        assets.EndScope(m_persistentScope);
        m_persistentScope = 0xFFU;
        return Err(FwErrc::ScopeExhausted);
    }
    m_roomScope = *room;
    return {};
}

void SceneDirector::Shutdown(AssetBank& assets, LoadCtx* load) noexcept {
    if (load && m_roomExitFn) {
        m_roomExitFn(*load);
        m_roomExitFn = nullptr;
    }
    for (u8 i = m_depth; i > 0U;) {
        --i;
        if (m_exitFns[i] && load) m_exitFns[i](*load, m_userData[i]);
    }
    m_depth = 0U;
    if (m_stagingScope != 0xFFU) {
        assets.EndScope(m_stagingScope);
        m_stagingScope = 0xFFU;
    }
    if (m_roomScope != 0xFFU) {
        assets.EndScope(m_roomScope);
        m_roomScope = 0xFFU;
    }
    if (m_persistentScope != 0xFFU) {
        assets.EndScope(m_persistentScope);
        m_persistentScope = 0xFFU;
    }
    (void)assets.EvictUnpinned();
    m_phase    = TransitionPhase::Idle;
    m_cmdCount = 0U;
}

Result<void, FwErrc> SceneDirector::TransitionToRoom(const RoomDesc& room) noexcept {
    if (m_phase != TransitionPhase::Idle) return Err(FwErrc::Busy);
    if (!room.roomId.IsValid()) return Err(FwErrc::InvalidRoom);
    m_nextRoom     = room;
    m_wipe         = room.wipe;
    m_wipeDuration = room.wipeTicks;
    m_wipeTick     = 0U;
    m_phase        = (m_wipe == WipeKind::None || m_wipeDuration == 0U)
        ? TransitionPhase::Load
        : TransitionPhase::Out;
    return {};
}

void SceneDirector::RebakeMasks() noexcept {
    m_simMask   = 0U;
    m_drawMask  = 0U;
    m_freeze    = 0U;
    m_inputMask = 0xFFFFU;
    for (u8 i = 0U; i < m_depth; ++i) {
        const u8 bit = static_cast<u8>(1U << i);
        if (!HasFlag(m_flags[i], StateFlags::PauseSim)) m_simMask |= bit;
        if (!HasFlag(m_flags[i], StateFlags::NoDraw)) m_drawMask |= bit;
        if (HasFlag(m_flags[i], StateFlags::Freeze)) m_freeze = 1U;
        m_inputMask = HasFlag(m_flags[i], StateFlags::NoInput) ? 0U : m_masks[i];
    }
}

void SceneDirector::ApplyStateCommands(LoadCtx& load) noexcept {
    for (u8 c = 0U; c < m_cmdCount; ++c) {
        const StateOp     op = m_ops[c];
        const SceneState& s  = m_cmds[c];
        const auto pushOne = [&]() noexcept {
            if (m_depth >= kMaxStates) return;
            const u8 i     = m_depth++;
            m_ids[i]       = s.id;
            m_flags[i]     = s.flags;
            m_masks[i]     = s.inputMask;
            m_tickFns[i]   = s.onTick;
            m_drawFns[i]   = s.onDraw;
            m_exitFns[i]   = s.onExit;
            m_userData[i]  = s.userData;
            if (s.onEnter) s.onEnter(load, s.userData);
        };
        const auto popOne = [&]() noexcept {
            if (m_depth == 0U) return;
            --m_depth;
            if (m_exitFns[m_depth]) m_exitFns[m_depth](load, m_userData[m_depth]);
        };
        switch (op) {
            case StateOp::Pop:
                popOne();
                break;
            case StateOp::Reset:
                while (m_depth > 0U) popOne();
                [[fallthrough]];
            case StateOp::Push:
                pushOne();
                break;
            case StateOp::Swap:
                popOne();
                pushOne();
                break;
        }
    }
    m_cmdCount = 0U;
    RebakeMasks();
}

Result<bool, FwErrc> SceneDirector::AdvanceRoomTransition(LoadCtx& load) noexcept {
    if (m_phase == TransitionPhase::Idle) return false;

    if (m_phase == TransitionPhase::Out) {
        if (m_wipeTick < m_wipeDuration) {
            ++m_wipeTick;
            return false;
        }
        m_phase    = TransitionPhase::Load;
        m_wipeTick = m_wipeDuration;
    }

    if (m_phase == TransitionPhase::Load) {
        AssetBank& assets = *load.assets;
        if (m_roomExitFn) {
            m_roomExitFn(load);
            m_roomExitFn = nullptr;
        }
        if (m_nextRoom.clearWorld && load.world) load.world->Clear();
        if (load.physics) load.physics->Clear();
        if (load.gameplay) load.gameplay->Clear();

        if (m_stagingScope == 0xFFU) {
            auto st = assets.BeginScope();
            if (!st) return Err(FwErrc::ScopeExhausted);
            m_stagingScope = *st;
        }

        load.roomScope       = m_stagingScope;
        load.persistentScope = m_persistentScope;

        if (m_nextRoom.collisionMap.IsValid() && load.physics) {
            const BlobHandle bh = assets.FindBlob(m_nextRoom.collisionMap);
            if (bh.IsValid()) (void)load.physics->BindCollisionMapBlob(assets, bh);
        }
        if (m_nextRoom.prefab.IsValid() && load.world) {
            const BlobHandle bh = assets.FindBlob(m_nextRoom.prefab);
            if (bh.IsValid()) (void)load.world->InstantiatePrefab(assets, bh);
        }
        if (m_nextRoom.musicTrack.IsValid() && load.audio) {
            (void)load.audio->PlayMusic(m_nextRoom.musicTrack, assets, m_nextRoom.musicFadeBlocks);
        }
        if (m_nextRoom.onEnter) {
            if (auto r = m_nextRoom.onEnter(load); !r) {
                assets.EndScope(m_stagingScope);
                (void)assets.EvictUnpinned();
                m_stagingScope = 0xFFU;
                m_phase        = TransitionPhase::Idle;
                return Err(r.error());
            }
        }

        const u8 oldScope = m_roomScope;
        m_roomScope       = m_stagingScope;
        m_stagingScope    = 0xFFU;
        m_currentRoom     = m_nextRoom.roomId;
        m_roomExitFn      = m_nextRoom.onExit;
        load.roomScope    = m_roomScope;
        ++m_transitionCount;

        if (oldScope != 0xFFU && oldScope != m_persistentScope && oldScope != m_roomScope) {
            assets.EndScope(oldScope);
            (void)assets.EvictUnpinned();
        }

        if (m_wipe == WipeKind::None || m_wipeDuration == 0U) {
            m_phase    = TransitionPhase::Idle;
            m_wipeTick = 0U;
            return true;
        }
        m_phase    = TransitionPhase::In;
        m_wipeTick = m_wipeDuration;
        return true;
    }

    if (m_phase == TransitionPhase::In) {
        if (m_wipeTick > 0U) {
            --m_wipeTick;
            return false;
        }
        m_phase = TransitionPhase::Idle;
        m_wipe  = WipeKind::None;
        return false;
    }
    return false;
}

Result<bool, FwErrc> SceneDirector::Tick(LoadCtx& load) noexcept {
    if (m_cmdCount > 0U) ApplyStateCommands(load);
    return AdvanceRoomTransition(load);
}

void SceneDirector::TickStates(SimCtx& ctx, const TickStep& step) const noexcept {
    for (u8 m = m_simMask; m != 0U; m &= static_cast<u8>(m - 1U)) {
        const u8 i = static_cast<u8>(std::countr_zero(m));
        if (m_tickFns[i]) m_tickFns[i](ctx, step, m_userData[i]);
    }
}

void SceneDirector::DrawStates(DrawCtx& ctx) const noexcept {
    for (u8 m = m_drawMask; m != 0U; m &= static_cast<u8>(m - 1U)) {
        const u8 i = static_cast<u8>(std::countr_zero(m));
        if (m_drawFns[i]) m_drawFns[i](ctx, m_userData[i]);
    }
}

void SceneDirector::DrawWipe(RenderList& list, Viewport vp) const noexcept {
    if (m_phase == TransitionPhase::Idle || m_wipe == WipeKind::None || !vp.Valid()) return;

    f32 t = 0.0f;
    if (m_wipeDuration > 0U) {
        t = std::clamp(static_cast<f32>(m_wipeTick) / static_cast<f32>(m_wipeDuration), 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);
        if (m_phase == TransitionPhase::Load) t = 1.0f;
    }
    if (t <= 1e-4f) return;

    const f32  vw = static_cast<f32>(vp.w);
    const f32  vh = static_cast<f32>(vp.h);
    const Vec2 center{vw * 0.5f, vh * 0.5f};

    QuadCmd q{
        .center  = center,
        .size    = {vw, vh},
        .texture = kWhiteTexture,
        .depth   = 65500U,
        .layer   = 250U,
        .view    = RenderList::kHudViewSlot,
        .blend   = BlendMode::Alpha,
    };

    switch (m_wipe) {
        case WipeKind::Fade:
            q.color = Color{0.0f, 0.0f, 0.0f, t};
            list.Push(q);
            break;
        case WipeKind::HSlide: {
            const f32 w = center.x * t;
            q.color     = Color{0.03f, 0.03f, 0.05f, 1.0f};
            q.size.x    = w;
            q.center.x  = w * 0.5f;
            list.Push(q);
            q.center.x  = vw - w * 0.5f;
            list.Push(q);
            break;
        }
        case WipeKind::VSlide: {
            const f32 h = center.y * t;
            q.color     = Color{0.03f, 0.03f, 0.05f, 1.0f};
            q.size.y    = h;
            q.center.y  = h * 0.5f;
            list.Push(q);
            q.center.y  = vh - h * 0.5f;
            list.Push(q);
            break;
        }
        default:
            break;
    }
}

} // namespace en
