# EngineN — Development Tasks & Roadmap
### High-Speed 2D Action Metroidvania Engine (C++20 / SDL3)

---

## 📌 Context for Future AI Agents / Chats
> **Read this section first if you are picking up this project in a new session:**
>
> 1. **Tech Stack:** Modern C++ (C++20), SDL3 (built automatically via CMake FetchContent), Ninja generator, GCC 16 (w64devkit).
> 2. **Build & Run:**
>    - Configure: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug` (optional: set `$env:ENGINE_NAME="MyGame"` to customize binary/window name)
>    - Compile: `cmake --build build`
>    - Run: `.\build\EngineN.exe` (or `.\build\<ENGINE_NAME>.exe`)
> 3. **Core Philosophy:**
>    - **4 Golden Boundaries:** `Input ≠ State`, `State ≠ Physics`, `Gameplay ≠ Presentation`, `Physics Correctness ≠ Player Forgiveness`.
>    - **Simulation:** Strict 120Hz Fixed Timestep (`uint64_t` nanoseconds accumulator, spiral-of-death clamp at 8 ticks).
>    - **Rendering:** Variable refresh rate (60/120/144/240Hz/Uncapped) with Cubic Hermite & Lerp state interpolation (`alpha`).
>    - **No Rigidbody Box2D:** Uses Kinematic Body + Minkowski Swept AABB.
> 4. **Key Documents:**
>    - [`architecture.md`](file:///d:/Coding/Game/EngineN/architecture.md): Complete gameplay & player system architecture.
>    - `TASKS.md`: Current progress, task checklist, and next steps.

---

## 🚀 Progress Tracker

### Milestone 1: Time Loop & Core Runtime (COMPLETED ✅)
- [x] C++20 & SDL3 project build setup with CMake + Ninja (`CMakeLists.txt`).
- [x] Basic window initialization and SDL3 linking test.
- [x] Precision clock (`src/core/time/Clock.h`): `HighResolutionClock` using `uint64_t` nanoseconds, `TimeChannel`.
- [x] Multi-channel Hitstop engine (`src/core/time/HitstopEngine.h/.cpp`): Non-stacking freeze-frame requests + smoothstep recovery ramp.
- [x] Hybrid sleep + spin Frame Pacer (`src/core/time/FramePacer.h/.cpp`): `SDL_DelayNS` down to 1.5ms + `SDL_CPUPauseInstruction()`.
- [x] Central Time Manager (`src/core/time/TimeManager.h/.cpp`): 120Hz fixed accumulator, 8-tick safety clamp, alpha calculation, multi-channel scaling (`RealTime`, `WorldTime`, `PlayerTime`, `EnemyTime`).
- [x] State Interpolation buffer (`src/core/interpolation/InterpolationBuffer.h`): Dual Lerp & Cubic Hermite velocity-aware blending.
- [x] Master Runtime Loop (`src/main.cpp`):
  - Fully decoupled FixedUpdate(120Hz) vs Render(Variable).
  - Diagnostic on-screen HUD (FPS, delta ms, ticks per frame, accumulator alpha).
  - Interactive test controls:
    - `I`: Toggle interpolation on/off (A/B testing).
    - `H`: Trigger 100ms hitstop on player.
    - `B`: Toggle 0.25x bullet-time.
    - `1 / 2 / 3 / 0`: Set target FPS (60, 120, 144, Uncapped).

---

### Milestone 2: Platform Subsystem & Input Layer (COMPLETED ✅)
- [x] Subsystem initialization & display management (`src/platform/Platform.h/.cpp`):
  - RAII `SDL_Init` with `SDL_INIT_VIDEO | SDL_INIT_GAMEPAD`.
  - Display enumeration (`DisplayInfo`: native resolution, refresh rate, DPI).
  - Power / battery status query (`GetPowerInfo`).
- [x] Modern Window abstraction (`src/platform/Window.h/.cpp`):
  - RAII `SDL_Window*` wrapper with `SDL_WINDOW_HIGH_PIXEL_DENSITY` High-DPI support.
  - Display modes: `Windowed`, `BorderlessFullscreen`, `ExclusiveFullscreen`.
  - DPI scale queries (`GetDPIScale()`), logical vs framebuffer pixel resolution handling.
  - Window lifecycle and focus state machine.
- [x] Single-Source Event Pump (`src/platform/EventPump.h/.cpp`):
  - Strictly calls `SDL_PollEvent(&e)` in exactly ONE loop per frame (`PollAll()`).
  - Zero heap allocations per frame, clean routing to window and input subsystem.
- [x] Input Subsystem & Gamepad abstraction (`src/platform/input/Gamepad.h/.cpp`, `InputSystem.h/.cpp`):
  - C++20 RAII `SDL_Gamepad*` wrapper with radial and axial deadzone filtering.
  - Hybrid input architecture: continuous polling (`IsKeyDown`, `GetMouseX`, `GetAxis`) + contextual states (`JustPressed`, `Held`, `JustReleased`).
  - Circular sub-frame timestamped event buffer (`TimestampedAction`, 64 entries) recording hardware `e.timestamp` in nanoseconds.
  - Virtual action mapping (`Jump`, `Attack`, `Dash`, `Parry`, `Interact`, `MenuConfirm`, `MenuCancel`, `MenuPause`) with multi-key and mouse bindings.
- [x] Complete automated test suite (`tests/test_platform.cpp`): 100% PASS on deadzone math, action transitions, timestamped queue, move semantics, window creation/modes.
- [x] Master Runtime Loop integration (`src/main.cpp`): Clean integration with interactive player movement, combat actions, fullscreen toggle (F11), and diagnostic HUD.

---

### Milestone 3: Collision World & Swept AABB Solver (PENDING ⏳)
- [ ] Implement `src/physics/AABB.h`: Axis-Aligned Bounding Box primitives, Minkowski difference.
- [ ] Implement `src/physics/Tilemap.h/.cpp`: Static grid collision broadphase.
- [ ] Implement `src/physics/SweptAABB.h/.cpp`:
  - Ray vs AABB slab algorithm with Time-of-Impact (TOI) and contact normal.
  - Multi-pass slide resolution (up to 4–6 iterations per tick).
  - Sub-pixel remainder accumulation.

---

### Milestone 4: Player Controller & Forgiveness Layer (PENDING ⏳)
- [ ] Implement `src/gameplay/player/KinematicBody2D.h/.cpp`: Velocity integration, surface contacts.
- [ ] Implement `src/gameplay/player/MotionArbiter.h/.cpp`: Priority-based motion request blending (Tiers 10–100).
- [ ] Implement Dual-Track FSM:
  - `LocomotionStateMachine`: Grounded, Airborne, WallSlide, Dash.
  - `ActionStateMachine`: Neutral, Attack, Parry, Hurt.
- [ ] Implement Forgiveness Layer:
  - Coyote Time (100ms ground, 80ms wall).
  - Variable Jump Height (0.45x release cut).
  - Apex Floatiness (0.55x gravity modifier).
  - Head-bump Corner Correction (4–6 px wiggle).
  - Dash Ledge Pop (3–5 px step-up).
  - Wall Jump Lockout (100ms opposite-direction clamp).
  - Floor Snap (3–4 px downward hysteresis).

---

### Milestone 5: Combat Kinetics & Feedback ("Juice") (PENDING ⏳)
- [ ] Down-slash Pogo Bounce & ability refresh.
- [ ] Directional weapon recoil & damage knockback decay.
- [ ] Procedural spring-damper Squash & Stretch.
- [ ] Directional spring-damper camera shake with lookahead.
- [ ] Ghost trail / Afterimage spawner pool.

---

## 🛠️ File Structure Reference

```text
src/
├── core/
│   ├── time/
│   │   ├── Time.h                  // Unified Time system header (TimeLoop alias)
│   │   ├── Clock.h                 // HighResolutionClock + TimeChannel
│   │   ├── HitstopEngine.h / .cpp  // Freeze-frame multi-channel manager
│   │   ├── FramePacer.h / .cpp     // Hybrid sleep+spin FPS limiter
│   │   └── TimeManager.h / .cpp    // Central accumulator & channel state
│   ├── interpolation/
│   │   └── InterpolationBuffer.h   // Lerp & Cubic Hermite transform blending (Reserved for Renderer/Scene)
│   ├── Config.h                    // AppConfig configuration
│   └── input/                      // [Milestone 2]
├── math/
│   └── Vec2.h                      // 2D Vector math
├── physics/                        // [Milestone 3]
├── gameplay/                       // [Milestone 4 & 5]
└── main.cpp                        // Sandbox runtime entry point & loop orchestrator
```
