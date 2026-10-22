cmake_minimum_required(VERSION 3.28)
project(EngineN LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SDL3 Dependency
include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG main
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(SDL3)

# Engine Subsystem Sources
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
file(GLOB TEST_SRCS "tests/subsystems/*.cpp")
foreach(src ${TEST_SRCS})
    cmake_path(GET src STEM name)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE engine)
    add_test(NAME ${name} COMMAND ${name})
endforeach()
