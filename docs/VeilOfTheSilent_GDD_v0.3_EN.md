# VEIL OF THE SILENT — GDD v0.3
## Prototype Systems Addendum

**Document type:** Incremental GDD update. This document does not rewrite v0.2.

**Source of truth:** `VeilOfTheSilent_GDD_v0.2_EN.md` retains the foundational decisions on vision, lore, the Twelve Echoes, reaction recipes, combat, checkpoints, and world progression. This addendum records only required clarifications and implementation specifications. Read together, v0.2 + this update form GDD v0.3.

**Parameter status:** Movement, frame-data, enemy, and Boss values are *prototype baselines* for implementation and playtesting, not final balance.

---

## 1. Changes from v0.2

### Inherited without duplication

- Core vision and Vaelen/Kaelen narrative.
- One shared weapon; Echo selection changes resonance palette/VFX and `EchoId`, not the base moveset.
- Three-Echo Triad Deck; Resonance Marks last up to 420 ticks; ten named reaction types with twelve valid recipes.
- HP Damage, Break, Crowd Control, Harmony, Sacred Chords, checkpoints, and Lost Wick Husk.
- Existing runtime/system concepts from v0.2, except for the clarifications below.

### Clarified rules

1. **Parry Window:** Once Parry input is accepted, a Parry Window lasts 10 ticks beginning on the input tick. A parryable hit is negated only when its actual collision occurs within the window. The player does not need to press exactly 10 ticks before impact. Parry is not buffered.
2. **Echo capture per action:** `EchoId` is captured when an attack or Parry starts. Switching Echo mid-action does not change that action's payload.
3. **Unison refunds:** Standard Unison deducts 50 Harmony at action start; Sacred Chord deducts 100. If Kaelen is hitstunned before Resolve, cancel the action and refund its full cost. No refund occurs after Resolve. Unison target selection is captured at the Resolve tick.

### New in v0.3

- Default controls, input buffers, action priorities, and rebinding.
- Movement/physics baseline, weapon frame data, Parry state, and fixed-tick processing order.
- Enemy attack-data contract, prototype archetypes, and the Ash-Sump Warden test Boss.
- HUD, gameplay feedback, audio, and accessibility requirements.
- Implementation boundaries and vertical-slice acceptance criteria.

---

## 2. Controls & Input Rules

The following bindings are prototype defaults. All gameplay actions must be rebindable; rebinding must not change combat rules. UI must show glyphs matching the active device.

| Action | Keyboard / Mouse | Default gamepad |
|---|---|---|
| Move | `A` / `D` | Left Stick / D-Pad Left/Right |
| Jump | `Space` | `A / Cross` |
| Light Attack | `Mouse 1` | `X / Square` |
| Heavy Attack | `Mouse 2` | `Y / Triangle` |
| Dash | `Left Shift` | `B / Circle` |
| Parry | `F` | `LB / L1` |
| Select Echo slot 1/2/3 | `1` / `2` / `3` | `LT/RT` to cycle slots |
| Previous / next Echo | `Q` / `C` | Tap `LT` / `RT` |
| Standard Unison | `R` | `RB / R1` |
| Sacred Chord | `T` | `D-Pad Down` |
| Interact | `E` | `D-Pad Up` |
| Map | `Tab` | `View / Select` |
| Pause / Menu | `Esc` | `Menu / Start` |

### Input buffering and priority

- Light-chain buffer: **8 ticks**; accept the next input only during the defined end window. Earlier input is discarded.
- Jump Buffer: **10 ticks**; execute on the first tick Grounded becomes valid.
- Parry, Unison, and Sacred Chord are not buffered.
- Same-tick priority: `Pause/Menu > Dead/Respawn/Hitstun lock > Parry > Dash > Jump > Unison > Attack > Interact`. Losing conflicting inputs do not execute on the next tick, except the explicit Jump/Light buffers.
- Echo selection is independent while Kaelen is not Dead/Hitstunned. It does not cancel a running attack or replace its captured `EchoId`.
- Interaction requires an explicit press and a valid target. Select by distance to interaction origin, then lower `InteractPriority`, then lower `EntityId`.
- Baseline analog dead zone: **0.18**. A trigger counts as pressed above **0.55**; both thresholds should be adjustable. Holding a button must not auto-repeat Light/Heavy/Parry/Unison.

