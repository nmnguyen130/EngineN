# EngineN 2026 — Industrial Clean-Slate C++23 2D Data-Oriented Game Engine

## Clean-Slate Engineering Highlights
- **Windows (MinGW-w64 GCC & MSVC) First-Class Compatibility**:
  - Uses C++23 over-aligned `::operator new(alignedCap, std::align_val_t{64}, std::nothrow)` and `::operator delete(m_base, std::align_val_t{64})` instead of C11 `std::aligned_alloc` (unsupported by Microsoft UCRT).
  - Enforces `kMaxArenaBytes` (`512 MB`) upper-bound invariants in `AlignedArena::Init` and `AlignedArena::Carve<T>` with `std::fill_n(raw, alignedCap, u8{0})`, eliminating GCC `-Werror=stringop-overflow=` range-analysis false positives.
- **Zero Dummy Padding (`pad[]`) & Strict Byte Layout**:
  - Every engine and game struct orders fields naturally by alignment and verifies layout via `static_assert(sizeof(...))` without a single dummy `pad[]` array.
- **Modular 12-Subsystem Directory Hierarchy**:
  - `engine/core/` (`Types.h`, `Math.h`, `Memory.h`, `Containers.h`, `Serialization.h`, `JobSystem.h/.cpp`)
  - `engine/time/` (`TimeTypes.h`, `TimePipeline.h/.cpp`) — Separated from `core/`, featuring a 32-byte `SimStepView`, 48-byte `RenderTimeView`, pause-aware `FrozenMask()` + `RequestStep()`, hit-stop, 8-channel dilation, `TimerWheel`, and `TweenManager`.
  - `engine/platform/` (`PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp`, `sdl3_shim/SDL3/`)
  - `engine/input/` (`InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h/.cpp`)
  - `engine/assets/` (`AssetTypes.h`, `AssetBank.h/.cpp`, `AssetSystem.h/.cpp`)
  - `engine/world/` (`WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`)
  - `engine/physics/` (`PhysicsTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`)
  - `engine/gameplay/` (`GameplayTypes.h`, `CombatAndStateSolver.h/.cpp`, `AiAndNavigation.h/.cpp`, `ParticleAndCamera.h/.cpp`, `GameplayPipeline.h/.cpp`)
  - `engine/renderer/` (`RendererTypes.h`, `RenderList.h/.cpp`, `LightingAndPostFx.h/.cpp`, `UiBuilder.h/.cpp`, `Renderer2D.h/.cpp`)
  - `engine/audio/` (`AudioTypes.h`, `AudioMixer.h/.cpp`, `AudioSystem.h/.cpp`)
  - `engine/debug/` (`DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`)
  - `engine/framework/` (`FrameworkTypes.h`, `SceneDirector.h/.cpp`, `Application.h/.cpp`)
- **Structured Client Game (`game/`)**:
  - `game/core/` (`VeilTypes.h`, `VeilAssets.h/.cpp`)
  - `game/systems/` (`VeilPlayerSystem.h/.cpp`, `VeilCombatAndAiSystem.h/.cpp`)
  - `game/runtime/` (`VeilGame.h/.cpp`) and `game/main.cpp`
- **Dedicated Per-Subsystem Verification (`tests/subsystems/`)**:
  - 6 standalone subsystem test suites (`TestCoreAndTime.cpp`, `TestPlatformAndInput.cpp`, `TestAssetsAndWorld.cpp`, `TestPhysicsAndGameplay.cpp`, `TestRendererAndAudio.cpp`, `TestDebugAndFramework.cpp`) plus `veil_game --test`.

## Build & Verification
### Using Make (`make clean && make test`)
```bash
make clean
make test
```

### Using CMake
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```
