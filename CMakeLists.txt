cmake_minimum_required(VERSION 3.28)
project(EngineN LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SDL3 Dependency
find_package(SDL3 CONFIG QUIET)
if(NOT TARGET SDL3::SDL3)
    include(FetchContent)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.0
        GIT_SHALLOW TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(SDL3)
endif()

# Engine Library
add_library(engine STATIC
    engine/core/Jobs.cpp
    engine/core/Log.cpp
    engine/time/Clock.cpp
    engine/platform/Platform.cpp
    engine/platform/Window.cpp
    engine/assets/AssetBank.cpp
    engine/assets/ImageLoader.cpp
    engine/renderer/Renderer.cpp
    engine/renderer/RenderList.cpp
    engine/world/TransformSolver.cpp
    engine/world/World.cpp
    engine/world/WorldSystems.cpp
)
add_library(enginen ALIAS engine)

find_package(Threads REQUIRED)
target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)
target_link_libraries(engine PUBLIC SDL3::SDL3 Threads::Threads)
target_compile_options(engine PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W4 /permissive- /GR->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror>
)

# Engine Subsystem Test Suites
enable_testing()
foreach(name CoreTests TimeTests InputTests PlatformTests AssetsTests RendererTests WorldTests)
    add_executable(${name} tests/subsystems/${name}.cpp)
    target_link_libraries(${name} PRIVATE engine)
    add_test(NAME ${name} COMMAND ${name})
endforeach()
