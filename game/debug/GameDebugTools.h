#pragma once

#include "debug/DebugSystem.h"

namespace vots {

class VeilGameApp;

// Live tunable gameplay CVars and developer console commands for Veil of the Silent.
struct GameDebugCVars {
    bool godMode{false};
    bool infiniteHarmony{false};
    bool showTelegraphLabels{true};
    float playerSpeedScale{1.0f};
};

class GameDebugTools {
public:
    static void RegisterCVarsAndCommands(
        en::DebugSystem& debug,
        VeilGameApp* app) noexcept;

    static void RecordFrameWatchPlots(
        en::DebugSystem& debug,
        const VeilGameApp& app) noexcept;
};

} // namespace vots
