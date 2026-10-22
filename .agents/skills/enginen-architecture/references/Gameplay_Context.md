# Phase 9: `gameplay` Subsystem (v2 Final Production Architecture — Locked)

## 1. Architectural Overview & Clean Decoupling from `physics`
With low-level spatial indexing, raycasting, `MoveAndSlide` character collision, and Soft-Step TGS rigid-body dynamics cleanly isolated inside `physics` (Phase 8), the `gameplay` v2 subsystem focuses exclusively on the 5 high-level 2D gameplay framework pillars:
1. **Orthogonal Motion Blending (`currentWalkVx` vs `impulseVx` vs `pendingLaunchVy`), Coyote/Wall-Jump & Variable Jump Height (`MotionSolver2D`)**
2. **Data-Driven Finite State Machine, Automatic `SpriteAnimator2D` Clip Sync, Action Buffering, Per-Entity Hitstop Freeze & Frame-Accurate `"ERIG"` Timeline Directives (`StateAndCombatSolver2D`)**
3. **Single-Pass Active-Attacker O(1) 64-Bit SplitMix Bloom-Deduplicated Hitbox/Hurtbox Combat, Directional Parry, Auto-Iframes & Clash Arbitration (`StateAndCombatSolver2D`)**
4. **2nd-Order Critically Damped Spring 2D Camera Director (`CameraDirector2D`)**
5. **Lockless `SimCue` Output Routing & 4096-Bit Sparse-Summary World Flags (`GameplayPipeline`)**

## 2. Exact Byte-Tight Memory Layouts & Invariants (`0B` Padding, Zero `using` Aliases)
All structs are laid out with zero hidden compiler padding and verified by compile-time `static_assert`:
- `SimCue` (`32B`, `alignas(32)`): `AssetId assetId (8B)`, `f32 worldX (4B)`, `f32 worldY (4B)`, `EntityId emitter (4B)`, `u16 magnitude_q16 (2B)`, `u16 durationTicks (2B)`, `SimCueKind kind (1B)`, `u8 channel (1B)`, `u16 userTag (2B)`.
- `CombatHitEvent` (`32B`, `alignas(32)`): `EntityId attacker`, `EntityId defender`, `f32 hitPointX`, `f32 hitPointY`, `f32 knockbackVx`, `f32 knockbackVy`, `i16 damageApplied`, `u16 attackTag`, `u8 hitstopTicks`, `HitOutcome outcome` (`Damaged`, `ArmorAbsorbed`, `Parried`, `Clashed`), `u8 defenderDied`, `u8 poiseBroken`.
- Cooked `"ERIG"` v1 Binary Format:
  - `CookedGameplayRigHeader` (`16B`, `alignas(16)`): `u32 magic (0x47495245 "ERIG")`, `u16 version (1)`, `u16 stateCount`, `u16 directiveCount`, `u16 flags`, `u32 checksum`.
  - `CookedStateEntry` (`16B`, `alignas(8)`): `u16 durationTicks`, `u16 firstDirective`, `u16 animFirstFrame`, `u16 animFrameCount` (auto-syncs `SpriteAnimator2D` on state entry), `u8 animTicksPerFrame`, `u8 animLoop`, `u8 directiveCount`, `u8 nextStateId`, `CancelCategory defaultCancels`, `CancelCategory onHitCancels`, `u16 stateTag`.
  - `CookedFrameDirective` (`16B`, `alignas(8)`): `CompactAabb2D boxOrMotion (8B)`, `u32 assetHashOrParam (4B)`, `u16 triggerFrame (2B)`, `FrameDirectiveOp op (1B)`, `u8 auxByte (1B)`.
- ECS Gameplay Components (Type IDs `16..21`):
  - `MotionStack2D` (`32B`, `alignas(32)`, `kTypeId = 16` — 2 per cache line): `f32 targetWalkVx`, `f32 currentWalkVx`, `f32 impulseVx`, `f32 pendingLaunchVy`, `i16 overrideVx_q6`, `i16 overrideVy_q6`, `i16 rootMotionDx_q8`, `i16 rootMotionDy_q8`, `u8 overrideTicks`, `u8 activePriority`, `u8 groundCoyoteTicks`, `u8 wallCoyoteTicks`, `u8 oneWayDropTicks`, `u8 pushboxGroup`, `MotionFlags flags`, `i8 wallContactSign`. Includes `TryExecuteJump` for ground coyote jump + directional wall jump.
  - `StateMachine2D` (`16B`, `alignas(16)`, `kTypeId = 17` — 4 per cache line): `BlobHandle rigBlob`, `u16 stateTicks`, `u16 lastAnimFrame`, `CancelCategory cancelMask`, `CancelCategory onHitCancelMask`, `u8 stateId`, `u8 prevStateId`, `u8 hitstopTicks`, `u8 bufferedActionId`, `u8 bufferedActionTicks`, `u8 flags`.
  - `HitboxRig2D` (`32B`, `alignas(32)`, `kTypeId = 18` — 2 per cache line): `u64 hitBloomMask`, `CompactAabb2D hurtbox`, `CompactAabb2D hitbox`, `i16 baseDamage`, `i8 knockbackX_q2`, `i8 knockbackY_q2`, `u8 hitLayerMask`, `u8 poiseDamage`, `u8 packedMeta` (`swingId`, `hurtLayer`, `priority`, `hitstopTicks`), `HitboxFlags flags`.
  - `DamageReceiver2D` (`16B`, `alignas(16)`, `kTypeId = 19` — 4 per cache line): `i32 hp`, `i32 maxHp`, `u16 iframeTicks`, `u16 stunTicks`, `u8 poise`, `u8 maxPoise`, `u8 autoIframeTicks`, `u8 packedTeamAndFlags`.
  - `SafeAnchor2D` (`16B`, `alignas(16)`, `kTypeId = 20` — 4 per cache line): `f32 safeX`, `f32 safeY`, `f32 prevSafeX`, `f32 prevSafeY`.
  - `CameraController2D` (`64B`, `alignas(64)`, `kTypeId = 21` — 1 cache line exact): `EntityId primaryTarget`, `EntityId secondaryTarget`, `f32 anchorX`, `f32 anchorY`, `f32 velX`, `f32 velY`, `f32 latchedGroundY`, `f32 kickOffsetX`, `f32 kickOffsetY`, `Rect activeRoomBounds`, `i16 kickVelX_q4`, `i16 kickVelY_q4`, `i16 lookaheadX_q8`, `u16 trauma_q16`, `u8 secondaryWeight_q8`, `u8 deadzoneHalfW_q4`, `u8 deadzoneHalfH_q4`, `CameraCtrlFlags flags`.
- `GameplayPolicyTable` (`24B`, `alignas(8)`), `GameplayConfig` (`32B`, `alignas(16)`), `GameplayPipeline` (`640B`, `alignas(64)`, 10 cache lines exact).