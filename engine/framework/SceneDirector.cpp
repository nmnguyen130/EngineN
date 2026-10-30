#include "framework/SceneDirector.h"

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

#include <algorithm>
#include <bit>

namespace en {

Result<void, FwErr> SceneDirector::Init(AssetBank& assets) noexcept {
    *this = SceneDirector{};
    auto keep = assets.BeginScope();
    if (!keep) return Err(FwErr::Scope);
    m_keep = *keep;
    auto room = assets.BeginScope();
    if (!room) {
        assets.EndScope(m_keep);
        m_keep = 0xFF;
        return Err(FwErr::Scope);
    }
    m_room = *room;
    return {};
}

void SceneDirector::Shutdown(AssetBank& assets, AudioSystem& audio, LoadCtx* load) noexcept {
    if (load && m_roomExit) {
        m_roomExit(*load);
        m_roomExit = nullptr;
    }
    for (u8 i = m_depth; i > 0;) {
        --i;
        if (m_exit[i] && load) m_exit[i](*load, m_user[i]);
    }
    m_depth = 0;
    if (m_stage != 0xFF) {
        assets.EndScope(m_stage);
        m_stage = 0xFF;
    }
    if (m_room != 0xFF) {
        audio.RequestScopeRelease(m_room);
        assets.EndScope(m_room);
        m_room = 0xFF;
    }
    if (m_keep != 0xFF) {
        assets.EndScope(m_keep);
        m_keep = 0xFF;
    }
    m_phase = Phase::Idle;
    m_nCmd = 0;
}

Result<void, FwErr> SceneDirector::Go(const Room& room) noexcept {
    if (m_phase != Phase::Idle) return Err(FwErr::Busy);
    if (!room.scene.IsValid()) return Err(FwErr::Manifest);
    m_next = room;
    m_wipe = room.wipe;
    m_wipeN = room.wipeTicks;
    m_wipeT = 0;
    m_phase = (m_wipe == Wipe::None || m_wipeN == 0) ? Phase::Load : Phase::Out;
    return {};
}

void SceneDirector::Rebake() noexcept {
    m_sim = 0;
    m_draw = 0;
    m_freeze = 0;
    m_inMask = 0xFFFF;
    for (u8 i = 0; i < m_depth; ++i) {
        const u8 b = u8(1u << i);
        if (!HasFlag(m_flag[i], StateFlag::NoSim)) m_sim |= b;
        if (!HasFlag(m_flag[i], StateFlag::NoDraw)) m_draw |= b;
        if (HasFlag(m_flag[i], StateFlag::Freeze)) m_freeze = 1;
        m_inMask = m_mask[i]; // top wins
    }
}

void SceneDirector::Apply(LoadCtx& load) noexcept {
    for (u8 c = 0; c < m_nCmd; ++c) {
        const StateOp op = m_op[c];
        const State& s = m_cmd[c];
        auto pushOne = [&] {
            if (m_depth >= kMaxStates) return;
            const u8 i = m_depth++;
            m_id[i] = s.id;
            m_flag[i] = s.flags;
            m_mask[i] = s.inMask;
            m_tick[i] = s.tick;
            m_drawFn[i] = s.draw;
            m_exit[i] = s.exit;
            m_user[i] = s.data;
            if (s.enter) s.enter(load, s.data);
        };
        auto popOne = [&] {
            if (!m_depth) return;
            --m_depth;
            if (m_exit[m_depth]) m_exit[m_depth](load, m_user[m_depth]);
        };
        switch (op) {
            case StateOp::Pop:  popOne(); break;
            case StateOp::Reset:
                while (m_depth) popOne();
                [[fallthrough]];
            case StateOp::Push: pushOne(); break;
            case StateOp::Swap: popOne(); pushOne(); break;
        }
    }
    m_nCmd = 0;
    Rebake();
}

Result<bool, FwErr> SceneDirector::Advance(LoadCtx& load) noexcept {
    if (m_phase == Phase::Idle) return false;

    if (m_phase == Phase::Out) {
        if (m_wipeT < m_wipeN) {
            ++m_wipeT;
            return false;
        }
        m_phase = Phase::Load;
        m_wipeT = m_wipeN;
    }

    if (m_phase == Phase::Load) {
        AssetBank& assets = *load.assets;
        if (m_roomExit) {
            m_roomExit(load);
            m_roomExit = nullptr;
        }
        if (load.phys) load.phys->Clear();
        if (load.game) load.game->Clear();

        if (m_stage == 0xFF) {
            auto st = assets.BeginScope();
            if (!st) return Err(FwErr::Scope);
            m_stage = *st;
        }

        if (m_next.collMap.IsValid() && load.phys) {
            const BlobHandle bh = assets.FindBlob(m_next.collMap);
            if (bh.IsValid()) (void)load.phys->BindCollisionMapBlob(assets, bh);
        }
        if (m_next.prefab.IsValid() && load.world) {
            const BlobHandle bh = assets.FindBlob(m_next.prefab);
            if (bh.IsValid()) (void)load.world->InstantiatePrefab(assets, bh);
        }
        if (m_next.cues.IsValid() && load.audio) {
            const BlobHandle bh = assets.FindBlob(m_next.cues);
            if (bh.IsValid()) (void)load.audio->RegisterBank(assets.GetBlob(bh), m_stage);
        }
        if (m_next.enter) {
            if (auto r = m_next.enter(load); !r) {
                assets.EndScope(m_stage);
                m_stage = 0xFF;
                m_phase = Phase::Idle;
                return Err(r.error());
            }
        }

        const u8 old = m_room;
        m_room = m_stage;
        m_stage = 0xFF;
        m_scene = m_next.scene;
        m_roomExit = m_next.exit;
        load.roomScope = m_room;
        load.keepScope = m_keep;
        ++m_nTrans;

        if (old != 0xFF && old != m_keep && old != m_room) {
            load.audio->RequestScopeRelease(old);
            assets.EndScope(old);
        }

        if (m_wipe == Wipe::None || m_wipeN == 0) {
            m_phase = Phase::Idle;
            m_wipeT = 0;
            return true;
        }
        m_phase = Phase::In;
        m_wipeT = m_wipeN;
        return true;
    }

    if (m_phase == Phase::In) {
        if (m_wipeT > 0) {
            --m_wipeT;
            return false;
        }
        m_phase = Phase::Idle;
        m_wipe = Wipe::None;
        return false;
    }
    return false;
}

Result<bool, FwErr> SceneDirector::Tick(LoadCtx& load) noexcept {
    if (m_nCmd) Apply(load);
    return Advance(load);
}

void SceneDirector::TickStates(SimCtx& ctx, const SimStepView& step) const noexcept {
    for (u8 m = m_sim; m; m &= u8(m - 1)) {
        const u8 i = u8(std::countr_zero(m));
        if (m_tick[i]) m_tick[i](ctx, step, m_user[i]);
    }
}

void SceneDirector::DrawStates(DrawCtx& ctx) const noexcept {
    for (u8 m = m_draw; m; m &= u8(m - 1)) {
        const u8 i = u8(std::countr_zero(m));
        if (m_drawFn[i]) m_drawFn[i](ctx, m_user[i]);
    }
}

void SceneDirector::DrawWipe(RenderList& list, Viewport vp) const noexcept {
    if (m_phase == Phase::Idle || m_wipe == Wipe::None || !vp.Valid()) return;

    f32 t = 0.f;
    if (m_wipeN) {
        t = std::clamp(f32(m_wipeT) / f32(m_wipeN), 0.f, 1.f);
        t = t * t * (3.f - 2.f * t);
        if (m_phase == Phase::Load) t = 1.f;
    }
    if (t <= 1e-4f) return;

    const f32 vw = f32(vp.w), vh = f32(vp.h);
    const Vec2 c{vw * 0.5f, vh * 0.5f};

    QuadCmd q{};
    q.center = c;
    q.size = {vw, vh};
    q.texture = kWhiteTexture;
    q.depth = 65500;
    q.layer = 250;
    q.view = RenderList::kHudViewSlot;
    q.blend = BlendMode::Alpha;

    switch (m_wipe) {
        case Wipe::Fade:
            q.color = Color{0, 0, 0, t};
            list.Push(q);
            break;
        case Wipe::HSlide: {
            const f32 w = c.x * t;
            q.color = Color{0.03f, 0.03f, 0.05f, 1};
            q.size.x = w;
            q.center.x = w * 0.5f;
            list.Push(q);
            q.center.x = vw - w * 0.5f;
            list.Push(q);
            break;
        }
        case Wipe::VSlide: {
            const f32 h = c.y * t;
            q.color = Color{0.03f, 0.03f, 0.05f, 1};
            q.size.y = h;
            q.center.y = h * 0.5f;
            list.Push(q);
            q.center.y = vh - h * 0.5f;
            list.Push(q);
            break;
        }
        default: break;
    }
}

} // namespace en
