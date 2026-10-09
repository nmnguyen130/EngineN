# EngineN — Context & Architecture Specification (Milestones 01 & 02)
### High-Performance 2D Action Metroidvania Engine in Modern C++20 & SDL3

---

## 1. Engine Overview & Core Philosophy

**EngineN** is a custom, high-speed 2D Action Metroidvania game engine built from scratch in **Modern C++20** and **SDL3** (tailored for responsive combat feel akin to *Hollow Knight*, *Dead Cells*, *Celeste*, and *Prince of Persia: The Lost Crown*).

### The 4 Golden Architectural Boundaries:
1. **Input ≠ State**: Input records raw intent and sub-frame hardware timestamps; game state only updates during deterministic simulation ticks.
2. **State ≠ Physics**: Kinematic motion arbitration and collision resolution update velocities and positions; gameplay states react to results.
3. **Gameplay ≠ Presentation**: Simulation runs at a strict **120Hz Fixed Timestep**; rendering runs at an arbitrary, variable refresh rate (**60 / 120 / 144 / 240Hz / Uncapped**) using **Sub-Frame Alpha Interpolation** (`alpha = accumulator / fixed_dt`).
4. **Physics Correctness ≠ Player Forgiveness**: Pure mathematical simulation is arbitrated by a player-forgiveness layer (Coyote time, jump buffering, corner correction, ledge pop).

### Non-Negotiable Engine Constraints:
- **Zero-Exception Policy**: The engine compiles with zero C++ exceptions. All fallible operations return `EngineN::Result<T, EngineError>`.
- **Zero Heap Allocation on Hot Path**: The per-frame loop (event pump, input queries, simulation updates, render command recording, and batch flush) must NOT perform per-frame dynamic heap allocations (`malloc`, `new`, dynamic `std::vector` resizing). Static pre-allocation, ring buffers, and value semantics are strictly enforced.
- **Modern C++20 Standard**: Relies on C++20 features (`std::source_location`, concepts, designated initializers, spaceship operator `<=>`, constexpr math).
- **Toolchain**: MinGW-w64 GCC 16 (w64devkit), CMake 3.28+, Ninja build generator.
- **Language**: English only in comments, code symbols, and documentation.

---

## 2. Implemented Subsystems & APIs

### 2.1 Core Foundation (`src/core/`) — Milestone 01 (✅ 100% DONE & Verified)

#### `Types.h` & `Constants.h`
- Explicit fixed-width primitive aliases: `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64`, `usize`.
- Memory size user-defined literals: `1024_KB`, `16_MB`.
- Math constants: `kPi`, `kTwoPi`, `kHalfPi`, `kDegToRad`, `kRadToDeg`, `kEpsilon`.

#### `Assert.h`
- High-performance zero-overhead assertions using `std::source_location`:
  - `ENGINE_ASSERT(cond, msg)`: Active in Debug/Dev builds.
  - `ENGINE_VERIFY(cond, msg)`: Always evaluates condition, panics on failure.
  - `ENGINE_PANIC(msg)`: Fatal unrecoverable termination.
  - `ENGINE_UNREACHABLE()`: Compiler optimization hint / trap.

#### `Log.h` / `Log.cpp`
- Thread-safe, structured console logger with ANSI colors:
  - Severities: `Trace`, `Debug`, `Info`, `Warn`, `Error`, `Fatal`.
  - Subsystems: `Core`, `Platform`, `Render`, `Physics`, `Audio`, `Game`.
  - Macro: `ENGINE_LOG(Level, Subsystem, format, ...)` formatting via C++20 `std::format` / string view.

#### `Error.h` & `Result.h`
- Monadic, zero-exception error handling:
  - `struct EngineError { std::string_view message; i32 code; std::source_location location; };`
  - `template<typename T, typename E = EngineError> class Result;`
  - Convenience helpers: `Ok`, `Err(error)`, `res.Value()`, `res.Error()`, `res.ValueOr(defaultVal)`.

#### `math/Vec2.h`
- 2D floating-point vector: `Vec2(f32 x, f32 y)`.
- Methods: `Length()`, `LengthSquared()`, `Normalized()`, `Dot()`, `Cross()`, arithmetic operators, `Lerp(a, b, t)`.

#### `core/time/` — Deterministic High-Precision Time System
1. **`Clock.h`**:
   - `HighResolutionClock`: Uses SDL3 nanoseconds directly (`SDL_GetTicksNS()`) as 64-bit unsigned integers. Zero frequency conversion drift.
   - `TimeChannel`: Multi-channel time-scaling (`RealTime`, `WorldTime`, `PlayerTime`, `EnemyTime`) with runtime modifiers (`SetTimeScale()`, `SetModifier()`, `CommitTick()`).
