# Veil of the Silent (GDD v0.2) — End-to-End Production Plan & Architecture Specification

## 1. Executive Summary & EngineN Integration Contract

**Veil of the Silent** is a 2D Action Metroidvania / Precision Combat Platformer built on the locked **EngineN** C++23 runtime (`-std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror`).
All game systems sit strictly in the client layer (`game/`) above EngineN's 12 locked subsystems (`core`, `time`, `platform`, `input`, `renderer`, `assets`, `world`, `physics`, `gameplay`, `audio`, `debug`, `framework`), communicating via `en::Application`, `en::AppHooks`, `en::GameplayPolicyTable`, `en::StateLayerDesc`, and `en::SceneTransitionDesc`.

### Core Architectural Invariants
1. **Zero Engine Intrusion:** All 12 EngineN subsystems remain locked and binary-compatible (`sizeof(Application) == 64`, `sizeof(SceneDirector) == 576`, `sizeof(GameplayPipeline) == 640`, `sizeof(World) == 192`, `sizeof(PhysicsSystem) == 192`, `sizeof(AssetBank) == 256`, `sizeof(AudioSystem) == 128`, `sizeof(DebugSystem) == 64`).
2. **Custom ECS Component IDs (`[24..63]`):** Built-in `world` components use `kTypeId` `0..7`, `physics` uses `8..11`, `gameplay` uses `16..21`. Client game components in `game/components/GameComponents.h` use `kTypeId` `24..33` with `static_assert` byte-layout verification and zero compiler padding holes.
3. **120 Hz Deterministic Simulation (`dt = 1/120 s`):** All timers (420-tick Resonance Marks, 10-tick Parry window, 24-tick Heavy startup, 45-tick Stagger, 150-tick Break State, 240-tick Break Recovery, 720-tick Fracture, 12-tick Chrono-Overdrive delay, etc.) are represented in integer 120 Hz simulation ticks.
4. **Deferred Structural ECS Mutations:** During simulation ticks and combat callbacks, entity creation/destruction and component attachment use `World::DeferSpawn`, `World::DeferDestroy`, `World::DeferEmplace`, and `World::DeferRemove`, flushed deterministically at `World::EndTick()`.

## 2. Standardized Folder & File Structure

```text
EngineN/
├── CMakeLists.txt
├── build_and_test.sh
├── data/
│   └── echo_pantheon.json
├── docs/
│   ├── VeilOfTheSilent_GDD_v0.2.md
│   └── VeilOfTheSilent_Production_Plan_v0.2.md
├── engine/                          # Locked 12-subsystem C++23 runtime
├── game/                            # Veil of the Silent client game layer
│   ├── core/
│   │   ├── GameTypes.h              # EchoId (0..11), WaveReactionId (1..10), SacredChordId, TriadDeck, CC/Break/Gate enums
│   │   ├── GameConfig.h             # 120 Hz frame-data constants, Echo table, Reaction formula table, Sacred Chord table
│   │   └── SaveSystem.h/.cpp        # Deterministic binary SaveData ("VOTS" v1) with CRC32/FNV-1a validation & atomic I/O
│   ├── components/
│   │   └── GameComponents.h         # Custom ECS components (kTypeId 24..33, 0B padding, trivially copyable)
│   ├── assets/
│   │   └── GameAssetPipeline.h/.cpp # Procedural pixel-art textures, grid atlases, ERIG v1 rigs, ESCN v1 manifests, ECOL v1 maps, PCM audio
│   ├── player/
│   │   └── PlayerController.h/.cpp  # InputMap bindings, 3-hit ground / 2-hit air / 24-tick heavy, Echo capture, 10-tick Parry, 1 Air-Dash
│   ├── combat/
│   │   └── CombatSystem.h/.cpp      # 420-tick ResonanceMark, 10 Signature Reactions (12 pairs), Break/Stagger/Recovery, CC priority, Harmony/Unison/Sacred Chords
│   ├── ai/
│   │   └── EnemyAiSystem.h/.cpp     # Standard Husk, Heavy Enforcer (BreakableGuard), Sentinel Projectile, & High Conductor 3-Phase Boss AI
│   ├── world/
│   │   └── MetroidvaniaSystem.h/.cpp# Tuning Resonators, Lost Wick Husk (single-husk rule + 25% HP penalty), Frequency Gates, Fog of War, Map Pins, 3 Endings
│   ├── ui/
│   │   └── HudAndMenuSystem.h/.cpp  # Accessible HUD (Echo glyphs + names, Mark icons, HP/Break/Harmony), Resonator Deck UI, Map Pin UI, Pause/Ending overlays
│   ├── scenes/
│   │   └── GameScenes.h/.cpp        # State layers (Title, Gameplay, ResonatorModal, PauseModal, EndingModal) & multi-room Vaelen transitions
│   ├── debug/
│   │   └── GameDebugTools.h/.cpp    # Live CVars, console commands (echo_unlock, harmony_set, trigger_ending, etc.), and Profiler WatchPlots
│   ├── VeilGameApp.h/.cpp           # Top-level client coordinator wiring AppHooks, GameplayPolicyTable, and Application
│   └── Main.cpp                     # Production entry point supporting interactive window or --headless verification run
└── tests/
    ├── subsystems/                  # 12 locked EngineN subsystem test suites
    └── game/
        └── VeilOfTheSilentTests.cpp # End-to-end GDD v0.2 verification suite (all 12 reactions, parry, break, husk, gates, save/load, endings, determinism)
```

