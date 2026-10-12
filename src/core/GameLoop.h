#pragma once

#include "core/EngineContext.h"
#include "core/Result.h"
#include "core/Error.h"
#include "core/time/TickContext.h"
#include "core/time/SimEvents.h"
#include "core/time/TimeManager.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "platform/input/InputSystem.h"
#include "asset/AssetManager.h"
#include "renderer/RenderQueue.h"
#include "renderer/Batcher.h"
#include "renderer/FramePacket.h"
#include "renderer/Presenter.h"

#include <concepts>

namespace EngineN {

/**
 * GameLoop: C++20 concept defining the strict contract between the engine and game simulation.
 * Enforces the 4 Golden Boundaries:
 * 1. OnFrameInput: Render-rate input for UI, menus, dev tools.
 * 2. BeginTick: Unconditional snapshot of all Interpolated<T> (runs every tick).
 * 3. Tick: Deterministic 120Hz fixed simulation consuming TickInput and TickContext.
 * 4. Update: Real-time cosmetic presentation (particles, camera shake, UI).
 * 5. Extract: Interpolated frame rendering into typed RenderQueue.
 */
template <class G>
concept GameLoop = requires(G g, const FrameInput& fi, const TickInput& ti, TickContext& tc,
                            const SimEvents& ev, f64 realDt, f32 alpha, RenderQueue& q, const AssetReader& ar) {
    { g.OnFrameInput(fi) }          -> std::same_as<void>;
    { g.BeginTick() }               -> std::same_as<void>;
    { g.Tick(ti, tc) }              -> std::same_as<void>;
    { g.Update(realDt, alpha, ev) } -> std::same_as<void>;
    { g.Extract(q, alpha, ar) }     -> std::same_as<void>;
};

/**
 * RunFrame: Structural engine frame runner executing the 14-step single-threaded lifecycle.
 */
template <GameLoop G>
[[nodiscard]] Result<void, EngineError> RunFrame(EngineContext& e, G& game) noexcept {
    // 0. Presenter waitable swapchain synchronization
    e.presenter->BeginFrame(*e.backend);

    // 1-2. Pump platform events into input system
    (void)e.platform->Pump(*e.window, *e.input);

    // 3. Asset commit: the only point where asset tables mutate
    e.assets->Commit();

    // 4. Publish frame input for UI / window commands
    game.OnFrameInput(e.input->PublishFrameInput());

    // 5. Sample clock and accumulate time
    e.time->BeginFrame();

    // 6. Simulation ticks at 120Hz fixed cadence
    e.events->Clear();
    TickContext ctx{*e.time, *e.events};
    while (e.time->ConsumeTick()) {
        const TickInput in = e.input->CutTick();
        game.BeginTick(); // Unconditional snapshot of all Interpolated<T>
        game.Tick(in, ctx);
        e.time->EndTick(); // Advances tick-counted hitstop inside EndTick
    }

    // 7. Calculate sub-frame presentation alpha
    e.time->EndFrame();
    const f32 alpha = static_cast<f32>(e.time->GetAlpha());
    const f64 realDt = e.time->GetRealDt();

    // 8. Real-time cosmetic presentation update
    game.Update(realDt, alpha, *e.events);

    // 9. Frame extraction into typed RenderQueue
    game.Extract(*e.queue, alpha, e.assets->Reader());

    // 10. Linearize and coalesce draw batches
    FramePacket packet{};
    e.batcher->Finalize(*e.arena, packet);

    // 11. Execute GPU draws (submission only, no present)
    if (auto r = e.backend->Execute(packet); !r) {
        return Err(r.Error());
    }

    // 12. Present swapchain to display and handle software pacing
    auto p = e.presenter->EndFrame(*e.backend);
    if (!p) {
        return Err(p.Error());
    }

    return {};
}

} // namespace EngineN