2. **`HitstopEngine.h/.cpp`**:
   - Multi-channel combat freeze-frame manager.
   - Features: Non-stacking merge-by-max duration, smoothstep recovery ramp, pause-aware decay.
3. **`FramePacer.h/.cpp`**:
   - Sub-millisecond hybrid sleep-spin frame limiter. Uses SDL3 `SDL_DelayPrecise()` for high-precision frame pacing without burning 100% CPU.
4. **`TimeManager.h/.cpp` (TimeLoop)**:
   - Fixed 120Hz simulation accumulator (8,333,333 ns per tick).
   - 8-tick spiral-of-death safety clamp.
   - Accumulator sub-frame alpha calculation (`GetAlpha()` $\in [0.0, 1.0)$) for variable presentation rate.
   - Passed all 46/46 adversarial stress tests in `tests/test_timeloop.cpp`.
5. **`interpolation/InterpolationBuffer.h`**:
   - Dual Lerp and Cubic Hermite spline blending with velocity awareness.

---

### 2.2 Platform & Input Subsystem (`src/platform/`) — Milestone 02 (✅ 100% DONE & Verified)

#### `Platform.h` / `Platform.cpp` & `PlatformTypes.h`
- **RAII Lifecycle**: Handles `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)` and clean `SDL_Quit()`.
- **Display Queries**: Enumerates connected displays (`DisplayInfo`), native resolution, refresh rate (60/120/144/240Hz), and DPI.
- **Power Queries**: System battery state and charge percentage (`GetPowerInfo()`).
- **`WindowMode`**: `Windowed`, `BorderlessFullscreen`, `ExclusiveFullscreen`.

#### `Window.h` / `Window.cpp`
- **RAII `SDL_Window*`**:
  - High-DPI support (`SDL_WINDOW_HIGH_PIXEL_DENSITY`).
  - Window sizing: Tracks both logical size (`GetWidth()`, `GetHeight()`) and framebuffer pixel size (`GetPixelWidth()`, `GetPixelHeight()`).
  - DPI scale queries (`GetDPIScale()`).
  - Runtime fullscreen switching via `SetMode(WindowMode)`.
  - Focus state machine (`HasFocus()`, `IsMinimized()`).

#### `EventPump.h` / `EventPump.cpp`
- **Single-Source of Truth**: `SDL_PollEvent(&e)` is called in **exactly ONE loop per frame** inside `EventPump::PollAll(window, input)`.
- **Zero Allocations**: Events are drained from SDL's OS queue and routed directly to `Window` and `InputSystem` without allocating any heap objects.
- Flags: `WasQuitRequested()`, `WasLowMemory()`, `WasTerminating()`.

#### `platform/input/Gamepad.h` / `Gamepad.cpp`
- **RAII `SDL_Gamepad*`**: Handles connection/disconnection hot-plugging.
- **Deadzone Filtering**:
  - `ApplyRadialDeadzone(rawX, rawY, deadzone)`: True 2D magnitude rescaling to $[0.0, 1.0]$.
  - `ApplyAxialDeadzone(rawVal, deadzone)`: Single-axis deadzone clamp and rescale.
- **Normalized Queries**: Left/Right analog sticks (`DeadzoneResult {x, y}`), triggers, buttons, and rumble feedback.

#### `platform/input/InputSystem.h` / `InputSystem.cpp`
- **Hybrid Input Architecture**:
  1. **Continuous Polling**: `IsKeyDown(scancode)`, `IsMouseButtonDown(button)`, `GetMouseX/Y()`, `GetMouseDeltaX/Y()`, `GetAxis(AxisId::MoveX/MoveY)`.
  2. **Contextual Action States**: `ActionState` (`Idle`, `JustPressed`, `Held`, `JustReleased`).
  3. **Circular Sub-Frame Timestamped Queue** (`TimestampedAction`, capacity = 64): Records raw hardware `e.timestamp` in nanoseconds for frame-rate-independent action buffering (e.g. jump/dash buffering).
- **Virtual Action Mapping**:
  - Virtual actions: `PlayerAction::Jump`, `Attack`, `Dash`, `Parry`, `Interact`, `MenuConfirm`, `MenuCancel`, `MenuPause`.
  - Multi-binding: Multiple keyboard keys, mouse buttons, and gamepad buttons can trigger the same virtual action seamlessly.

