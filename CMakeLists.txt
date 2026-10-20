cmake_minimum_required(VERSION 3.24)
project(EngineN_2026_Clean VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX /permissive- /GR- /EHs-c-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wstringop-overflow=2 -fno-exceptions -fno-rtti -O2)
endif()

file(GLOB_RECURSE ENGINEN_SOURCES
    "engine/core/*.cpp"
    "engine/time/*.cpp"
    "engine/platform/*.cpp"
    "engine/input/*.cpp"
    "engine/assets/*.cpp"
    "engine/world/*.cpp"
    "engine/physics/*.cpp"
    "engine/gameplay/*.cpp"
    "engine/renderer/*.cpp"
    "engine/audio/*.cpp"
    "engine/debug/*.cpp"
    "engine/framework/*.cpp"
)

add_library(enginen STATIC ${ENGINEN_SOURCES})
target_include_directories(enginen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)

find_package(SDL3 QUIET CONFIG)
if(SDL3_FOUND)
    message(STATUS "EngineN: Linking against native production SDL3::SDL3")
    target_link_libraries(enginen PUBLIC SDL3::SDL3)
else()
    message(STATUS "EngineN: Using built-in Win32/POSIX Window+GDI backend (engine/platform/sdl3_shim)")
    target_include_directories(enginen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine/platform/sdl3_shim)
    if(WIN32)
        target_link_libraries(enginen PUBLIC user32 gdi32)
    endif()
endif()
if(WIN32)
    target_link_libraries(enginen PUBLIC user32 gdi32)
endif()

file(GLOB_RECURSE VEIL_GAME_SOURCES
    "game/core/*.cpp"
    "game/systems/*.cpp"
    "game/runtime/*.cpp"
    "game/main.cpp"
)

add_executable(veil_game ${VEIL_GAME_SOURCES})
target_include_directories(veil_game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(veil_game PRIVATE enginen)

file(GLOB_RECURSE SUBSYSTEM_TEST_SOURCES
    "tests/*.cpp"
    "tests/subsystems/*.cpp"
)

add_executable(enginen_tests ${SUBSYSTEM_TEST_SOURCES})
target_include_directories(enginen_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(enginen_tests PRIVATE enginen)

enable_testing()
add_test(NAME SubsystemSuite COMMAND enginen_tests)
add_test(NAME VeilGameIntegration COMMAND veil_game --test)
