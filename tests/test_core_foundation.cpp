#include "core/Types.h"
#include "core/Constants.h"
#include "core/Assert.h"
#include "core/Log.h"
#include "core/Error.h"
#include "core/Result.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace EngineN;

void TestTypesAndConstants() {
    static_assert(sizeof(u8) == 1);
    static_assert(sizeof(u16) == 2);
    static_assert(sizeof(u32) == 4);
    static_assert(sizeof(u64) == 8);
    static_assert(sizeof(i8) == 1);
    static_assert(sizeof(i16) == 2);
    static_assert(sizeof(i32) == 4);
    static_assert(sizeof(i64) == 8);
    static_assert(sizeof(f32) == 4);
    static_assert(sizeof(f64) == 8);

    assert(1_KB == 1024);
    assert(2_MB == 2 * 1024 * 1024);
    assert(1_GB == 1024ULL * 1024ULL * 1024ULL);

    assert(std::abs(kDegToRad * 180.0f - kPi) < 1e-5f);
    assert(std::abs(kRadToDeg * kPi - 180.0f) < 1e-5f);
    assert(kInvalidEntityId == ~0u);
    assert(kInvalidIndex == static_cast<usize>(~0ULL));
}

void TestAssertions() {
    int counter = 0;
    ENGINE_VERIFY(++counter == 1, "Counter must increment");
    assert(counter == 1);

    ENGINE_ASSERT(counter == 1, "Counter must still be 1");
}

void TestLogging() {
    const std::string logFile = "test_engine.log";
    std::error_code ec;
    std::filesystem::remove(logFile, ec);

    Logger::Init(logFile);
    LOG_TRACE("Trace message: {}", 42);
    LOG_INFO("Info message: test running");
    LOG_WARN("Warn message with value: {:.2f}", 3.14159f);
    ENGINE_LOG(Error, Physics, "Physics error simulated on body ID {}", 99);
    Logger::Shutdown();

    // Verify log file was written
    assert(std::filesystem::exists(logFile));
    {
        std::ifstream file(logFile);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        assert(content.find("[INFO][Core] Info message: test running") != std::string::npos);
        assert(content.find("[ERROR][Physics] Physics error simulated on body ID 99") != std::string::npos);
    }

    std::filesystem::remove(logFile, ec);
}

Result<int, EngineError> Divide(int a, int b) {
    if (b == 0) {
        return Err(EngineError("Division by zero", 101));
    }
    return a / b;
}

Result<void, EngineError> Action(bool succeed) {
    if (!succeed) {
        return Err(EngineError("Action failed", 500));
    }
    return Ok;
}

void TestResultAndError() {
    // Success path
    auto okRes = Divide(10, 2);
    assert(okRes.HasValue());
    assert(static_cast<bool>(okRes));
    assert(*okRes == 5);
    assert(okRes.ValueOr(0) == 5);

    // Error path
    auto errRes = Divide(10, 0);
    assert(!errRes.HasValue());
    assert(!static_cast<bool>(errRes));
    assert(errRes.Error().code == 101);
    assert(errRes.Error().message == "Division by zero");
    assert(errRes.ValueOr(42) == 42);

    // Void success
    auto voidOk = Action(true);
    assert(voidOk.HasValue());

    // Void error
    auto voidErr = Action(false);
    assert(!voidErr.HasValue());
    assert(voidErr.Error().code == 500);
    assert(voidErr.Error().message == "Action failed");
}

int main() {
    std::cout << "[TestCoreFoundation] Running Simplified Core Tests...\n";
    TestTypesAndConstants();
    std::cout << "  -> Types & Constants: PASSED\n";
    TestAssertions();
    std::cout << "  -> Assertions: PASSED\n";
    TestLogging();
    std::cout << "  -> Structured Logging: PASSED\n";
    TestResultAndError();
    std::cout << "  -> Zero-Exception Result<T, E>: PASSED\n";
    std::cout << "[TestCoreFoundation] All Tests Passed Successfully!\n";
    return 0;
}