---

## 3. Player Movement & Action State

> All values below are prototype baselines. `wu` is a world unit, not a pixel.

### 3.1 Space and movement

- `1 wu = 1 m` for physics/combat tuning. `+X` points right; `+Y` points up in gameplay math.
- Baseline player collider: **1.65 wu** high and **0.55 wu** wide, with its origin at the midpoint between the feet. The collider does not change with Echo/VFX.
- Only `WalkableGround` counts as valid ground and restores Air-Dash. No default fall damage.

| Parameter | Baseline |
|---|---:|
| Max ground speed | `6.5 wu/s` |
| Ground acceleration / braking | `50 / 65 wu/s²` |
| Max air speed | `6.0 wu/s` |
| Air acceleration | `24 wu/s²` |
| Max fall speed | `22 wu/s` |
| Initial jump velocity | `11.5 wu/s` |
| Gravity | `32 wu/s²` |
| Coyote Time | `8 ticks` |
| Jump Buffer | `10 ticks` |

Jump Buffer is retained for up to 10 ticks. Coyote Time permits a jump shortly after leaving an edge but does not restore Air-Dash. Releasing Jump while rising multiplies the current upward velocity by `0.55` on the next tick. Double Jump and Wall Jump are out of scope. Theoretical jump height is approximately `2.06 wu` before collider, collision, and variable-height effects.

### 3.2 Dash

| Parameter | Baseline |
|---|---:|
| Dash speed | `13.5 wu/s` horizontal |
| Duration | `12 ticks` (100 ms) |
| Air-Dash charges | 1 per airborne sequence |
| Recovery lockout | `6 ticks` after Dash |
| Ground Dash cooldown | `24 ticks` after Dash |
| Default i-frames | None |

- Dash direction uses current left/right input; without directional input, use facing. No diagonal/vertical Dash.
- Air-Dash sets vertical velocity to `0` and suspends gravity for 12 ticks. On completion, horizontal velocity becomes `35%` of Dash speed and gravity resumes.
- Ground Dash does not consume an Air-Dash charge. Air-Dash charge is consumed at start and only restored by touching `WalkableGround`. Resonance Reactions, Parry, and Harmony do not restore Dash.
- Collision remains active during Dash: it does not pass through walls, hazards, Boss hitboxes, or locked gates. Dash grants no invulnerability.
- During the 6-tick recovery lockout, movement and a valid grounded jump remain possible, but new Light, Heavy, Parry, Unison, or Dash actions are blocked. Ground Dash also observes its 24-tick cooldown after the current Dash ends.

### 3.3 HP, damage reception, and death

- Baseline Max HP: **100**; no passive healing.
- One `EnemyAttackInstance` deals at most one hit unless `MultiHitPolicy` says otherwise.
- Standard hit protection: **42 ticks**; hitstun: **18 ticks**. Hitstun blocks attack, Parry, Echo selection, interact, and Unison. Dash does not remove hitstun.
- Pits/death volumes, special hazards, and attacks with `IgnoreHitProtection` must be explicitly declared; ordinary hit protection does not implicitly disable them.
- At HP ≤ 0 or on a death volume, enter Dead during hit resolution and ignore the remaining gameplay inputs in that tick. Respawn at the most recently activated checkpoint.
- Under the inherited Lost Wick Husk rule, effective Max HP after death is **75/100** until the Husk is recovered. Saved Echo/unlock/world flags persist; the current Boss encounter resets according to its encounter contract.

### 3.4 Shared weapon frame data

