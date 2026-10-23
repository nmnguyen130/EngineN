cmake_minimum_required(VERSION 3.28)
project(EngineN_VeilOfTheSilent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(ENGINE_USE_SDL3_SHIM "Use headless SDL3 mock shim instead of real SDL3" OFF)
if(ENGINE_USE_SDL3_SHIM)
    add_library(sdl3_shim INTERFACE)
    target_include_directories(sdl3_shim INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl3_shim")
    target_compile_definitions(sdl3_shim INTERFACE EN_SDL3_SHIM_ACTIVE=1)
    add_library(SDL3::SDL3 ALIAS sdl3_shim)
else()
    find_package(SDL3 CONFIG QUIET)
    if(NOT TARGET SDL3::SDL3)
        include(FetchContent)
        FetchContent_Declare(
            SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG main
            GIT_SHALLOW TRUE
            SYSTEM
        )
        FetchContent_MakeAvailable(SDL3)
    endif()
endif()

# Locked 12-Subsystem EngineN Sources
file(GLOB ENGINE_SRCS
    "engine/core/*.cpp"
    "engine/time/*.cpp"
    "engine/platform/*.cpp"
    "engine/renderer/*.cpp"
    "engine/assets/*.cpp"
    "engine/world/*.cpp"
    "engine/physics/*.cpp"
    "engine/gameplay/*.cpp"
    "engine/audio/*.cpp"
    "engine/debug/*.cpp"
    "engine/framework/*.cpp"
)

add_library(engine STATIC ${ENGINE_SRCS})
add_library(enginen ALIAS engine)

set_target_properties(engine PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(engine PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W4 /permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror>
)
target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
target_link_libraries(engine PUBLIC SDL3::SDL3)

# Veil of the Silent (GDD v0.2) Client Game Library & Executable
file(GLOB GAME_LIB_SRCS
    "game/core/*.cpp"
    "game/config/*.cpp"
    "game/assets/*.cpp"
    "game/camera/*.cpp"
    "game/player/*.cpp"
    "game/combat/*.cpp"
    "game/ai/*.cpp"
    "game/world/*.cpp"
    "game/ui/*.cpp"
    "game/scenes/*.cpp"
    "game/debug/*.cpp"
    "game/VeilGameApp.cpp"
)

add_library(veil_game STATIC ${GAME_LIB_SRCS})
set_target_properties(veil_game PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(veil_game PUBLIC engine)

add_executable(veil_of_the_silent game/Main.cpp)
target_link_libraries(veil_of_the_silent PRIVATE veil_game)

# Engine Subsystem & Game Verification Suites
enable_testing()
file(GLOB TEST_SRCS "tests/subsystems/*.cpp")
foreach(src ${TEST_SRCS})
    cmake_path(GET src STEM name)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE engine)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

add_executable(VeilOfTheSilentTests tests/game/VeilOfTheSilentTests.cpp)
target_link_libraries(VeilOfTheSilentTests PRIVATE veil_game)
add_test(NAME VeilOfTheSilentTests COMMAND VeilOfTheSilentTests)

add_test(NAME veil_of_the_silent_headless COMMAND veil_of_the_silent --headless)