---

## 3. Current Engine Runtime Loop (`src/main.cpp`)

Here is how the master loop currently operates in `src/main.cpp`:

```cpp
int main(int argc, char* argv[]) {
    // 1. Initialize Platform
    EngineN::Platform platform;
    auto platRes = platform.Init();
    
    // 2. Create Window
    EngineN::Window window;
    window.Create({ .title = "EngineN", .width = 1280, .height = 720, .highDPI = true, .resizable = true });
    
    // 3. Hardware Renderer handle
    SDL_Renderer* rawRenderer = SDL_CreateRenderer(window.GetRawHandle(), nullptr);
    
    // 4. Input & Event Pump
    EngineN::InputSystem input;
    EngineN::EventPump pump;
    
    // 5. TimeLoop (120Hz Fixed Sim)
    EngineN::TimeLoop time;
    time.Init({ .simulationHz = 120, .targetRenderFps = 0, .maxCatchupTicks = 8 });
    
    bool running = true;
    while (running) {
        // [TIME] Begin frame measurement
        time.BeginFrame();
        
        // [EVENT PUMP & INPUT] Drain SDL events once per frame
        pump.BeginFrame();
        input.BeginFrame();
        pump.PollAll(window, input);
        if (pump.WasQuitRequested() || input.IsActionJustPressed(PlayerAction::MenuPause)) {
            running = false;
        }
        
        // [SIMULATION] Deterministic Fixed Timestep 120Hz Loop
        while (time.ConsumeTick()) {
            double dt = time.PlayerTime().scaledDt;
            if (dt > 0.0) {
                // FixedUpdate logic:
                // 1. Query input: input.GetAxis(AxisId::MoveX), input.IsActionHeld(...)
                // 2. Physics & Kinematic movement
                // 3. Store player.prevPosition before updating player.position
            }
            time.EndTick();
        }
        
        // [RENDER] Variable Rate Presentation with Sub-frame Alpha
        time.EndFrame();
        double alpha = time.GetAlpha(); // In range [0.0, 1.0)
        
        // Visual position via alpha interpolation:
        // Vec2 visualPos = Lerp(player.prevPosition, player.position, static_cast<float>(alpha));
        
        // Present frame...
        
        // [PACER] Frame limiter delay
        time.Pacer().WaitForTarget();
    }
    
    SDL_DestroyRenderer(rawRenderer);
    window.Destroy();
    platform.Shutdown();
    return 0;
}
```

---

## 4. Current File Tree

```text
EngineN/
├── CMakeLists.txt
├── roadmap.md
├── TASKS.md
├── src/
│   ├── main.cpp
│   ├── core/
│   │   ├── Types.h
│   │   ├── Constants.h
│   │   ├── Assert.h
│   │   ├── Log.h / Log.cpp
│   │   ├── Error.h / Result.h
│   │   ├── Config.h
│   │   ├── interpolation/
│   │   │   └── InterpolationBuffer.h
│   │   └── time/
│   │       ├── Clock.h
│   │       ├── HitstopEngine.h / HitstopEngine.cpp
│   │       ├── FramePacer.h / FramePacer.cpp
│   │       ├── TimeManager.h / TimeManager.cpp
│   │       └── Time.h
│   ├── math/
│   │   └── Vec2.h
│   └── platform/
│       ├── PlatformTypes.h
│       ├── Platform.h / Platform.cpp
│       ├── Window.h / Window.cpp
│       ├── EventPump.h / EventPump.cpp
│       └── input/
│           ├── Gamepad.h / Gamepad.cpp
│           └── InputSystem.h / InputSystem.cpp
└── tests/
    ├── test_core_foundation.cpp
    ├── test_timeloop.cpp
    └── test_platform.cpp
```

---

## 5. Next Objective: Milestone 03 — Renderer Subsystem

According to `roadmap.md`:
```text
03. Renderer
    ├── RenderDevice
    ├── RenderContext
    ├── Color / Rect / Transform
    ├── Primitive drawing
    └── Texture
```

### Architectural Boundary Note:
- **Renderer scope**: Pure rendering pipeline (hardware context, VSync, logical presentation scaling, 2D view transform/matrix, primitive & sprite batching, texture resources).
- **Gameplay Camera scope** (Milestone 06): Player tracking, spring-damper smoothing, room clamping, trauma-based screen shake. The Renderer will only consume a `Viewport` and `ViewTransform2D` passed from the camera.