## 3. Phased Production Plan, Dependencies & Acceptance Criteria

### Phase 0: Due Diligence, GDD v0.2 & Engine Codebase Audit
- **Tasks:** Inspect GDD v0.2 and all 12 EngineN subsystems; verify compiler and SDL3 headless shim; run all 12 existing subsystem test suites.
- **Acceptance Criteria:** All 12 engine test suites (`CoreTests` through `FrameworkTests`) compile with `-std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror` and pass with 0 failures.

### Phase 1: Architecture Standardization, Configuration, Save/Load & Asset Pipeline
- **Dependencies:** Phase 0 (`core`, `assets`, `platform`, `gameplay`, `physics`, `audio`, `framework`).
- **Tasks:**
  1. Implement `game/core/GameTypes.h` and `game/core/GameConfig.h` with `constexpr` tables for all 12 Echoes (`Ignis`..`Stridor`), 10 Signature Reactions across 12 unordered Echo pairs, 4 Sacred Chords, and prototype combat/movement constants.
  2. Implement `game/components/GameComponents.h` with 0B-padded ECS components (`PlayerStateComp`, `WeaponAttackComp`, `ResonanceMarkComp`, `BreakAndCcComp`, `EnemyAiComp`, `ProjectileComp`, `InteractableComp`, `DestructibleTerrainComp`).
  3. Implement `game/core/SaveSystem.h/.cpp` for deterministic binary serialization (`SaveData` with FNV-1a checksum, unlocked Echo mask, `TriadDeck`, `LostWickHuskRecord`, world flags, discovered room mask, user `MapPin` array, and `Platform::WriteFileAtomic` / `ReadFileBuffer`).
  4. Implement `game/assets/GameAssetPipeline.h/.cpp` to cook and register procedural RGBA8 textures, grid atlases (`CommitGridAtlas`), `"ERIG"` v1 combat state machine blobs (`BlobHandle`), `"ESCN"` v1 scene manifests, and synthesized PCM audio cues/music stems into `AssetBank` & `AudioSystem`.
- **Acceptance Criteria:**
  - `static_assert` verifies zero padding and `Component` concept compliance on every custom component.
  - All 12 Echo definitions, 12 reaction formulas, and 4 Sacred Chords resolve in `constexpr` tests.
  - `SaveSystem` round-trips save files and rejects corrupted checksums cleanly.
  - `GameAssetPipeline` populates `AssetBank` and `AudioSystem` with valid handles and `"ERIG"` / `"ESCN"` blobs.

