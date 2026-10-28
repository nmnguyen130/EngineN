---
name: enginen-architecture
description: Architect, refactor, and build C++23 subsystems and client games on EngineN. Trigger whenever the user mentions EngineN, Target Architecture Flow, or any EngineN subsystem (core, time, platform, input).
---
# EngineN Architecture

Enforce subsystem boundaries, data contracts, and runtime invariants across EngineN's foundational layers (`core`, `time`, `platform`, `input`).

## When to Use

- Refactoring, inspecting, or testing EngineN's foundational subsystems (`engine/core`, `engine/time`, `engine/platform`, `engine/input`).
- Aligning foundational subsystems with the Target Architecture Flow.

## Target Architecture Flow

1. **BOOT:**
   `Diagnostics` (sink + `CrashHandler`) -> `SDL_Init` (`VIDEO | GAMEPAD | AUDIO`) -> Mount `.enpak` & `JobSystem::Init` -> Load settings/save (CRC32 check) -> Title / Continue.
2. **FRAME (`Application::Run`):**
   `Platform::Pump` -> Lifecycle (skip render when `!window.State().IsPresentable()`) -> `while (SimStepView step = time.PollTick())` (120 Hz fixed):
     - `InputPipeline::PollTick` -> `ActionBuffer::Feed` -> `World::BeginTick`
     - `if (!step.IsFrozen(channel))`: Game Hooks (AI, Timers) -> Gameplay (Hitboxes -> Policy -> Game HP) -> `PhysicsSystem::Step(singleDt)`
     - `World::EndTick` -> `CueRouter` (`AudioCommandRing` / VFX / Camera)
   -> `WorldExtractor` (per-channel alpha interpolation) -> `Renderer::Submit` -> Present
   -> `AudioDevice::Pull` (independent audio thread).
3. **PERSIST:**
   Checkpoint / Exit -> `SaveSystem` + `WorldSnapshot` (header + payload CRC32, atomic write via temp file + rename, deterministic reload).
4. **ASSETS:**
   Cooked `.enpak` -> Transactional mount -> Async upload / Touch on draw -> LRU evict -> reloadable from pack.

## Foundational Contracts & Invariants

- **Build & Runtime Contract:** C++23 (`-fno-exceptions`, `-fno-rtti`), zero heap allocations on hot frame/tick paths after `Init()`, and zero virtual dispatch.
- **Two-Layer Subsystem Boundary:**
  - **Layer 1 (Pure C++23):** Deterministic data structures and state machines (`TimeTypes.h`, `TimePipeline.h`, `PlatformTypes.h`, `input/*.h`) with zero `<SDL3/...>` or OS header dependencies. Testable at compile time (`consteval`) and in headless CI via `ComputeAuditHash()`.
  - **Layer 2 (Hardware/OS Adapter):** Confines `<SDL3/...>` and OS system calls strictly to `.cpp` implementation files (`SdlClock.cpp`, `Window.cpp`, `Platform.cpp`, `Diagnostics.cpp`).
- **Dependency Direction:**
  - `core/` has zero engine dependencies.
  - `time/` depends only on `core/`.
  - `platform/` depends only on `core/`.
  - `input/` depends only on `core/` (never includes `platform/` headers; ingests `PlatformFrameEvents` via template interface).

## Reference Context Files (Contracts & Invariants)

Load only the reference files needed for the active task via `get_skill_resources`:

- `references/Core_Context.md` — `engine/core/`: `Types.h`, `Math.h`, `Memory.h`, `Containers.h`, `Diagnostics.h/.cpp`, `Jobs.h/.cpp`.
- `references/Time_Context.md` — `engine/time/`: `TimeTypes.h`, `TimePipeline.h`, `SdlClock.h/.cpp`.
- `references/Platform_Context.md` — `engine/platform/`: `PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp`.
- `references/Input_Context.md` — `engine/input/`: `InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h`.

## Gotchas

- Respect subsystem scope constraints when refactoring: only touch `core`, `time`, `platform`, and `input` unless explicitly instructed otherwise.
- Keep `SimStepView` at `32B` (`alignas(32)`): advance channel modulation at the start of the next tick (`FlushPendingAdvance`) rather than rebaking `m_bakedTick` before returning the current tick's view.
