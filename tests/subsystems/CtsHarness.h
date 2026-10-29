#pragma once

#include "core/Types.h"

#include <cmath>
#include <cstdio>

namespace en::cts {

// Production Engine Conformance Test Suite (CTS) & Deterministic Simulation Test (DST) harness.
// Tracks assertion counts, fuzz operations, and deterministic 64-bit state digests.
struct CtsRunner {
    const char* suiteName{"CTS"};
    u32         passedSuites{0};
    u32         failedSuites{0};
    u64         totalChecks{0};
    u64         fuzzOps{0};
    u64         stateDigest{0xCBF29CE484222325ULL};

    constexpr void RecordCheck(bool ok) noexcept {
        ++totalChecks;
        if (!ok) ++failedSuites;
    }

    constexpr void RecordFuzzOps(u64 ops) noexcept {
        fuzzOps += ops;
    }

    constexpr void MixDigest(u64 h) noexcept {
        stateDigest = HashCombine(stateDigest, h);
    }

    template <typename Fn>
    bool RunCase(const char* caseName, Fn&& fn) noexcept {
        const u64 checksBefore = totalChecks;
        const bool ok = fn(*this);
        const u64 caseChecks = totalChecks - checksBefore;
        if (ok) {
            ++passedSuites;
            std::printf("  [CTS PASS] %-44s (%llu checks)\n",
                        caseName, static_cast<unsigned long long>(caseChecks));
            return true;
        }
        ++failedSuites;
        std::fprintf(stderr, "  [CTS FAIL] %s\n", caseName);
        return false;
    }

    int Finish() const noexcept {
        if (failedSuites == 0U) {
            std::printf(
                "%s: ALL PASSED (%u suites, %llu checks, %llu fuzz/DST steps, digest=0x%016llx)\n",
                suiteName,
                passedSuites,
                static_cast<unsigned long long>(totalChecks),
                static_cast<unsigned long long>(fuzzOps),
                static_cast<unsigned long long>(stateDigest));
            return 0;
        }
        std::fprintf(stderr, "%s: FAILED (%u failed suites)\n", suiteName, failedSuites);
        return 1;
    }
};

[[nodiscard]] constexpr bool Near(f32 a, f32 b, f32 eps = 1e-4f) noexcept {
    return std::abs(a - b) <= eps;
}

#define CTS_CHECK(runner, ...)                                                                      \
    do {                                                                                            \
        const bool _ok = static_cast<bool>(__VA_ARGS__);                                            \
        (runner).RecordCheck(_ok);                                                                  \
        if (!_ok) [[unlikely]] {                                                                    \
            std::fprintf(stderr, "    Check failed at %s:%d: %s\n",                                 \
                         __FILE__, __LINE__, #__VA_ARGS__);                                         \
            return false;                                                                           \
        }                                                                                           \
    } while (false)

} // namespace en::cts
