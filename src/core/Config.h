#pragma once

#include "core/Types.h"

namespace EngineN {

struct AppConfig {
    const char* appName = "EngineN - Sandbox Runtime";
    int windowWidth = 1280;
    int windowHeight = 720;
    int simulationHz = 120;
    int targetRenderFps = 0; // 0 = uncapped / vsync-driven
    int maxCatchupTicks = 8;
};

} // namespace EngineN
