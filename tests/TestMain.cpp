#include "tests/TestRunner.h"
#include <cstdio>

int main() {
    using namespace en::tests;

    struct SuiteEntry {
        const char* name;
        void (*fn)(SuiteStats&) noexcept;
    };

    const SuiteEntry suites[]{
        {"1. Core & Time Subsystem",        &RunSuiteCoreAndTime},
        {"2. Platform & Input Subsystem",   &RunSuitePlatformAndInput},
        {"3. Assets & World ECS Subsystem", &RunSuiteAssetsAndWorld},
        {"4. Physics & Gameplay Subsystem", &RunSuitePhysicsAndGameplay},
        {"5. Renderer & Audio Subsystem",   &RunSuiteRendererAndAudio},
        {"6. Debug & Framework Subsystem",  &RunSuiteDebugAndFramework},
    };

    en::u32 totalPassed = 0;
    en::u32 totalFailed = 0;

    std::printf("EngineN 2026 Clean-Slate Per-Subsystem Verification Suite\n");
    for (const auto& s : suites) {
        SuiteStats st{s.name, 0, 0};
        s.fn(st);
        totalPassed += st.passed;
        totalFailed += st.failed;
        std::printf("  [SUITE] %-32s : %3u passed, %u failed\n", s.name, st.passed, st.failed);
    }
    std::printf("Total Assertions: %u passed, %u failed\n", totalPassed, totalFailed);
    return (totalFailed == 0U) ? 0 : 1;
}
