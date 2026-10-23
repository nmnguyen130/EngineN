# Veil of the Silent — GDD v0.5 Architecture, Combat, AI & Level Design Guide

## 1. Clean Domain Tree Structure & Concise C++23 Naming (SOLID)

```text
game/
├── VeilGameApp.h / .cpp               # 120 Hz fixed-step runtime coordinator & AppHooks bridge
├── Main.cpp                           # Entry point (--windowed / --headless)
├── ai/
│   └── EnemyAiSystem.h / .cpp         # 2D elevation leash, Turn-Delay, Front-Arc Hit & Dual Attack-Tokens
├── assets/
│   └── GameAssetPipeline.h / .cpp     # Procedural sprite/tilemap atlas & manifest cooker
├── camera/
│   └── RoomCameraController.h / .cpp  # Dynamic per-room confiner (640/52 wu margin), lookahead & snap
├── combat/
│   ├── CombatSystem.h / .cpp          # HP, Break, CC, 16-tick Parry, Nail Recoil & Knockback Arc
│   └── WaveReactionResolver.h / .cpp  # 10 Signature Wave Reactions, Unison Burst & 4 Sacred Chords
├── components/
│   └── GameComponents.h               # Cache-aligned (16B/32B, 0B padding) ECS components
├── config/
│   ├── CameraConfig.h                 # Viewport, lookahead, deadband & exact room confiner margins
│   ├── CombatConfig.h                 # Shared weapon frame data, 12 Echoes & 12 Reaction recipes
│   ├── EnemyConfig.h                  # Enemy archetypes, turn delays, cooldowns & dual Attack-Token caps
│   ├── LevelConfig.h                  # Variable-dimension room specs (GDD v0.5) & screen-edge thresholds
│   └── PlayerConfig.h                 # Locomotion, Wall Slide/Jump, Dash, Recoil & Hitstun tuning
├── core/
│   ├── GameConfig.h                   # Umbrella header aggregating game/config/*.h
│   ├── GameTypes.h                    # Core enums, TriadDeck, RoomRole & fixed-width types
│   └── SaveSystem.h / .cpp            # FNV-1a 64-bit checksummed binary save/load serializer
├── debug/
│   └── GameDebugTools.h / .cpp        # Live F1 telemetry & F2 hitbox/hurtbox/confiner overlays
├── player/
│   └── PlayerController.h / .cpp      # Priority input, Wall Slide/Jump, Double Jump & Dash
├── scenes/
│   └── GameScenes.h / .cpp            # Variable-size room builder & modal state stack layers
├── ui/
│   ├── HudAndMenuSystem.h / .cpp      # HUD bars, Triad Deck, crimson hit vignette & World Map
│   └── WorldVfxRenderer.h / .cpp      # 2D Greedy Tilemap Meshing, slash arcs, telegraphs & hit flash
└── world/
    ├── MetroidvaniaSystem.h / .cpp    # Resonators, Single Lost Wick Husk, Shards & 3 Endings
    └── WorldTopology.h / .cpp         # GDD v0.4/v0.5 room graph, Conductor Seals & Memory Tuning
```

---

## 2. 2D Rectangular Greedy Tilemap Meshing & Span-Merged `SweepAabb`

1. **2D Rectangular Greedy Meshing (`game/ui/WorldVfxRenderer.cpp`)**:
   - Contiguous `Solid` and `OneWay` tiles are merged into maximal rectangular slabs (`runW × runH`) before emitting quads.
   - Shared internal edges between adjacent tiles are **never double-drawn**, reducing per-room tilemap quads by **>75%**.
2. **Horizontal Span-Merged `SweepAabb` & `Raycast` (`engine/physics/SpatialIndex2D.cpp`)**:
   - When sweeping or raycasting against the static tilemap, contiguous horizontal tiles `[tx .. runEnd]` with identical `TileMaterialFlags` are merged into a single slab AABB `[tileMinX, tileMinY, tileMaxX, tileMaxY]`, eliminating internal vertical tile seams and reducing narrowphase checks by **4x–10x**.

---

## 3. Overhauled Enemy AI: Facing Lock on Windup, Turn-Around Delay, Front-Arc Hitbox, Dual Attack-Tokens & Overlap

| Mechanism | Constant / Field | Value | Gameplay Behavior |
|---|---|---:|---|
| **Enemy Overlap** | `MotionStack2D::pushboxGroup` | `0U` | Enemies pass through and overlap each other naturally like *Hollow Knight* instead of forming a rigid line |
| **Windup Facing Lock** | `ai.actionTimerTicks > 0` | Locked | Once an attack windup begins, enemy facing is **100% locked** through windup, active, and recovery |
| **Front-Arc Melee Check** | `forwardDx = dx * facing` | `>= -0.15 wu` | Dashing behind an enemy during windup completely evades its front slash |
| **Turn-Around Delay** | `EnemyAiComp::turnDelayTicks` | `18 / 22 ticks` | When Kaelen crosses behind a neutral enemy, it waits `150–183 ms` before flipping and cannot snap-attack |
| **Dual Attack-Token Pools** | `kMaxSimultaneousCommonAttackers` / `Ranged` | `1 Melee + 1 Ranged` | At most 1 melee + 1 ranged common enemy can attack simultaneously (`28-tick` staggered retry when busy) |
| **Post-Attack Cooldown** | `postCooldownTicks` | `102..138 ticks` | `0.85s–1.15s` clear rest window after recovery so single enemies never spam back-to-back swings |

---

## 4. True Screen-Edge Room Transitions (`LevelConfig.h`, `WorldTopology.cpp`, `CameraConfig.h`)

- **Exact Viewport Confiner Margin**: `kCameraHalfViewportMarginX = 640.0f / 52.0f = 12.3077 wu` stops the camera flush with the room's outer boundary (`MinWorldX()` and `MaxWorldX()`).
- **Screen-Edge Doorway Trigger**: `WestExitThresholdX() = MinWorldX() + 0.35f` and `EastExitThresholdX() = MaxWorldX() - 0.35f`, gated by the doorway vertical passage (`y in [0.4 .. 4.3]`). Walking near the inner wall drop-down (`x = 20..22 wu`) or touching the upper wall (`y > 4.3 wu`) **never** triggers an early teleport.
