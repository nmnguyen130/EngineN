#include "game/debug/GameDebugTools.h"
#include "game/VeilGameApp.h"
#include "game/scenes/GameScenes.h"

namespace vots {

void GameDebugTools::RegisterCVarsAndCommands(
    en::DebugSystem& debug,
    VeilGameApp* app) noexcept {
#if EN_ENABLE_DEBUG_TOOLS
    if (!debug.IsInitialized() || app == nullptr) return;

    en::DebugConsole& con = debug.Console();
    (void)con.RegisterCVarBool("vots_god_mode", &app->CVars().godMode);
    (void)con.RegisterCVarBool("vots_infinite_harmony", &app->CVars().infiniteHarmony);
    (void)con.RegisterCVarBool("vots_show_telegraphs", &app->CVars().showTelegraphLabels);
    (void)con.RegisterCVarF32("vots_speed_scale", &app->CVars().playerSpeedScale, 0.25f, 4.0f);

    (void)con.RegisterCommand(
        "echo_unlock_all",
        [](std::string_view, void* userCtx, en::DebugConsole& console) noexcept {
            auto* g = static_cast<VeilGameApp*>(userCtx);
            if (!g) return;
            const EntityId pid = g->PlayerEntity();
            if (g->EngineApp().GetWorld().IsAlive(pid)) {
                if (auto* ps = g->EngineApp().GetWorld().TryGet<PlayerStateComp>(pid)) {
                    ps->unlockedEchoMask = 0x0FFFU;
                    console.Print(en::LogSeverity::Info, "Unlocked all 12 Apostate Echoes (0x0FFF).");
                }
            }
        },
        app);

    (void)con.RegisterCommand(
        "harmony_fill",
        [](std::string_view, void* userCtx, en::DebugConsole& console) noexcept {
            auto* g = static_cast<VeilGameApp*>(userCtx);
            if (!g) return;
            const EntityId pid = g->PlayerEntity();
            if (g->EngineApp().GetWorld().IsAlive(pid)) {
                if (auto* ps = g->EngineApp().GetWorld().TryGet<PlayerStateComp>(pid)) {
                    ps->harmony = kMaxHarmonyGauge;
                    console.Print(en::LogSeverity::Info, "Harmony set to 100.");
                }
            }
        },
        app);

    (void)con.RegisterCommand(
        "memory_tuning_complete",
        [](std::string_view, void* userCtx, en::DebugConsole& console) noexcept {
            auto* g = static_cast<VeilGameApp*>(userCtx);
            if (!g) return;
            g->Metroidvania().CompleteMemoryTuningFork(0U, g->EngineApp().Gameplay());
            g->Metroidvania().CompleteMemoryTuningFork(1U, g->EngineApp().Gameplay());
            g->Metroidvania().CompleteMemoryTuningFork(2U, g->EngineApp().Gameplay());
            console.Print(en::LogSeverity::Info, "Memory Tuning questline completed.");
        },
        app);
#else
    (void)debug;
    (void)app;
#endif
}

void GameDebugTools::RecordFrameWatchPlots(
    en::DebugSystem& debug,
    const VeilGameApp& app) noexcept {
#if EN_ENABLE_DEBUG_TOOLS
    if (!debug.IsInitialized()) return;
    const en::World& world = app.EngineApp().GetWorld();
    const EntityId pid = app.PlayerEntity();
    if (world.IsAlive(pid)) {
        if (const auto* ps = world.TryGet<PlayerStateComp>(pid)) {
            debug.Profiler().PlotValue("Harmony", static_cast<f32>(ps->harmony));
        }
        if (const auto* recv = world.TryGet<en::DamageReceiver2D>(pid)) {
            debug.Profiler().PlotValue("KaelenHP", static_cast<f32>(recv->hp));
        }
    }
    const EntityId bid = app.BossEntity();
    if (world.IsAlive(bid)) {
        if (const auto* brk = world.TryGet<BreakAndCcComp>(bid)) {
            debug.Profiler().PlotValue("BossBreak", static_cast<f32>(brk->breakMeter));
        }
    }
#else
    (void)debug;
    (void)app;
#endif
}

} // namespace vots
