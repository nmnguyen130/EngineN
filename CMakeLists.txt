cmake_minimum_required(VERSION 3.28)
project(EngineN LANGUAGES CXX)

# Set output directory for the final executable
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG main
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(SDL3)

# Engine Library
add_library(engine STATIC
    # Core
    engine/core/Log.cpp

    # Input
    engine/input/SdlInput.cpp

    # Platform
    engine/platform/Platform.cpp
    engine/platform/Window.cpp

    # Renderer
    engine/renderer/RendererList.cpp
    engine/renderer/Renderer.cpp

    # Assets
    engine/assets/AssetBank.cpp
    engine/assets/AssetSystem.cpp

    # World
    engine/world/TransformSolver.cpp
    engine/world/World.cpp
    engine/world/WorldExtractor.cpp

    # Gameplay
    engine/gameplay/SpatialIndex2D.cpp
    engine/gameplay/KinematicSolver2D.cpp
    engine/gameplay/StateAndCombatSolver.cpp
    engine/gameplay/GameplayPipeline.cpp

    # Audio
    engine/audio/AudioMixer.cpp
    engine/audio/AudioSystem.cpp

    # Framework
    engine/framework/SceneDirector.cpp
    engine/framework/Application.cpp

    # Debug
    engine/debug/DebugBatcher.cpp
    engine/debug/DebugInspector.cpp
    engine/debug/DebugProfilerAndConsole.cpp
    engine/debug/DebugSystem.cpp
)

target_compile_features(engine PUBLIC cxx_std_23)
set_target_properties(engine PROPERTIES CXX_EXTENSIONS OFF)

target_compile_options(engine PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W4 /permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

target_link_libraries(engine PUBLIC SDL3::SDL3)

# Unit Tests
enable_testing()

function(add_engine_test TEST_NAME)
    add_executable(${TEST_NAME} tests/${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE engine)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

add_engine_test(TestTimeLoop)
add_engine_test(TestInput)
add_engine_test(TestPlatform)
add_engine_test(TestRenderer)
add_engine_test(TestAssets)
add_engine_test(TestWorld)
add_engine_test(TestGameplay)
add_engine_test(TestAudio)
add_engine_test(TestFramework)
add_engine_test(TestDebug)
