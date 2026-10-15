#include "debug/DebugSystem.h"
#include "framework/Application.h"
#include "game/GamePolicies.h"
#include "game/GameStates.h"
#include "game/GameTypes.h"

#include <SDL3/SDL_events.h>

namespace {

// Zero-heap static allocations for the client session state and debug subsystem
alignas(64) game::GameSessionState g_gameSession{};
en::debug::DebugSystem g_debugSystem{};
en::framework::Application* g_appPtr{nullptr};

en::platform::EventCapture OnDebugEventTap(void* user, const SDL_Event& ev) noexcept {
    auto* debugSys = static_cast<en::debug::DebugSystem*>(user);
    if (!debugSys || !g_appPtr) return en::platform::EventCapture::None;

    if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
        const bool down = (ev.type == SDL_EVENT_KEY_DOWN);
        const bool shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
        const bool ctrl = (ev.key.mod & SDL_KMOD_CTRL) != 0;
        if (debugSys->OnPlatformKeyEvent(ev.key.scancode, down, shift, ctrl, g_appPtr->Input())) {
            return en::platform::EventCapture::Keyboard;
        }
    } else if (ev.type == SDL_EVENT_TEXT_INPUT) {
        if (debugSys->OnPlatformTextInput(ev.text.text)) {
            return en::platform::EventCapture::Keyboard;
        }
    }
    return en::platform::EventCapture::None;
}

en::Result<void, en::u8> OnClientBoot(
    en::framework::SceneLoadContext& ctx,
    en::framework::SceneDirector& director) noexcept
{
    g_gameSession.director = &director;

    // 1. Initialize in-game DebugSystem and register client CVars & commands
    if (ctx.renderer != nullptr) {
        if (auto dbgRes = g_debugSystem.Init(*ctx.renderer); !dbgRes) {
            return en::Err(static_cast<en::u8>(dbgRes.error()));
        }
    }
    game::RegisterClientDebugTools(g_gameSession, g_debugSystem);

    // 2. Request initial Room 01 load via ImmediateBarrier so Room 01 is ready on Frame 0
    game::RequestRoomLoad(
        g_gameSession,
        ctx.assets,
        game::Assets::kRoom01Scene,
        4.0f,
        4.0f,
        en::framework::SceneTransitionMode::FullReset,
        en::framework::SceneWaitPolicy::ImmediateBarrier);

    // 3. Push initial Gameplay state (or Title state) onto SceneDirector
    if (auto pushRes = director.PushState(game::MakeGameplayStateDesc(&g_gameSession)); !pushRes) {
        return en::Err(static_cast<en::u8>(pushRes.error()));
    }

    // Pin the player entity in DebugInspector so the 64-tick Flight Scrubber records it automatically
    g_debugSystem.Inspector().SetPinnedEntity(g_gameSession.playerEntity);

    return en::Ok;
}

void OnClientPreShutdown(en::framework::SceneLoadContext& ctx) noexcept {
    g_debugSystem.Shutdown(ctx.renderer);
}

} // namespace

int main(int argc, char** argv) {
    bool headlessAuditMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--headless-audit") {
            headlessAuditMode = true;
        }
    }

    en::framework::Application app{};
    g_appPtr = &app;

    const en::framework::ApplicationConfig cfg{
        .appName     = "Aethelgard: Echoes of the Abyss",
        .bootPakPath = "base.enpak"
    };

    const en::framework::AppHooks hooks{
        .userAppState         = &g_gameSession,
        .devToolsTap          = en::platform::EventTap{
            .user     = &g_debugSystem,
            .callback = &OnDebugEventTap
        },
        .gameplayPolicies     = game::MakeGameplayPolicyTable(&g_gameSession),
        .OnRegisterComponents = &game::RegisterClientComponents,
        .OnBoot               = &OnClientBoot,
        .OnPreShutdown        = &OnClientPreShutdown,
        .OnConsumeSimCue      = &game::ConsumeClientSimCue
    };

    auto initRes = app.Init(cfg, hooks);
    if (!initRes) {
        return static_cast<int>(initRes.error().code);
    }

    // Bind default keyboard & gamepad controls on the initialized InputPipeline
    game::ConfigureDefaultInputBindings(app.Input());

    if (headlessAuditMode) {
        // Deterministic 600-tick (5.0 second) headless simulation & audio verification
        auto digestRes = app.StepHeadless(600, 2);
        app.Shutdown();
        return digestRes.has_value() ? 0 : 1;
    }

    // Run canonical 120 Hz fixed-step game loop until window close or quit requested
    while (!g_gameSession.quitRequested) {
        auto stepRes = app.StepFrame();
        if (!stepRes) {
            break;
        }
    }

    app.Shutdown();
    return 0;
}