### Phase 2: Input, Player Controller, Movement, Gravity & Physics Integration
- **Dependencies:** Phase 1 (`input`, `physics`, `gameplay`).
- **Tasks:**
  1. Bind keyboard + gamepad controls in `InputMap` (`MoveX`, `MoveY`, `Jump`, `AttackLight`, `AttackHeavy`, `Dash`, `Parry`, `CycleEchoNext`, `CycleEchoPrev`, `UnisonBurst`, `SacredChord`, `Interact`, `Pause`, `ToggleMap`).
  2. Implement Kaelen's locomotion & air mobility in `PlayerController`: ground walk/run, coyote jump & wall jump via `MotionStack2D::TryExecuteJump`, variable jump cut (`MotionFlags::JumpHeld`), one-way platform drop-through, and the strict **1 Air-Dash charge per airborne sequence** rule (restored only on valid ground contact, never by Signature Reactions).
  3. Implement single-weapon `Resonance Blade` attack state machine (`Light 1 -> Light 2 -> Light 3` = `10 / 12 / 18` HP Damage, `4 / 5 / 9` Break; `Air 1 -> Air 2` = `10 / 15` HP Damage, `4 / 7` Break; `Heavy` = `24`-tick startup, `24` HP Damage, `18` Break) with `capturedEchoId` latched on attack start and preserved across mid-swing Echo switches.
  4. Integrate `SafeAnchor2D` hazard recovery and 10-tick directional/telegraph Parry window.
- **Acceptance Criteria:**
  - Switching Echoes changes weapon tint/VFX and future mark `EchoId` without altering damage, break, startup, recovery, or hitbox size.
  - Mid-swing Echo switch does not change the `capturedEchoId` of an already started attack instance.
  - Air-Dash can be used once in the air, cannot be used a second time before landing, and is not refunded by Signature Reactions.

### Phase 3: Combat, Resonance Marks, 10 Signature Reactions, Break/CC, Parry, Harmony & AI/Bosses
- **Dependencies:** Phase 2 (`gameplay`, `world`, `physics`).
- **Tasks:**
  1. Implement `CombatSystem`:
     - Tick `ResonanceMarkComp` (`420` ticks) and expire marks reaching `0` *before* hit/reaction evaluation on that tick.
     - Same-Echo hit refreshes mark to `420` ticks; valid unordered pair triggers the exact Signature Reaction once, consumes both marks, and grants `+10 Harmony`; invalid pair replaces mark with `420` ticks.
     - Implement all 10 Signature Reactions (`Melt-Shock`, `Piezo-Quake`, `Steam Cavitation`, `Superconductor Gale`, `Pyro-Cyclone`, `Tectonic Shatter` [2 pairs], `Eclipse Singularity`, `Graviton Quake`, `Chrono-Overdrive` [12-tick delayed hit], `Sanguine Boil` [2 pairs]) with exact radius, target caps, secondary damage, Break, CC, `BreakableGuard` shatter (`90` ticks `GuardBroken`), and projectile clearing.
     - Ensure primary killing hits still resolve area/secondary reactions before entity despawn, and secondary/delayed hits never apply marks, trigger recursive reactions, or grant Harmony.
     - Implement independent HP Damage (with multiplicative `Fracture 1.15x` and `Break State 1.25x` = `1.4375x`), `Break Meter (0..100)`, `Standard Stagger (45 ticks)`, `Elite/Boss Break State (150 ticks)`, `Elite/Boss Break Recovery (240 ticks)` (ignoring Break gain during Break State & Recovery), and CC priority (`Break State > Stun > Micro-stagger`, plus `Slow`).
     - Implement `Parry` (`10`-tick window against `Gold Harmonic Wave`, `+15 Harmony`, `+18 Break`, `+12 Phase-Inversion Bonus Break` = `30` when matching `CounterEchoId`, failure against `Crimson Overload Wave` or outside window with zero hidden block/iframe).
     - Implement `HarmonyGauge (0..100)`, `Standard Unison Burst` (`50 Harmony`, `3m` radius, `5` targets, `18` Damage, `15` Break, `30`-tick Standard interrupt), and all 4 `Sacred Chords` (`100 Harmony`, unordered Triad Deck check: `Elemental Cataclysm`, `Cryo-Tempest Protocol`, `Eclipse of Eternity`, `Singularity Overdrive`).
  2. Implement `EnemyAiSystem`:
     - `Standard` Dissonant Husk (`100 HP`, `100 Break`)
     - `Heavy/Elite` Choir Enforcer (`300 HP`, `100 Break`, `BreakableGuard`, displacement resistance, Gold/Crimson telegraphs)
     - `Boss` High Conductor of the Spire (`1200 HP` per phase, `100 Break`, `CounterEchoId` Gold Harmonic waves, Crimson Overload waves, protected & normal projectiles, multi-phase transitions).
