cmake_minimum_required(VERSION 3.20)
project(EngineN CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Automatically fetch and link latest SDL3
include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

# Engine source files
set(ENGINE_SOURCES
    src/main.cpp
    src/core/Log.cpp
    src/core/time/HitstopEngine.cpp
    src/core/time/FramePacer.cpp
    src/core/time/TimeManager.cpp
    src/platform/Platform.cpp
    src/platform/Window.cpp
    src/platform/EventPump.cpp
    src/platform/input/Gamepad.cpp
    src/platform/input/InputSystem.cpp
)

add_executable(EngineN ${ENGINE_SOURCES})
target_include_directories(EngineN PRIVATE src)
target_link_libraries(EngineN PRIVATE SDL3::SDL3)

# Automatically copy SDL3.dll to executable directory post-build
add_custom_command(TARGET EngineN POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL3::SDL3-shared>
    $<TARGET_FILE_DIR:EngineN>
)

# Automated Verification Test Suites
set(TEST_TIMELOOP_SOURCES
    tests/test_timeloop.cpp
    src/core/time/HitstopEngine.cpp
    src/core/time/FramePacer.cpp
    src/core/time/TimeManager.cpp
)
add_executable(TestTimeLoop ${TEST_TIMELOOP_SOURCES})
target_include_directories(TestTimeLoop PRIVATE src)
target_link_libraries(TestTimeLoop PRIVATE SDL3::SDL3)

set(TEST_FOUNDATION_SOURCES
    tests/test_core_foundation.cpp
    src/core/Log.cpp
)
add_executable(TestCoreFoundation ${TEST_FOUNDATION_SOURCES})
target_include_directories(TestCoreFoundation PRIVATE src)
target_link_libraries(TestCoreFoundation PRIVATE SDL3::SDL3)

set(TEST_PLATFORM_SOURCES
    tests/test_platform.cpp
    src/core/Log.cpp
    src/platform/Platform.cpp
    src/platform/Window.cpp
    src/platform/EventPump.cpp
    src/platform/input/Gamepad.cpp
    src/platform/input/InputSystem.cpp
)
add_executable(TestPlatform ${TEST_PLATFORM_SOURCES})
target_include_directories(TestPlatform PRIVATE src)
target_link_libraries(TestPlatform PRIVATE SDL3::SDL3)
