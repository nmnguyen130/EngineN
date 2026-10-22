#!/usr/bin/env bash
set -euo pipefail

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror -I. -Iengine -Ivendor -Ivendor/sdl3_shim"

mkdir -p build/obj build/game_obj build/bin

echo "[1/4] Compiling EngineN 12 Subsystems..."
for f in engine/*/*.cpp; do
    obj="build/obj/$(basename "${f%.cpp}").o"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        "$CXX" $CXXFLAGS -c "$f" -o "$obj" &
    fi
done
wait
rm -f build/libengine.a
ar rcs build/libengine.a build/obj/*.o

echo "[2/4] Compiling Veil of the Silent (GDD v0.2) Client Layer..."
for f in game/*/*.cpp game/VeilGameApp.cpp; do
    obj="build/game_obj/$(basename "${f%.cpp}").o"
    "$CXX" $CXXFLAGS -c "$f" -o "$obj" &
done
wait
rm -f build/libveil_game.a
ar rcs build/libveil_game.a build/game_obj/*.o

echo "[3/4] Linking veil_of_the_silent & VeilOfTheSilentTests..."
"$CXX" $CXXFLAGS game/Main.cpp build/libveil_game.a build/libengine.a -o build/bin/veil_of_the_silent &
"$CXX" $CXXFLAGS tests/game/VeilOfTheSilentTests.cpp build/libveil_game.a build/libengine.a -o build/bin/VeilOfTheSilentTests &
wait

echo "[4/4] Running Verification Suites..."
./build/bin/veil_of_the_silent
./build/bin/VeilOfTheSilentTests