The same frame data applies to all twelve Echoes. `Startup` is the number of ticks before the first active tick; `Active` is the number of ticks during which the hitbox is live; `Recovery` is the post-active time before returning to neutral. These are simulation ticks, not render frames.

| Attack | Startup | Active | Recovery | HP Damage | Break |
|---|---:|---:|---:|---:|---:|
| Ground Light 1 | 5 | 3 | 10 | 10 | 4 |
| Ground Light 2 | 6 | 3 | 10 | 12 | 5 |
| Ground Light 3 | 8 | 4 | 14 | 18 | 9 |
| Air Light 1 | 5 | 3 | 9 | 10 | 4 |
| Air Light 2 | 7 | 3 | 12 | 15 | 7 |
| Heavy | 24 | 4 | 18 | 24 | 18 |

- Light 1 → 2 → 3 uses the same input; buffer only during the final 8 ticks of the current action. The chain may continue after a hit or whiff.
- Air Light has at most two hits per chain. Each `AttackInstanceId` can hit each target at most once.
- Dash or Parry can cancel Light/Heavy only during the final 4 ticks of Recovery; never during Startup/Active. Jump cannot cancel Startup/Active.
- Each attack captures `EchoId` at start; switching Echo affects only later actions.

### 3.5 Parry and Unison timing

- ParryWindow: **10 ticks**, beginning on the accepted input tick. Only a parryable hit whose collision occurs in the window is negated. The first successful hit consumes the window.
- Capture `EchoIdAtParryStart` when Parry begins. On success: `+15 Harmony`, `+18 Break` to an eligible attacker. Matching `CounterEchoId` adds another `+12 Break`. Switching Echo during the window does not change the counter Echo.
- Standard Unison: **12-tick Startup**, **1-tick Resolve**, **18-tick Recovery**; cost **50 Harmony**.
- Sacred Chord: **24-tick Startup**, **1-tick Resolve**, **36-tick Recovery**; cost **100 Harmony**.
- Neither action can be manually cancelled; neither grants i-frames, applies Marks/Reactions, or grants Harmony. If hitstunned before Resolve, cancel and refund the cost. After Resolve, no refund.
- Capture the target set at Resolve. Select up to the target limit in v0.2 by ascending distance from Kaelen, then ascending `EntityId`. If no valid targets remain at Resolve, no HP Damage/Break is dealt, but the cost is still paid.

### 3.6 Fixed-tick processing order

1. Sample input and create action intents.
2. Validate action locks/priorities; accept at most one primary action.
3. Update locomotion, attack, ParryWindow, Unison, and velocity.
4. Integrate physics and solve world-geometry collisions.
5. Resolve attack-target and hit-ParryWindow contacts.
6. Resolve Parry before damage when the same attack instance collides in the same tick.
7. Apply HP Damage/Break and create valid reaction events.
8. Apply reaction payload once; payloads cannot create new Marks/Reactions.
9. Process death/despawn, encounter state, and feedback events.
10. Update timers at end of tick.

Use tick-based timers: `expiresAtTick = currentTick + durationTicks`. An effect created at tick `T` with duration 420 is active from `T` through `T+419` and expires before hit checks at `T+420`. If the engine pipeline differs, a gameplay adapter must preserve equivalent results and must not rely on container iteration order.

---

## 4. Enemy AI & Boss Encounter Contract

### 4.1 Required attack data

Each enemy attack must define: `AttackId`, `WindupTicks`, `ActiveTicks`, `RecoveryTicks`, `HitDamage`, `AttackShape`, `ParryClass` (`Parryable`/`Unparryable`), `Interruptible`, `TargetPolicy`, and `MultiHitPolicy`; `CounterEchoId` is optional. Projectiles additionally require speed, lifetime, collision mask, and interaction/destruction rules.

Standard loop: `Idle/Approach → Telegraph → Active → Recovery → Cooldown/Approach`. A valid Interrupt during Telegraph cancels the attack and enters `InterruptedRecovery`. After Active begins, an attack cannot retarget or change direction unless tracking and its limits are explicit in data.

