#include "game/app/GameApp.h"

#include <cstdio>
#include <string_view>

int main(int argc, char* argv[]) {
    bool runTestsOnly = false;
    bool headless = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--test" || arg == "-t") {
            runTestsOnly = true;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--windowed") {
            headless = false;
        }
    }

    if (runTestsOnly) {
        return en::veil::GameApp::RunTests() ? 0 : 1;
    }

    en::veil::GameApp app{};
    auto initRes = app.Initialize(headless);
    if (!initRes) {
        std::printf("[Veil] Init failed (code %u)\n", static_cast<unsigned>(initRes.error().code));
        return 1;
    }

    if (headless) {
        const auto hit1 = app.StepSimulationTick(1.0f, false, false, true);
        const auto hit2 = app.StepSimulationTick(1.0f, false, false, false, false, true);
        std::printf("[Veil] Headless tick OK | PrimaryDmg=%.1f | Reaction=%u (Dmg=%.1f)\n",
            static_cast<double>(hit1.damageDealt),
            static_cast<unsigned>(hit2.triggeredReaction),
            static_cast<double>(hit2.damageDealt));
        return 0;
    }

    std::printf("[Veil of the Silent] SDL3 Window active. Controls: WASD/Space=Move, J=Atk, K=Dash, L=Parry, I=Shift, Q/E=Wings, U/O=Unison.\n");
    std::fflush(stdout);

    auto runRes = app.App().Run();
    return runRes ? 0 : 1;
}
