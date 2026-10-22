#include "game/VeilGameApp.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "renderer/Renderer.h"
#include "time/SdlClock.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    bool headless = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--windowed") == 0) {
            headless = false;
        }
    }

    vots::VeilGameApp game{};
    auto initRes = game.Init(headless, {1280, 720});
    if (!initRes.has_value()) {
        std::fprintf(stderr, "[VeilOfTheSilent] Failed to initialize runtime (code=%u, stage=%u)\n",
                     static_cast<unsigned>(initRes.error().code),
                     static_cast<unsigned>(initRes.error().stage));
        return 1;
    }

    if (headless) {
        auto digestRes = game.StepHeadlessTicks(120U, 2U);
        if (!digestRes.has_value()) {
            std::fprintf(stderr, "[VeilOfTheSilent] StepHeadlessTicks failed\n");
            game.Shutdown();
            return 1;
        }
        std::printf("[VeilOfTheSilent] Headless 120 Hz simulation verified: ticks=%llu, combinedAuditHash=0x%016llX\n",
                    static_cast<unsigned long long>(digestRes->tickIndex),
                    static_cast<unsigned long long>(digestRes->combinedHash));
        game.Shutdown();
        return 0;
    }

    en::Platform plat{};
    if (!plat.Init("VeilOfTheSilent", "EngineN", false).has_value()) {
        game.Shutdown();
        return 1;
    }
    en::Window win{};
    en::WindowConfig wcfg{};
    wcfg.title = "Veil of the Silent (GDD v0.2)";
    if (!win.Create(wcfg, false).has_value()) {
        plat.Shutdown();
        game.Shutdown();
        return 1;
    }
    en::Renderer renderer{};
    if (!renderer.Init(win).has_value()) {
        win.Destroy();
        plat.Shutdown();
        game.Shutdown();
        return 1;
    }

    en::FramePacer pacer{};
    pacer.SetTargetFps(120U);
    en::u64 prevNs = en::SdlClock::NowNs();

    while (game.EngineApp().IsRunning() && !plat.ShouldQuit()) {
        pacer.BeginFrame();
        const en::u64 nowNs = en::SdlClock::NowNs();
        const en::u64 deltaNs = (nowNs > prevNs) ? (nowNs - prevNs) : en::kFixedDtNs;
        prevNs = nowNs;

        (void)plat.Pump(win, game.EngineApp().Input());
        (void)game.EngineApp().Input().EndFrame();

        if (!game.StepFrame(deltaNs).has_value()) {
            break;
        }
        renderer.Submit(game.EngineApp().GetRenderList());
        pacer.EndFrameAndPace();
    }

    renderer.Shutdown();
    win.Destroy();
    plat.Shutdown();
    game.Shutdown();
    return 0;
}