### 4.2 Prototype enemy archetypes

| Archetype | HP | Break | Damage | Role / main rule |
|---|---:|---:|---:|---|
| Dissonant Drifter | 100 | 100 | 12 | Standard melee; clear telegraph; Break causes 45-tick Stagger. |
| Choir Sentry | 100 | 100 | 10/projectile | Fires independent projectiles with finite lifetime; no auto-tracking after launch. |
| Resonance Brute | 300 | 100 | 20 | Heavy positional pressure; long wind-up; cannot be freely pulled; Break State 150 ticks, Recovery 240 ticks. |
| Tuning Husk / Elite | 300 | 100 | Per attack | Elite test variant; at least one Parryable and one Unparryable attack. |

| Attack | Wind-up | Active | Recovery | Damage | Parry | Behavior |
|---|---:|---:|---:|---:|---|---|
| Drifter — Close Swipe | 18 | 4 | 24 | 12 | Yes | Front arc up to `1.1 wu`; direction locks when Active begins. |
| Sentry — Single Bolt | 30 | One shot | 30 | 10 | Per projectile data | Projectile at `7 wu/s`, lifetime `180 ticks`, straight trajectory. |
| Brute — Heavy Slam | 36 | 6 | 36 | 20 | No | Crimson cue; near-ground danger radius `1.5 wu`, avoid by jumping/leaving the area. |
| Brute — Shoulder Break | 24 | 5 | 30 | 16 | Yes | Horizontal charge `2.0 wu`, stops at valid collision. |

One AttackInstance hits Kaelen at most once unless MultiHitPolicy specifies otherwise. Every damaging attack must telegraph before Active. Parryable and Unparryable cues must differ through shape/pose/audio as well as color. A Boss cannot change attack class after telegraph begins. If multiple hits occur in one tick: declared lethal hazard > higher attack priority > ascending `AttackId`.

### 4.3 Prototype Boss — Ash-Sump Warden

**Test goals:** read telegraphs, Parry the correct attack type, evade Crimson/Unparryable attacks, and use Echoes without requiring a matching Counter Echo.

- **HP:** 1,200 per phase, 2 phases; **Break Meter:** 100.
- Immune to ordinary Stun, forced arena displacement, and forced teleportation.
- Break State: **150 ticks**, receives `1.25x` HP Damage; Break Recovery: **240 ticks**.
- Phase transition: at 0 HP for the current phase; lock damage for **60 ticks**, clear encounter projectiles, reset Break Meter and Marks; next phase starts at 1,200 HP. Transition does not damage the player.
- Arena must provide horizontal dodge space and at least one safe landing route; no Echo-specific traversal is required.

| Attack | Phase | Wind-up | Active | Recovery | Damage | Parry / Counter Echo | Rule |
|---|---|---:|---:|---:|---:|---|---|
| Resonant Cleave | I/II | 30 | 6 | 30 | 18 | Yes / Terra | Front hitbox `2.0 × 1.4 wu`, one hit. Terra adds `+12 Break` beyond the normal Parry reward. |
| Choir Bolts | I/II | 24 | One volley | 36 | 12/projectile | Per projectile / Fulgur | Three projectiles at `-15°/0°/+15°`, `8 wu/s`, lifetime 180 ticks. |
| Crimson Collapse | I/II | 42 | 60 | 36 | 24 | No / — | Near-ground shockwave `0.35 wu` high, advances up to `4.5 wu` at `9 wu/s`; jump or leave the lane. |
| Refracted Sweep | II | 30 | 6 | 26 | 16 | Yes / Glacies | Front hitbox `2.4 × 1.5 wu`, one hit; afterimage is visual only. |

Phase II adds Refracted Sweep; it does not automatically speed up every attack. Do not select the same Unparryable attack twice consecutively while another valid option exists. Every attack must be avoidable by an available movement/jump/Dash/Parry action as applicable. Counter Echo grants Break only; the Boss must remain beatable without matching Echoes.

