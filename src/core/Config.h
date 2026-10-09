#pragma once

#include <cstdint>

namespace EngineN {

struct AppConfig {
    const char* appName = "Metroidvania";
    int windowWidth = 1280;
    int windowHeight = 720;
    int simulationHz = 120;
    int targetRenderFps = 0; // 0 = uncapped / vsync-driven
    int maxCatchupTicks = 8;
};

} // namespace EngineN
