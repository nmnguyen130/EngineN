#pragma once

#include "core/Types.h"
#include <cstdio>

namespace en::tests {

struct SuiteStats {
    const char* suiteName{nullptr};
    u32 passed{0};
    u32 failed{0};
};

#define EN_CHECK(stats, expr)                                                            \
    do {                                                                                 \
        if (expr) {                                                                      \
            ++(stats).passed;                                                            \
        } else {                                                                         \
            ++(stats).failed;                                                            \
            std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                                \
    } while (false)

void RunSuiteCoreAndTime(SuiteStats& stats) noexcept;
void RunSuitePlatformAndInput(SuiteStats& stats) noexcept;
void RunSuiteAssetsAndWorld(SuiteStats& stats) noexcept;
void RunSuitePhysicsAndGameplay(SuiteStats& stats) noexcept;
void RunSuiteRendererAndAudio(SuiteStats& stats) noexcept;
void RunSuiteDebugAndFramework(SuiteStats& stats) noexcept;

} // namespace en::tests