---

## 5. UI, Feedback, Audio & Accessibility

### Minimum HUD

| Element | Requirement |
|---|---|
| HP | Show current HP and effective Max HP; distinguish damage taken from the Lost Wick Husk HP lock. |
| Harmony | 0–100 gauge; show gains/costs and never exceed 100. |
| Triad Deck | Three Echoes and active slot; each has glyph + palette, not color alone. |
| Resonance Mark | Echo icon and remaining duration on valid targets; signal expiry/replacement. |
| Boss HP/Break | Current phase HP; distinguish Break State from Break Recovery through more than color. |
| Interact prompt | Device-correct glyph; show only when interaction is valid. |

### Feedback and accessibility

- Hit, Break gain, Break State, Parry success/failure, Reaction, and Mark expiry/replacement must have distinct feedback. Do not confuse HP Damage with Break or Reaction with Unison.
- Gold/Crimson telegraphs must differ in pose/shape/audio and color; color alone is insufficient. Player VFX must not fully obscure dangerous telegraphs.
- Echo/Reaction VFX must match the actual `EchoId`; VFX must not change hitbox, direction, or damage. Reducing VFX must preserve essential gameplay information.
- Separate Master, Music, SFX, and Combat Cue volume; subtitles have speaker labels; combat information must not be available through audio alone.
- Screen Shake settings: `0%`, Reduced, Full. Provide reduced flash/repeated-flash options; high-frequency flashing cannot be the only gameplay cue.
- Support keyboard/gamepad rebinding, binding-conflict checks, and correct device glyphs. Hold/toggle support can be added for future hold actions.
- Hit-stop, if added, affects presentation only and must not shift simulation ticks/timers.

---

## 6. EngineN / C++23 — Implementation Addendum

Keep responsibility boundaries clear without requiring a separate source file for every small concept.

- Runtime: C++23, custom EngineN runtime, fixed-step **120 Hz**. Reference compiler flags where supported: `-std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror`.
- Required concepts or equivalents: `EchoId (0..11)`, `TriadDeck`, `ResonanceMark`, `WaveReactionId`, table of 12 valid pairs, `HarmonyGauge`, attack metadata, optional `CounterEchoId`, world flags/checkpoint/save.
- Responsibility areas: Echo/reaction data; weapon attack + hit registration; Mark expiry/reaction resolution; defense/posture/CC; Harmony/Unison; checkpoint/death/world progression.

### Implementation invariants

1. Attack/Parry captures EchoId at action start.
2. At most one Resonance Mark is active per target.
3. Reaction pairs resolve through an order-independent table; payloads do not recursively create Marks/Reactions or grant extra Harmony.
4. `Break` and `Harmony` stay within `0..100`.
5. Timers use fixed ticks and consistent expiry semantics.
6. Boss cannot re-enter Break State during Break Recovery.
7. Failed Parry grants no hidden block/i-frames.
8. Core gameplay must not assume deferred features (unique weapons, Wing Assists, Echo-specific traversal mutations).
9. Checkpoint/death/save must not duplicate or lose Lost Wick Husk/Shards.

### Minimum additional tests

- Same-Echo marking, valid/invalid pairs, exact expiry at tick 420.
- All 12 valid reaction recipes; no recursive reaction from payloads.
- Reaction Damage/Break is applied exactly once.
- Break State/Recovery cannot be locked indefinitely.
- Parry at both boundaries of the 10-tick window; `CounterEchoId` adds exactly `+12 Break`.
- Harmony cap at 100; correct Unison/Sacred Chord cost and refund.
- Death/respawn/Lost Wick Husk never duplicates or loses Shards; Echo switching mid-attack does not change its payload.

---

## 7. Vertical Slice & Acceptance Criteria

### Proposed scope