- **Acceptance Criteria:**
  - Every item in GDD v0.2 Section 6.5 (Recommended Test Cases) passes deterministic unit & integration assertions.

### Phase 4: Metroidvania World, Rooms, Camera, Audio, UI & Debugging
- **Dependencies:** Phase 3 (`framework`, `renderer`, `audio`, `debug`).
- **Tasks:**
  1. Implement `MetroidvaniaSystem`:
     - `Tuning Resonator` checkpoint (heals HP, sets Harmony to `100`, saves state, configures `TriadDeck` and `ResonanceSigils`).
     - `Lost Wick Husk` death/corpse-run mechanic (respawns at last Resonator, `0` Harmony, `-25%` Max HP cap non-stacking, single Husk rule accumulating Tuning Shards across consecutive deaths, Husk recovery restoring Shards, Max HP cap without overhealing, and `100` Harmony).
     - `Acoustic Fog of War` room discovery and player-placed `MapPin` system (`FrequencyGate`, `UnreachedLedge`, `LoreTablet`, `BossArena`, `TuningForkQuest`).
     - `Frequency Gate` progression checks using permanent world traversal unlocks / world flags (`GameplayPipeline::SetWorldFlag` / `TestWorldFlag`), Lore Tablets, Memory Tuning quests, and the 3 Endings (`The False Dawn`, `The Shattered Spire`, `Symphony of the Off-Beat`).
  2. Implement multi-room Vaelen world (`Ash-Sump Slums`, `Resonator Sanctum`, `Echo Vault`, `Spire Boss Arena`) with `SceneDirector` `RoomSwap` and `FullReset` transitions, `CameraController2D` room confiners/trauma, `AudioSystem` spatial cues & music crossfades, `HudAndMenuSystem` accessible color+glyph+name rendering, and `GameDebugTools` CVars/commands/WatchPlots.
- **Acceptance Criteria:**
  - Full playable and headless-verifiable game loop across all rooms, states, checkpoints, corpse runs, combat encounters, and 3 endings.

### Phase 5: Comprehensive Verification, Headless Integration Tests & Final Deliverables
- **Dependencies:** Phases 1–4.
- **Tasks:**
  1. Build `VeilOfTheSilentTests` covering all GDD v0.2 systems and edge cases + all 12 EngineN subsystem test suites.
  2. Build standalone `veil_of_the_silent` game executable (`--headless` smoke & replay verification mode + interactive mode) and update `CMakeLists.txt` + `build_and_test.sh`.
  3. Verify deterministic `StepHeadless` + `InputReplayStream` recording/playback (`FrameAuditDigest` bit-exact equality).
- **Acceptance Criteria:**
  - 100% of engine and game tests compile with zero warnings (`-Wall -Wextra -Wpedantic -Werror`) and pass with 0 failures.