- Connected route: **Ash-Sump Slums → Collapsed Mine → Lantern Chamber → Warden Arena → shortcut back to Resonator**.
- Initial layout target: **8–12 genuinely connected rooms**, one Resonator, one lore/memory reveal, and at least one shortcut opened from the far side.
- Three Echoes available in the test build: Ignis, Glacies, Terra. Test Melt-Shock and Tectonic Shatter; all other pairs follow the default rule.
- One Resonance Blade with 3-hit Ground Light, 2-hit Air Light, Heavy, Dash, Parry, and Unison.
- Drifter, Sentry, Brute, two-phase Ash-Sump Warden; HUD, basic map, Lost Wick Husk, and save/load.
- Initial target playtime: **15–25 minutes** after brief onboarding; measure in playtests rather than treating as a guarantee.

### Gameplay acceptance

- The slice can be completed with the available Echoes; a single Echo retains normal base Damage.
- All 12 reaction recipes are correct; invalid pairs cannot trigger a named reaction.
- A target cannot have two Marks simultaneously; expiry is tick-accurate; reaction payloads cannot apply Marks or recursively trigger reactions.
- Parry follows the 10-tick window; Unparryable attacks are not negated; Boss cannot be stun/displacement-locked or Break-locked indefinitely.
- Echo switching does not change an existing attack instance; Dash grants no i-frames; Air-Dash restores only on landing.
- Death/Husk/checkpoint/save-load does not duplicate or lose Shards/progression.

### Control, readability & technical acceptance

- Every listed gameplay action is rebindable; UI glyphs match the active device.
- Players can distinguish Parryable/Unparryable attacks using at least two cue channels.
- Internal playtest targets: ≥80% correctly classify the two telegraph types; ≥70% trigger at least one Reaction without step-by-step prompting. These are targets to measure, not existing user data.
- Simulation uses fixed-step 120 Hz; changing render rate does not change gameplay tick count.
- Replaying the same input log, seed, and initial state in the same build produces the same combat/world-progress event sequence. Do not claim cross-platform determinism without separate validation.
- Test save/load after Echo unlock, checkpoint, while a Husk exists, after Husk recovery, and after Boss defeat.
- Essential telegraphs remain readable when VFX are reduced/disabled.

### Implementation order

1. Movement sandbox.
2. Weapon sandbox: frame data, chain buffer, hit registration, HP/Break/Hitstun.
3. Defense sandbox: Parry, Break State/Recovery.
4. Echo sandbox: deck switching, Mark timing, valid/invalid recipes.
5. Enemy sandbox, then two-phase Boss.
6. World slice: Resonator, save/load, shortcut, map, Lost Wick Husk.
7. Feedback/accessibility and playtesting.

Do not expand to twelve traversal sets, unique Wing Assists, Amplitude Tiers, or multiple Bosses until core acceptance criteria pass.

---

## 8. Scope and Open Decisions

### In scope for v0.3

Rebindable controls; one shared weapon; three Echo slots; movement baseline; shared frame data; Resonance Marks/reactions; Parry/Break/Harmony/Unison; four prototype archetypes; Ash-Sump Warden; basic HUD/feedback; save/checkpoint/Lost Wick Husk; one vertical slice.

### Deferred

Twelve unique weapons/movesets; twelve Wing Assists; twelve Echo-specific traversal mutations; Amplitude Tiers; full region/Boss rosters; unlisted reactions; unlimited healing/Air-Dash restoration; final tuning of physics, frame data, damage, and economy.

### Not yet locked

- Final name/silhouette of the Resonance Blade.
- Physics, collider, and frame-data tuning after testing with the actual sprite/camera/room scale.
- Hit-stop/camera-shake timing.
- Full region/Boss roster and detailed Memory Tuning questline.
- Resonance Sigils, Tuning Shards economy, and non-Echo upgrades.
- Concrete hardware/performance target.

**Change control:** When a combat or movement value changes, update the relevant section of this addendum and its tests. Do not rewrite the entire GDD for a local change. The original v0.2 remains the foundational source; v0.4 adds world/content progression.
