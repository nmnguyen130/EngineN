# VEIL OF THE SILENT
## Master Game Design Document — Prototype Specification v0.3

**Document status:** Prototype specification. V0.3 preserves the core decisions from v0.2 and adds default controls, movement/physics baselines, enemy/Boss contracts, UI/feedback requirements, and vertical-slice acceptance criteria. New values marked *prototype baseline* require playtesting and are not final balance.

| Field | Current decision |
|---|---|
| Genre | 2D Action Metroidvania / Precision Combat Platformer |
| World | Vaelen, an underground kingdom sustained by the stolen voices, memories, and life frequencies of its people |
| Player character | Kaelen, the Off-Beat Scavenger |
| Signature artifact | The Resonance Lantern |
| Core identity | One weapon; twelve resonance modes; deliberate mark interactions; exploration through discovery |
| Runtime target | EngineN custom runtime, C++23 |
| Simulation target | Fixed 120 Hz step; `dt = 1/120 s` (approximately 8.333 ms) |
| Current combat scope | One shared weapon moveset, three-Echo quick deck, one-mark-per-target reaction system, parry, Break, Harmony |
| Explicitly deferred | Twelve separate weapons/movesets, Echo-specific Wing Assist kits, twelve unique traversal mutations, weapon reach/damage tiers |
| V0.3 additions | Default control map; movement and frame-data baselines; enemy/Boss templates; HUD/feedback/accessibility; vertical-slice scope and acceptance criteria |

**Reference points:** *Hollow Knight* for interconnected exploration; *Nine Sols* for readable deflection-focused combat; *TEVI* and *BlazBlue Entropy Effect* for expressive action-system ideas. These are design references, not features to reproduce directly. The goal is to establish a distinct identity through Vaelen's resonance rules, narrative, exploration, and combat decisions.

---

## Version Log

- **v0.2:** Established the vision, world lore, shared weapon, Triad Deck, Resonance Mark rules, ten reaction types across twelve recipes, Parry, Break, Harmony, checkpoints, and runtime boundaries.
- **v0.3:** Adds control mapping, movement/combat frame-data baselines, tick/hit-resolution order, enemy and Boss contracts, HUD/accessibility, vertical-slice scope, and acceptance criteria. New prototype values are not validated final balance.
- **Parry clarification:** The v0.2 ten-tick window is standardized as an active window beginning when Parry is pressed; the player is not required to predict a hit exactly ten ticks in advance.

## Contents

1. Core Vision & Distinct Identity
2. Narrative, Lore & Worldbuilding
3. Twelve Echoes — Resonance Lantern
4. Combat System
5. Metroidvania World Systems
6. Controls & Input Rules
7. Player Movement & Action State
8. Enemy AI & Boss Encounter Contract
9. UI, Feedback, Audio & Accessibility
10. EngineN / C++23 Implementation Boundaries
11. Vertical Slice & Acceptance Criteria
Appendix A. Current Scope Summary
Appendix B. Open Decisions

---

# 1. Core Vision & Distinct Identity

## 1.1 High Concept

**Veil of the Silent** is a 2D Action Metroidvania set in Vaelen, an underground kingdom sustained by the stolen voices, memories, and life frequencies of its people.

Kaelen, an outcast whose heartbeat cannot synchronize with the kingdom's controlling resonance, survives a catastrophic purge and discovers the Resonance Lantern—a forbidden artifact containing twelve lost resonance imprints.

Through these Echoes, Kaelen learns to identify and disrupt the hidden patterns governing creatures, machines, and the world. Exploration reveals what Vaelen has erased. Combat asks the player to read threats and break hostile patterns. Each recovered Echo deepens the player's understanding of the kingdom and expands the possible combinations available in combat.

**Core design thesis:** The player does not simply overpower a world built on control. They learn its rules, discover its contradictions, and exploit what those rules cannot fully explain: the Off-Beat.

## 1.2 Four Core Pillars

### Pillar 1 — The Off-Beat Lantern

The Resonance Lantern is the central system connecting combat, exploration, and narrative. It contains twelve Echoes grouped into six Elemental Resonances and six Phenomenon Resonances.

For the current implementation scope, Kaelen uses **one shared weapon and one shared core moveset**. Switching Echo changes the weapon's resonance palette/VFX and the Echo ID used for Resonance Marks. It does **not** change the weapon type, base damage, reach, attack timing, hitbox dimensions, or attack animation.

Distinct Echo-specific weapons, Wing Assist kits, and traversal mutations are deferred until the core system has been validated.

### Pillar 2 — Deliberate, Expressive Combat

Combat has three distinct outcomes:

- **HP Damage:** reduces a target's health.
- **Break:** fills a target's posture meter and can open a punish window.
- **Crowd Control (CC):** changes whether or how a target can move, attack, or be repositioned.

These are separate values. A move that deals high damage does not automatically deal high Break or apply strong CC. Echo reactions are additional tactical options, not a mandatory damage rotation.

### Pillar 3 — Exploration Through Discovery

World navigation should reward observation, experimentation, and the acquisition of persistent traversal capabilities. Revisiting an area should reveal new routes, shortcuts, or interpretations rather than merely expose another color-coded door.

The current Echo-color system does not itself grant twelve separate traversal powers. Any progression gate in the current build must use a clearly defined persistent world ability, item, route condition, or puzzle flag—not require a particular Echo to be equipped unless that exception is explicitly documented.

### Pillar 4 — Silence, Memory, and Consequence

Silence is a form of control: a means of erasing memory, suppressing identity, and hiding the truth. The world, its inhabitants, enemies, and architecture should express this theme through gameplay and environmental storytelling.

Recovered Echoes are not merely collectible power-ups. They preserve imperfect imprints of the people who created them and provide evidence that the official history of Vaelen is false.

## 1.3 Core Gameplay Loop

**Explore → Observe → Interpret → Unlock or Reconfigure → Overcome a New Constraint → Discover Evidence → Reinterpret the World.**

Combat, traversal, and narrative should reinforce this loop. A new feature is successful when it creates new player decisions, not merely larger numbers or more visual effects.

## 1.4 Design Priorities

1. Distinct player decisions over raw feature count.
2. Readable combat over visual and numerical excess.
3. Consistent world rules over arbitrary ability gates.
4. Meaningful discovery over map size alone.
5. Viable preferred playstyles over compulsory combo optimization.
6. A small, validated vertical slice before committing to all twelve Echoes' future abilities.

---

# 2. Narrative, Lore & Worldbuilding

> **Design status:** The following is the current proposed narrative baseline. It clarifies causal gaps in the earlier draft while preserving its central premise, names, and three-ending structure.

## 2.1 Vaelen — The Kingdom Built on a Lie

For generations, the people of Vaelen have lived beneath the earth, believing that the surface was destroyed long ago. The High Conductors of the Spire of Unison claim that only the Spire's protective resonance keeps humanity alive.

Every citizen receives a **Tuning Collar** at birth. Publicly, the collars protect their wearers from hostile frequencies and contribute to the kingdom's survival. In reality, they connect each person to a vast extraction network.

Through daily resonance rituals, the Spire gradually harvests fragments of citizens' voices, memories, emotions, and vital frequencies. The collected energy sustains the High Conductors' unnatural longevity. The extraction is deliberately gradual: citizens continue to work and form relationships while progressively losing fragments of their identities.

The surface has never been destroyed. The claim of extinction is a political instrument designed to prevent escape and discovery. Maps have been erased or falsified, historical records censored, and unauthorized attempts to reach the upper world treated as existential threats to Vaelen.

## 2.2 The Veil of Silence and the Twelve Apostate Resonators

Twelve Resonators discover that the Spire is a harvesting mechanism rather than a sanctuary. They create the **Resonance Lantern**, an artifact designed to preserve their distinct resonance imprints and disrupt the Spire's centralized control.

The High Conductors discover the rebellion and activate the **Veil of Silence**, an emergency protocol intended to suppress resistance and force the population back into synchronization.

The twelve Resonators sabotage the protocol to prevent a complete reset. Their interference causes a catastrophic feedback event. The Veil spreads through Vaelen, and many citizens become trapped in the corrupted synchronization network. Their remaining instincts and memories are distorted into the hostile behavior of the **Dissonant Husks**.

The twelve Resonators are defeated. Their **original consciousnesses** are imprisoned within twelve living anchors that stabilize the Spire. Their **resonance imprints** are separated from those consciousnesses and scattered among the kingdom's sealed regions. These are two distinct things: recovering an Echo does not automatically free or restore its original person.

The catastrophe is neither a successful revolution nor a deliberate extermination. It is the result of a regime's attempt to preserve control at any cost, compounded by the rebels' desperate attempt to stop it.

## 2.3 Kaelen — The Off-Beat Scavenger

Kaelen is a scrap scavenger from the **Ash-Sump Slums**. He has no royal lineage, prophetic destiny, military training, or inherited claim to the Spire.

He was born with an unusual rhythm in his heart and nervous system. His natural frequency cannot be fully synchronized by the Tuning Collar, making him difficult for the Spire to monitor, control, or harvest through its normal procedures.

This condition does not make him invulnerable. He can still be injured, overwhelmed by environmental phenomena, or harmed by the physical consequences of corrupted resonance. His advantage is specific: the Spire cannot easily overwrite his identity through its normal synchronization process.

Before the Veil of Silence, his difference marks him as defective. He survives by remaining unnoticed and scavenging discarded machinery. When the catastrophe strikes, he survives the initial collapse but finds himself alone in a kingdom whose rules have become lethal.

His initial objective is practical: survive, find a route to the surface, and escape Vaelen. That objective begins to change when he encounters evidence that some Husks retain traces of recognition, memory, or personal behavior beneath their corruption.

Kaelen cannot save everyone simply by refusing the Spire's commands. He must discover what remains of the people trapped within the system and whether the machinery can be dismantled without destroying what it was built to sustain.

His defining conflict is not whether he is willing to become a hero. It is whether he can take responsibility for a world he never chose to belong to.

## 2.4 The Resonance Lantern — The Pharos of Twelve Wicks

The Resonance Lantern was created by the Twelve Apostate Resonators. Its original purpose was to preserve their individual resonance imprints and provide a means of disrupting the Spire's centralized synchronization.

The Lantern was damaged during the rebellion. Its twelve Echo imprints became dormant or scattered, while the corresponding original consciousnesses remained imprisoned within the Spire's twelve living anchors.

Kaelen discovers the damaged artifact while escaping a collapsed mine. When his blood reaches its fractured inner crystal, the first Wick, **Ignis**, awakens. His Off-Beat frequency lets him interact with the Lantern without being forced into the Spire's normal synchronization pattern.

Each recovered Echo provides a resonance identity and reveals fragments of the person whose imprint it preserves. These imprints are imperfect: they may contain missing memories, emotional distortions, or incompatible interpretations of the rebellion. Kaelen must compare their revelations against evidence found throughout Vaelen.

The Lantern is both an instrument of power and a mechanism of discovery. Its ultimate purpose depends on what Kaelen learns and chooses to do with it.

## 2.5 Narrative Progression

### Act I — Survival

Kaelen discovers the Lantern, escapes the first Dissonant Husks, and learns that his Off-Beat condition offers limited protection from the corrupted network. His immediate goal is to find a route to the surface.

### Act II — Contradiction

As Kaelen explores Vaelen and recovers Echoes, official history begins to contradict physical evidence. He discovers the purpose of the Tuning Collars, fragments of the original rebellion, and signs that some Husks retain traces of their former identities. Escape remains possible, but abandoning Vaelen now means leaving unanswered questions and potentially recoverable lives behind.

### Act III — Responsibility

Kaelen reaches the upper Spire and discovers the extraction network's full purpose. The Echoes reveal that fleeing, destroying the system, and transforming it are different choices with different costs.

## 2.6 Three Endings

### Ending A — The False Dawn

Kaelen defeats the High Conductor and reaches the surface without dismantling the extraction network. He escapes Vaelen and discovers that the surface still exists. The dawn is real, but his freedom is incomplete: those left below remain trapped in a system that has not fundamentally changed.

### Ending B — The Shattered Spire

Kaelen destroys the Spire's central core and severs the Tuning Collars' connection to the extraction network. The High Conductors lose the mechanism that sustains their unnatural longevity, and the population is freed from centralized synchronization.

However, the Spire also supports essential infrastructure throughout Vaelen. Its destruction destabilizes parts of the underground kingdom. The control mechanism is destroyed, but liberation does not instantly undo accumulated damage.

### Ending C — Symphony of the Off-Beat

After recovering all twelve Echoes and completing the **Memory Tuning** questline, Kaelen discovers a way to alter the Spire's resonance architecture instead of merely preserving or destroying it.

Using his Off-Beat frequency, he breaks the extraction network's centralized authority and transforms the Spire into a communication relay that no longer requires the sacrifice of its population. Some Husks recover fragments of their original identities where enough memory remains; others cannot be restored, and the dead remain dead.

To complete the transformation, Kaelen must surrender his unique ability to channel the twelve Echoes. He survives but can no longer use the Lantern as a weapon. The result is not a perfect restoration of the old kingdom, but the beginning of a future in which its people can choose whether and how to connect with one another.

The Symphony of the Off-Beat is not the recovery of a lost paradise. It is the rejection of a system that demanded everyone sound alike.

---

# 3. The Twelve Echoes — Resonance Lantern System

## 3.1 Core Rules and Current Scope

The Lantern contains twelve Echoes: six Elemental Resonances and six Phenomenon Resonances. An Echo represents a resonance identity, not a separate weapon.

### One Weapon Rule

Kaelen uses one shared weapon, provisionally named the **Resonance Blade**. Its silhouette, moveset, hitboxes, reach, attack speed, base damage, and attack animations are shared by all Echoes.

Switching Echo changes only:

1. The weapon's resonance color palette and associated VFX.
2. The active `EchoId` used when the weapon applies a Resonance Mark.

Switching Echo does **not** directly buff or nerf HP Damage, Break, reach, startup, recovery, movement, or hitbox size. The gameplay difference comes from the Echo identity participating in a valid, explicitly defined reaction.

For accessibility and combat readability, color must not be the only identifier. Every Echo also requires a distinct glyph/icon and a consistent name in the HUD.

### Current Triad Deck

The player can equip up to three unlocked Echoes in a **Triad Deck**: one active Echo and two quick-switch Echoes. All three are modes of the same weapon; they are not three weapon slots. The deck is configured at a Tuning Resonator. Switching among the equipped Echoes does not require returning to a Resonator.

The current scope does not include separate Echo-specific Wing Assist actions, twelve unique weapon movesets, or twelve unique traversal mutations. Those are deferred until the shared weapon and reaction system are validated.

A player who chooses not to trigger reactions must still retain the full baseline effectiveness of the shared weapon. Reactions are optional advantages, not required damage maintenance.

## 3.2 Echo Identity Table

The colors below are first-pass visual identifiers. They are not gameplay stat modifiers. Final color values, contrast, animation, and glyphs must be validated in-game against every environment and HUD background.

| ID | Echo | Resonance Type | Primary Fictional Identity | Visual Palette (Provisional) |
|---:|---|---|---|---|
| 0 | **Ignis** | Thermal / Hỏa Âm | Heat, ignition, thermal stress | Ember orange |
| 1 | **Terra** | Seismic / Địa Âm | Mass, vibration, structural force | Ochre / stone gold |
| 2 | **Fulgur** | Piezoelectric / Lôi Âm | Conductivity, charge, sudden transfer | Electric cyan |
| 3 | **Glacies** | Cryo-crystalline / Băng Âm | Crystallization, brittleness, geometric order | Ice blue |
| 4 | **Ventus** | Aero-acoustic / Phong Âm | Airflow, lift, pressure gradients | Jade teal |
| 5 | **Aqua** | Hydro-cavitation / Thủy Âm | Fluid pressure, bubbles, implosion | Deep blue |
| 6 | **Lux** | Photonic / Quang Âm | Light, visibility, refraction | Ivory gold |
| 7 | **Umbra** | Null / Ám Âm | Cancellation, silence, absence | Violet black |
| 8 | **Gravitas** | Gravitational / Trọng Âm | Mass, attraction, positional force | Indigo |
| 9 | **Tempus** | Temporal delay / Thời Âm | Delay, stored actions, temporal offset | Silver cyan |
| 10 | **Aether** | Bio-sympathetic / Sinh Âm | Living systems, shared consequences | Crimson |
| 11 | **Stridor** | Shockwave / Phá Âm | Impact, recoil, pressure fronts | White cyan |

## 3.3 Resonance Marks — Exact State Rules

A successful weapon hit applies a **Resonance Mark** to the hit target using the active Echo ID.

- **Duration:** 420 simulation ticks (3.5 seconds at 120 Hz).
- **Maximum marks:** One active Resonance Mark per target. Marks do not stack.
- **Same Echo hits:** Refresh that Echo's mark timer to 420 ticks. They do not trigger a reaction.
- **Different Echo hit with a valid recipe:** Resolve the normal weapon hit first, resolve the reaction's additional effects, then consume the old and new marks. The target is left with no active Resonance Mark.
- **Different Echo hit without a valid recipe:** The old mark is replaced by a mark for the new Echo, with a fresh 420-tick timer. No named reaction occurs.
- **Expired mark:** Removed before reaction validation on that simulation tick. An expired mark cannot be used to trigger a reaction.
- **One-hit rule:** One attack instance can apply a mark to a target at most once unless a future move explicitly defines a multi-hit exception. Secondary damage generated by a reaction does not apply marks and cannot trigger another reaction.
- **Reaction resolution:** Reactions do not recursively trigger themselves. Each pair is resolved from the explicit table in Section 4.3; Echo pairing is order-independent.

### Example

1. Ignis hits a target: the target receives normal attack damage and gains an Ignis mark for 420 ticks.
2. Glacies hits the same target before that mark expires: the hit deals its normal attack damage, then triggers Melt-Shock's explicitly listed bonus damage, Break, and status effect. The Ignis and Glacies marks are consumed.
3. A further Glacies hit applies a new Glacies mark. It does not immediately trigger another reaction because the previous pair was consumed.

## 3.4 Traversal and Ability Scope

The original concept of twelve Echo-specific Traversal Mutations remains a possible future expansion, not a current implementation requirement. No current progression gate may assume these twelve mutations already exist.

For this design baseline, traversal upgrades should be persistent world progression capabilities. A player must not lose access to a required path merely because a different Echo is active or because an Echo is absent from the Triad Deck.

## 3.5 Signature Reaction Contract

The first version supports ten named reaction types across twelve valid pair recipes. Exact behavior, base bonus values, target classes, and restrictions are defined in Section 4.3. Other Echo pairs use the mark replacement rule and have no hidden or inferred special effect.

---

# 4. Combat System — Explicit Rules & Prototype Baseline

> **Important tuning note:** Values in this section are a single coherent starting profile for the prototype. They remove implementation ambiguity but are not a claim of final balance. Change them only through documented tuning and playtests. All durations use the 120 Hz simulation tick unless stated otherwise.

## 4.1 Combat Design Principles

1. **One weapon, one core moveset.** Echo switching does not change the shared weapon's base stats or animation set.
2. **Baseline viability.** The full game must be completable without relying on a specific Signature Reaction rotation. Reactions improve tactical options and efficiency; they are not required to maintain base damage.
3. **Separate dimensions.** HP Damage, Break, and CC are separately authored and resolved.
4. **No free infinite loops.** Reactions do not automatically restore Air-Dash, do not recursively trigger reactions, and do not restore Harmony without a listed rule.
5. **Readable enemy rules.** Every parryable, unblockable, armor-breaking, or CC-resistant attack must have readable telegraphing and consistent resolution.
6. **Bosses are not ordinary enemies with larger HP.** Bosses can receive HP Damage and Break, but ordinary hard-stun, forced displacement, and repeated interrupt effects do not automatically apply to them.

## 4.2 Shared Weapon and Core Combat Values

### Prototype Enemy Health Profile

| Target Class | Prototype HP | Break Meter | Notes |
|---|---:|---:|---|
| Standard enemy | 100 | 100 | Uses normal stagger/CC rules |
| Heavy enemy / Elite | 300 | 100 | May resist displacement; explicit flags decide |
| Boss | Encounter-defined; start testing at 1,200 per phase | 100 | Boss-specific resistance and phase rules must be explicit |

Health values are prototype baselines. Boss HP may vary by encounter; the Break meter remains 100 unless an encounter specification explicitly overrides it.

### Ground Light Attack Chain

| Attack | HP Damage | Break | Definition |
|---|---:|---:|---|
| Light 1 | 10 | 4 | Fast opening strike |
| Light 2 | 12 | 5 | Continues the grounded chain |
| Light 3 | 18 | 9 | Finisher of the three-hit grounded chain |

A complete uninterrupted three-hit chain deals 40 HP Damage and 18 Break before reaction bonuses. A target can be hit only once by each attack instance.

### Air Attack Chain

| Attack | HP Damage | Break | Definition |
|---|---:|---:|---|
| Air 1 | 10 | 4 | First aerial strike |
| Air 2 | 15 | 7 | Aerial finisher |

A complete two-hit air chain deals 25 HP Damage and 11 Break before reaction bonuses.

### Heavy Attack

- **Startup:** 24 ticks (200 ms) before the hit becomes active.
- **HP Damage:** 24.
- **Break:** 18.
- **Purpose:** A deliberate, punishable attack with more Break than a single light hit.
- **Rule:** The heavy attack does not automatically launch, stun, or displace a target. Those effects require an explicit ability or reaction rule.

### Input and Hit Resolution

- The shared weapon uses the same animation, timing, range, and hitbox configuration for all twelve Echoes.
- The active Echo ID is sampled when an attack begins. That attack instance retains the sampled Echo ID until it completes, so a mid-animation Echo switch cannot change the mark type of an already-started attack.
- Echo-switching does not cancel an attack or grant invulnerability. The visual change may be shown immediately, but the current attack resolves using its sampled Echo ID.
- Attack buffering, cancel windows, and final keyboard/gamepad bindings will be specified in the Input/Controls section. They must not change the Damage, Break, or reaction rules defined here.

## 4.3 HP Damage, Break, and Crowd Control — Separate Definitions

### HP Damage

HP Damage is subtracted from the target's current health after applicable damage modifiers. A reaction's **Bonus Damage** is additional to the normal damage of the attack that triggered it. Secondary damage explicitly listed by a reaction is additional to the primary reaction damage.

### Break

Break is a posture meter with a range of 0–100.

- Every hit adds its authored Break value, subject to the target's Break resistance multiplier.
- When a standard enemy reaches 100 Break, it enters **Stagger** for 45 ticks (0.375 seconds). While Staggered, additional Break gain is ignored. Its Break meter resets to 0 when Stagger ends.
- When a Heavy/Elite or Boss reaches 100 Break, it enters **Break State** for 150 ticks (1.25 seconds). It cannot initiate an attack during Break State; an interruptible attack wind-up is cancelled when Break State begins. During Break State, it receives `1.25x` HP Damage. Its Break meter is reset to 0 at the start of Break State.
- After a Heavy/Elite or Boss exits Break State, it has a 240-tick (2-second) **Break Recovery**. During Break Recovery, further Break gain is ignored. HP Damage and explicitly allowed CC still resolve.
- A boss cannot be repeatedly Break-stunned by filling the meter during Break State or Break Recovery.
- Unless an effect explicitly says otherwise, applicable HP Damage multipliers multiply together. For example, Fracture (`1.15x`) and Boss Break State (`1.25x`) produce `1.4375x` damage before any target-specific resistance modifier. No other hidden damage modifier is assumed.

An encounter may define a Break resistance multiplier, but it must state that multiplier explicitly. It may not silently redefine the common 0–100 meter.

### Crowd Control (CC)

CC is not a synonym for damage or Break. Each CC effect must name its type, affected target class, magnitude, and duration.

- **Interrupt:** Cancels an interruptible enemy attack wind-up immediately. It does not automatically prevent the next attack.
- **Micro-stagger:** For the stated duration, the target cannot voluntarily move or attack. It is shorter than a full Stun and may be assigned to standard enemies only.
- **Stun:** The target cannot voluntarily move or attack for the stated duration. It may affect Standard and Heavy/Elite targets only when the effect explicitly lists those target classes. Bosses are excluded unless a specific Boss encounter explicitly permits it.
- **Slow:** Multiplies eligible target movement speed and attack/action progression by the stated factor for the stated duration. A 40% Slow means both are reduced to 60% of their normal rate.
- **Knockback:** Applies the listed instantaneous velocity change to targets that permit displacement. Heavy enemies and bosses may ignore displacement; this does not cancel the reaction's separately listed Damage or Break.
- **Pull:** Moves eligible targets toward a stated point over a stated duration. Bosses are not forcibly repositioned unless an encounter explicitly allows it.
- **Projectile removal:** Removes only eligible ordinary projectiles. Scripted arena hazards, boss signatures, and protected projectiles are not deleted unless their encounter definition permits it.

Bosses do not receive ordinary Stun or forced displacement from standard reactions. Where a reaction mentions control against a boss, the reaction table specifies its actual boss effect. CC durations do not add together: reapplying the same CC refreshes its timer, and the stronger magnitude is kept. For action-lock effects, the priority is `Break State > Stun > Micro-stagger`; a lower-priority effect cannot shorten a higher-priority effect. Slow may coexist with an action-lock effect, but repeated Slow effects do not stack their percentages; use the strongest Slow magnitude and refresh its duration.

## 4.4 Standing-Wave Reactions — Exact Recipes and Effects

### Shared Trigger Sequence

1. The first Echo hit deals its normal weapon Damage and Break and places a mark.
2. A hit from a different Echo deals that hit's normal Damage and Break.
3. If the old/new Echo pair is a valid recipe, apply the recipe's additional Damage, additional Break, and listed effect exactly once.
4. Consume both marks. Secondary targets hit by a reaction do not receive marks and cannot trigger another reaction.
5. Grant **10 Harmony** for a successfully resolved Signature Reaction, once per reaction event.

No reaction restores Air-Dash. No reaction grants Harmony more than once for the same trigger. No reaction uses full-screen targeting unless its radius and maximum target count explicitly say so; the reactions below do not use full-screen targeting. The hit pipeline validates the primary target and existing mark at collision time, applies the base hit and any valid reaction as one hit event, then performs death/despawn cleanup. Therefore a reaction can resolve on a hit that is lethal to its primary target. Secondary damage skips targets that are already dead or invalid.

### Signature Reaction Table

| Reaction | Valid Echo Pair(s) | Bonus HP Damage | Bonus Break | Additional Effect and Restrictions |
|---|---|---:|---:|---|
| **1. Melt-Shock** | Ignis + Glacies | 24 | 16 | Applies **Fracture** for 720 ticks (6 s): the target receives `1.15x` HP Damage from subsequent hits. Fracture does not stack; reapplication refreshes its timer. Works on Bosses. |
| **2. Piezo-Quake** | Terra + Fulgur | 20 | 22 | Standard and Heavy/Elite enemies are interrupted and Stunned for 42 ticks (0.35 s). Bosses are not Stunned; they receive only the listed Damage and Break. |
| **3. Steam Cavitation** | Ignis + Aqua | 22 | 12 | The primary target and up to two additional eligible enemies within 1.75 m of it are affected. Each secondary target receives 8 HP Damage and no Break. Eligible light enemies receive an immediate outward velocity change of `2.5 m/s`; Heavy/Elite and Boss displacement follows their resistance rules. Secondary damage cannot apply marks. |
| **4. Superconductor Gale** | Fulgur + Ventus | 18 | 16 | Arcs to up to two additional enemies within 2.5 m of the primary target. Each secondary target receives 9 HP Damage and 4 Break. No secondary target receives a mark or triggers another reaction. |
| **5. Pyro-Cyclone** | Ignis + Ventus | 20 | 10 | Up to four other eligible enemies within 2.5 m of the primary target receive 12 HP Damage each and no Break. Removes at most three eligible ordinary projectiles in the same radius. Does not remove scripted hazards or protected Boss projectiles. |
| **6. Tectonic Shatter** | Glacies + Terra **or** Glacies + Stridor | 28 | 28 | Cancels an interruptible wind-up and applies a 30-tick (0.25 s) Micro-stagger to standard enemies. If the target has the explicit `BreakableGuard` property and is currently guarding, this reaction breaks the current guard and applies `GuardBroken` for 90 ticks (0.75 s), during which that guard cannot be reactivated. It does not automatically destroy other shields or armor. Bosses are not Stunned. |
| **7. Eclipse Singularity** | Lux + Umbra | 26 | 18 | Pulls up to four eligible light enemies within 3.5 m of the primary target up to 1.5 m toward that target over 12 ticks (0.1 s), stopping at least 0.5 m away from it. Bosses and anchored enemies are not displaced. No full-screen pull. |
| **8. Graviton Quake** | Gravitas + Terra | 22 | 25 | Forces eligible airborne ordinary enemies to land. Up to four other eligible enemies within 2 m of the primary target receive 8 HP Damage and no Break. Bosses are not forced to land or displaced. |
| **9. Chrono-Overdrive** | Tempus + Stridor | 20 | 14 | After 12 ticks (0.1 s), a delayed strike deals an additional 10 HP Damage to the original target if it is still alive and valid. The delayed strike adds no Break, applies no mark, grants no Harmony, and cannot trigger another reaction. If the original target is no longer valid or is dead, the delayed strike is cancelled. |
| **10. Sanguine Boil** | Aether + Ignis **or** Aether + Aqua | 18 | 12 | Up to two additional eligible enemies within 2 m of the primary target receive 8 HP Damage each and no Break. This current-scope reaction does not require a separate link ability; it does not spread marks or chain to further targets. |

The table defines **ten named reaction types and twelve valid pair recipes**, because Tectonic Shatter and Sanguine Boil each accept two alternative pairs. All other Echo pairs follow the default mark replacement rule.

## 4.5 Parry — Phase-Inversion Counter

### Telegraph Rules

Elite enemies and Bosses use readable attack tells that combine a visual cue and a distinct audio cue. Color must be paired with a shape, icon, animation, or sound so the attack remains distinguishable without color perception.

- **Gold Harmonic Wave — Parryable:** A successful timed Parry negates the attack's hit on Kaelen and interrupts the attacking enemy if that enemy is currently interruptible.
- **Crimson Overload Wave — Unblockable:** Parry cannot negate it. The player must avoid the hit using movement, jump, or the shared Dash. The attack must not require one of the deferred Echo-specific traversal abilities.
- **Other attacks:** Their Parry/Dodge classification must be defined by the enemy's attack data. It must not be inferred from damage or visual color alone.

### Parry Window and Rewards

- **Parry timing window:** An accepted Parry input creates a `ParryWindow` lasting 10 ticks (approximately 83 ms), beginning on the input tick. A parryable hit is negated only if actual collision occurs within that window; the player does not need to predict collision exactly ten ticks ahead.
- **Successful Parry:** Negates that incoming hit, interrupts the attacker if interruptible, grants 15 Harmony, and adds 18 Break to the attacker if it has a Break meter.
- **Phase-Inversion bonus:** A Boss attack may declare one `CounterEchoId` in its authored attack data. If the `EchoIdAtParryStart` captured when Parry begins matches that ID, the Parry adds another 12 Break. Switching Echo during the window does not alter that Parry result. The matched Parry therefore adds 30 total Break (18 base + 12 bonus), compared with 18 Break for a normal successful Parry. This is a bonus, not a requirement for a successful Parry.
- **No cascading rewards:** A successful Phase-Inversion Parry does not automatically grant Tier III, does not grant additional unlisted Harmony, and does not force the Boss into Break State unless the resulting Break meter actually reaches 100.
- **Failed Parry:** If the timing window is missed, the incoming attack resolves normally. There is no automatic block or hidden invulnerability from a failed Parry input.

## 4.6 Air Movement and Anti-Infinite Rules

- Kaelen starts an airborne sequence with **one Air-Dash charge**.
- Using Air-Dash consumes that charge. It is restored when Kaelen returns to a valid grounded state.
- Signature Reactions do not restore Air-Dash.
- A future movement ability may grant an additional Air-Dash only if that ability has an explicit trigger and a cap. No such Echo-specific ability is part of the current scope.
- Hit-stop, if used, is a presentation/game-feel effect and must not create extra hit events, repeat mark application, pause expiry timers inconsistently, or allow repeated damage from one attack instance.

These rules prevent the basic reaction loop from becoming an unlimited aerial combo engine. Skillful aerial routing remains possible through jumps, position, attack timing, and the single available Air-Dash.

## 4.7 Harmony Gauge and Unison

### Harmony Rules

- **Range:** 0–100.
- **Gain:** +10 per successfully resolved Signature Reaction; +15 per successful Parry.
- **No gain:** Normal weapon hits, misses, secondary reaction damage, delayed follow-up damage, and damage-over-time do not generate Harmony.
- **Cap:** Harmony cannot exceed 100. Excess gain is discarded.
- **Death / checkpoint behavior:** Checkpoint and death behavior is defined in Section 5. Harmony must never become negative.

### Standard Unison Burst

Standard Unison and Sacred Chord are separate actions at the gameplay-input level; final key/button bindings are deferred to the Controls section. A player may use Standard Unison at 50 or more Harmony even if the current deck qualifies for a Sacred Chord.

- **Cost:** 50 Harmony, deducted when the action starts. If Kaelen enters Hitstun before the Resolve tick, refund 50; no refund is given after Resolve.
- **Area:** 3 m radius around Kaelen.
- **Targets:** Up to five eligible enemies, selected by the combat targeting rule.
- **Effect per target:** 18 HP Damage and 15 Break.
- **CC:** Standard enemies are interrupted for 30 ticks (0.25 s). Bosses receive no Stun or forced displacement.
- **Marks and reactions:** Unison damage does not apply Resonance Marks and cannot trigger Signature Reactions.

### Sacred Chords

A Sacred Chord is available only at exactly 100 or more Harmony and only when the three distinct Echoes in the Triad Deck match one of the exact sets below; slot order does not matter. Activating the separate Sacred Chord action deducts all 100 Harmony when the action starts. If Kaelen enters Hitstun before Resolve, refund all 100; no refund is given after Resolve. The Standard Unison action remains independently available and consumes 50 Harmony when used instead.

| Sacred Chord | Required Triad Deck | Exact Effect |
|---|---|---|
| **Elemental Cataclysm** | Ignis + Terra + Fulgur | A 4 m radius burst around Kaelen. Each of up to five eligible enemies receives 30 HP Damage and 24 Break. Eligible light enemies receive an immediate outward velocity change of `2.0 m/s`. Only explicitly tagged breakable terrain may be damaged; the attack does not destroy arbitrary level geometry. |
| **Cryo-Tempest Protocol** | Glacies + Ventus + Aqua | A 4 m radius burst. Each of up to five eligible enemies receives 22 HP Damage and 18 Break. Non-Boss enemies are Slowed by 40% for 180 ticks (1.5 s). Bosses receive the Damage and Break but are not Slowed. |
| **Eclipse of Eternity** | Lux + Umbra + Tempus | Three afterimage strikes hit each of up to three eligible enemies for 8 HP Damage per strike (24 total) and 18 total Break. Standard enemies are unable to act for 72 ticks (0.6 s). Bosses are not frozen or Stunned; they receive the listed Damage and Break. |
| **Singularity Overdrive** | Gravitas + Aether + Stridor | Within a 4.5 m radius, eligible light enemies are pulled toward a central point over 36 ticks (0.3 s), then take 28 HP Damage and 25 Break. Up to five eligible enemies can be hit. Bosses are not displaced but still receive the listed Damage and Break. |

Sacred Chord effects do not apply marks, trigger reactions, or grant extra Harmony. A Sacred Chord is a high-impact option, not a required condition for defeating any Boss.

## 4.8. Amplitude Ascension — Current Status

The original system that increased weapon reach and damage through weapon-specific Tiers is **removed from the current scope**. There is one weapon, Echo switching does not alter weapon stats, and a second weapon-upgrade meter would add complexity without a validated need.

Do not implement `Tier I / Tier II / Tier III`, weapon reach multipliers, automatic Crescendo Wave-Bursts, or Echo-switch damage bonuses in the current prototype. If testing later shows that Echo switching lacks tactical value, revisit progression only after the shared weapon, marks, reactions, Parry, Break, and Harmony loop is stable.

## 4.9. Combat Readability and Test Requirements

Every reaction, Parry, Break State, CC effect, and Harmony change must have clear feedback. At minimum:

- Show the active Echo glyph in the HUD and a corresponding mark icon on a marked target.
- Use distinct visual/audio cues for normal hit, Break gain, Break State, Parry success, reaction trigger, and reaction expiration.
- Do not depend on color alone to indicate attack safety or reaction identity.
- Ensure reaction damage cannot apply a new mark or recursively trigger another reaction unless a later rule explicitly changes this contract.
- Verify that a player who uses only one Echo can still defeat ordinary enemies and complete baseline encounters without a hidden damage penalty.
- Test target death/despawn, mark expiration on the same tick as a hit, simultaneous hits, multi-target reactions, Boss Break Recovery, and death/checkpoint transitions.

---

# 5. Metroidvania World Systems

## 5.1 Tuning Resonators — Checkpoints

Tuning Resonators are checkpoints that restore Kaelen's HP, restore Harmony to 100, save progression, and allow the player to configure the three-Echo Triad Deck from unlocked Echoes.

They are not required for switching among the three Echoes already equipped. Required traversal progression is persistent and cannot be invalidated by the active Echo or deck configuration.

**Resonance Sigils** may be equipped at a Resonator if introduced as a separate upgrade system. Their effects must be defined individually and must not silently change the combat contracts in Section 4.

## 5.2 Lost Wick Husk — Death Recovery

On death, Kaelen respawns at the last activated Tuning Resonator, Harmony resets to 0, and a **Lost Wick Husk** is placed at the death location. It stores all currently carried **Tuning Shards** and applies a `25%` reduction to maximum HP until the Husk is recovered. The maximum-HP penalty does not stack beyond 25%.

Touching/interacting with and destroying the Lost Wick Husk recovers the stored Shards, removes the maximum-HP penalty, and restores Harmony to 100. HP after recovery is capped at the newly restored maximum HP; recovery does not grant more than maximum HP.

For the prototype, only one recoverable Lost Wick Husk may exist at a time. If Kaelen dies again before recovering it, the existing Husk remains at its original location, and any newly carried Tuning Shards are added to that same stash; a second Husk is not created. This avoids ambiguous duplicate-grave behavior without silently discarding carried currency.

## 5.3 Acoustic Fog of War and Map Pins

The map is revealed through exploration. The map supports these pins:

- `FrequencyGate`
- `UnreachedLedge`
- `LoreTablet`
- `BossArena`
- `TuningForkQuest`

Map pins are player-authored reminders; they do not automatically reveal hidden solutions. Critical progression should provide enough environmental feedback to form a hypothesis without removing optional discovery.

## 5.4 Frequency Gates and Progression Rules

Frequency Gates are environmental locks whose conditions must be consistent with the abilities currently implemented.

For the current scope, a gate may require a persistent traversal upgrade, an acquired item, an environmental puzzle state, or a world-progress flag. It must **not** require one of twelve deferred Echo-specific traversal mutations or a specific Echo to be active unless that mechanic is explicitly brought back into scope and documented.

A progression gate should communicate:

1. What observable property prevents passage.
2. What kind of interaction may change that property.
3. Whether it is required for the main route or optional content.

The game should avoid requiring repeated checkpoint travel solely to change an Echo.

---

# 6. Controls & Input Rules

> The following is the prototype's default input map. All gameplay actions must support rebinding; remapping must not alter gameplay rules.

## 6.1 Default Controls

| Action | Keyboard / Mouse | Default Gamepad | Rule |
|---|---|---|---|
| Move left/right | `A` / `D` | Left Stick / D-Pad Left/Right | Facing changes during neutral movement; each attack instance locks its facing when it starts. |
| Jump | `Space` | `A / Cross` | Uses Jump Buffer and Coyote Time from Section 7. |
| Light Attack | `Mouse 1` | `X / Square` | Uses the shared weapon moveset; grounded/air variant is selected when input is accepted. |
| Heavy Attack | `Mouse 2` | `Y / Triangle` | A committed heavy strike; does not automatically Stun or knock back. |
| Dash | `Left Shift` | `B / Circle` | Ground Dash or Air-Dash depending on current state; no default invulnerability. |
| Parry | `F` | `LB / L1` | Starts a ten-tick Parry Active Window. |
| Select Echo slot 1 | `1` | Cycle with `LT/RT` | Selects slot 1 of the Triad Deck. |
| Select Echo slot 2 | `2` | Cycle with `LT/RT` | Selects slot 2 of the Triad Deck. |
| Select Echo slot 3 | `3` | Cycle with `LT/RT` | Selects slot 3 of the Triad Deck. |
| Previous Echo | `Q` | Quick press `LT` | Cycles one slot backward; does not alter an attack instance already running. |
| Next Echo | `C` | Quick press `RT` | Cycles one slot forward; does not alter an attack instance already running. |
| Standard Unison | `R` | `RB / R1` | Available at Harmony ≥ 50; costs 50 Harmony. |
| Sacred Chord | `T` | `D-Pad Down` | Requires Harmony = 100 and a qualifying Triad Deck. |
| Interact | `E` | `D-Pad Up` | Requires a valid target within range and facing; never triggers merely by walking past. |
| Map | `Tab` | `View / Select` | Opens/closes the map; does not pause simulation unless the pause-menu setting explicitly says so. |
| Pause / Menu | `Esc` | `Menu / Start` | Pauses gameplay simulation in single-player. |

Platform labels are approximate equivalents. The UI must show the actual glyph for the active input layout. All bindings are remappable. Warn about action conflicts, but allow custom mappings if conflicting-input resolution remains deterministic.

## 6.2 Echo Switching Rules

- The Triad Deck contains exactly three distinct unlocked Echoes; no slots may remain empty after confirming a deck.
- Switching Echo costs no Harmony, has no cooldown, and grants no invulnerability in the prototype.
- Echo switching is allowed during an attack, but does not change the EchoId captured by the current attack instance.
- If Echo is switched during an active attack, that attack's VFX retains the captured Echo until it completes. The HUD may show the newly selected slot immediately. This prevents the visual effect from contradicting the gameplay payload.
- Echo switching is blocked while Kaelen is Dead or in Hitstun. Otherwise, it does not cancel locomotion or the current attack.
- Selecting the already active slot creates no gameplay event and does not refresh a target's mark.

## 6.3 Input Buffering, Priority and Conflicts

### Input buffers

- **Light-chain buffer:** 8 ticks (66.7 ms). Light 2/3 can be queued within the end window of the previous attack, as defined in Section 7.
- **Jump buffer:** 10 ticks (83.3 ms). If Jump is pressed within ten ticks before Kaelen lands, it executes on the first valid Grounded tick.
- **No Parry buffer:** Parry is timing-sensitive. An input issued before Parry can activate or while control is locked does not automatically activate later.
- **No Unison/Sacred Chord buffer:** If Harmony or the Triad Deck is invalid on the input tick, the action is not queued.
- Buffers record press edges, not continuous holds, except that Jump Hold controls jump height.

### Action priority

When competing actions are pressed on the same simulation tick, resolve in this order:

1. Pause/Menu.
2. Locked states: Dead, Respawn, Hitstun, or forced knockback.
3. Parry.
4. Dash.
5. Jump.
6. Standard Unison / Sacred Chord.
7. Attack.
8. Interact.

Echo selection is independent of the priority sequence unless Kaelen is Dead or in Hitstun. If two actions of the same priority are pressed together, create no more than one primary action; use the fixed tie-break `Parry > Dash > Jump > Unison > Attack > Interact`. Losing inputs are not automatically executed on a later tick, except for the explicitly defined Jump and Light-chain buffers.

### Interaction

- Interaction succeeds only when a target is in the interaction volume, is in a valid state, and the player presses Interact.
- If several targets qualify, choose the nearest to the interaction origin; ties resolve by lowest numeric `InteractPriority`, then ascending EntityId. Never pick randomly.
- Interact does not trigger automatically when attacking or passing a target.

## 6.4 Rebinding and Input Support

- Keyboard and Gamepad remapping is required for every gameplay action in the table.
- Support left/right remapping and gamepad-layout changes according to the active device.
- Analog-stick dead zone is configurable; prototype baseline is `0.18` of normalized magnitude, with a live calibration display.
- Gamepad triggers count as pressed above `0.55`; allow threshold adjustment.
- Hold-to-repeat applies to menus only, not Light Attack, Heavy Attack, Parry, or Unison.
- Keyboard `1/2/3` selects slots directly; `Tab` is not also bound to Echo switching by default.

---

# 7. Player Movement & Action State

> **All values in Section 7 are prototype baselines.** They are concrete starting assumptions for implementation and measurement, not final balance. Tune them after playtesting and update both the specification and tests.

## 7.1 Simulation Units and Collider

- **Gameplay unit:** `1 wu = 1 m` for physics/combat tuning. Pixel art and camera scale map to world units separately; pixels are not physics units.
- **Conceptual axes:** `+X` is right and `+Y` is up in gameplay math. The renderer may use the opposite screen-space Y axis, but conversion belongs at the engine boundary.
- **Player collision capsule baseline:** `1.65 wu` tall and `0.55 wu` wide; gameplay origin is at the feet midpoint so jumps and landings remain stable. The collider does not change with Echo or VFX.
- **Grounded:** Kaelen's feet are supported by a valid surface and falling motion has ended. Only contact tagged `WalkableGround` restores Air-Dash. Facing changes during neutral movement; each attack instance snapshots facing when it starts.
- No default fall damage. Pits/death volumes and damaging hazards must be explicitly defined by level data.

## 7.2 Horizontal Movement

| Parameter | Baseline | Meaning |
|---|---:|---|
| Max ground speed | `6.5 wu/s` | Maximum horizontal speed on ground. |
| Ground acceleration | `50 wu/s²` | Acceleration from rest with directional input. |
| Ground braking | `65 wu/s²` | Deceleration when input is released or reversed. |
| Max air speed | `6.0 wu/s` | Horizontal speed cap in air. |
| Air acceleration | `24 wu/s²` | Airborne horizontal steering. |
| Max fall speed | `22 wu/s` | Terminal falling speed. |

There is no unlimited ground slide. With no directional input, horizontal velocity decays according to Ground Braking. Air control is weaker and does not stop motion instantly. The active Echo does not alter movement parameters.

## 7.3 Jump, Coyote Time and Jump Buffer

- **Initial jump velocity:** `11.5 wu/s` upward.
- **Gravity:** `32 wu/s²` downward.
- **Coyote Time:** 8 ticks (66.7 ms) after leaving WalkableGround. It permits a late takeoff jump but does not restore Air-Dash without landing.
- **Jump Buffer:** 10 ticks (83.3 ms), as defined in Section 6.3.
- **Variable-height jump:** if Jump is released while Kaelen is ascending, multiply current upward velocity by `0.55` on the first tick that detects release. Holding Jump does not increase initial velocity; it only avoids cutting the jump short.
- **Jump retrigger:** one ground jump per Grounded landing. Double Jump and Wall Jump are out of prototype scope.
- **Landing:** on contact with WalkableGround, set vertical velocity to zero, enter Grounded, clear the coyote timer, and restore one Air-Dash charge.

With the gravity and initial velocity above, theoretical jump height is approximately `2.06 wu`, before collider geometry, ceiling contacts, and variable-height cutting. Test low ceilings and sloped ground; collision must never create an extra jump.

## 7.4 Dash and Air-Dash

| Parameter | Baseline |
|---|---:|
| Dash velocity | `13.5 wu/s`, horizontal only |
| Dash duration | 12 ticks (100 ms) |
| Air-Dash charges | 1 per airborne sequence |
| Recovery lockout | 6 ticks (50 ms) after Dash ends |
| Ground Dash cooldown | 24 ticks (200 ms) after Dash ends |
| Invulnerability | None by default |

- Dash is horizontal only in this prototype. Use left/right input if present; otherwise use current facing. Diagonal and vertical Dash are out of scope.
- During Air-Dash, horizontal velocity is set by the Dash direction, vertical velocity is temporarily set to zero, and gravity is suspended until Dash ends.
- Ground Dash does not consume Air-Dash charge. Air-Dash can start only while airborne and with a charge available; the charge is consumed when Dash starts, not when it ends. Ground Dash cannot be started again until its 24-tick post-Dash cooldown expires. Air-Dash does not use this ground cooldown because its charge is the limiting resource.
- During the 12 Dash ticks, horizontal velocity is locked to the Dash direction. When Dash ends, horizontal velocity becomes `35%` of Dash velocity; gravity resumes. During the six-tick recovery lockout, Kaelen can run/steer and can jump if Grounded, but cannot start Light, Heavy, Parry, Unison, or another Dash. The Ground Dash cooldown still blocks Ground Dash for 24 ticks after the previous Dash ends.
- Dash does not pass through walls, hazards, Boss hitboxes, or gate collisions. Collision is resolved every tick; wall-normal motion is blocked at contact.
- Dash grants no i-frames. Avoid attacks through hitbox positioning or Parry where allowed.
- Air-Dash is not restored by Resonance Reactions, Parry, or Harmony. It returns only on valid landing unless a future movement ability explicitly defines an exception.

## 7.5 HP, Damage Reception and Death

- **Player Max HP baseline:** `100`.
- **Passive regeneration:** none.
- A valid `EnemyAttackInstance` can damage Kaelen at most once per attack instance unless `MultiHit` is explicitly flagged and its inter-hit interval is defined.
- After taking damage, Kaelen receives `42 ticks` (350 ms) of hit protection against ordinary hits. This does not protect from pits/death volumes, scripted hazards, or attacks flagged `IgnoreHitProtection`; every exception must be specified in encounter data.
- Baseline hitstun lasts `18 ticks` (150 ms), unless an attack defines another value. Hitstun blocks attacks, Parry, Echo selection, Interact, and Unison; movement input is accepted again when hitstun ends. Dash does not remove hitstun.
- At HP ≤ 0 or when entering a death volume, transition to Dead during hit resolution and process no further gameplay action that tick.
- Respawn at the most recently activated checkpoint, with current HP set to the effective maximum. After death, effective maximum HP is `75/100` until the Lost Wick Husk is recovered, as specified in Section 5.2. Saved Echo unlocks and world flags remain. The active Boss encounter resets according to its encounter contract.

## 7.6 Weapon Attack Frame Data

Frame data below applies equally to all twelve Echoes. `Startup` is the number of ticks from accepting the action to the first active tick; `Active` is the number of ticks during which the hitbox is live; `Recovery` is the number of ticks locked from attacking after Active, before returning to neutral. These are simulation ticks, not rendered frames.

| Attack | Startup | Active | Recovery | HP Damage | Break | Notes |
|---|---:|---:|---:|---:|---:|---|
| Ground Light 1 | 5 | 3 | 10 | 10 | 4 | Starts ground chain. |
| Ground Light 2 | 6 | 3 | 10 | 12 | 5 | Second hit when correctly buffered. |
| Ground Light 3 | 8 | 4 | 14 | 18 | 9 | Finisher of ground chain. |
| Air Light 1 | 5 | 3 | 9 | 10 | 4 | First aerial hit. |
| Air Light 2 | 7 | 3 | 12 | 15 | 7 | Aerial-chain finisher. |
| Heavy | 24 | 4 | 18 | 24 | 18 | High commitment; creates no CC by itself. |

### Chain and Cancel Rules

- Ground Light 1 → 2 → 3 uses the same Light input. The next Light can be buffered in the final eight ticks of the current action; earlier input is not retained.
- The Light chain can continue after a hit or a whiff. A confirmed hit is not required, avoiding an excessively long lockout after a miss; each hit still requires valid collision.
- Air Light has at most two hits per chain. It cannot automatically restart at Air Light 1 without returning to a valid neutral state.
- Dash or Parry may cancel Light/Heavy only during the final four Recovery ticks. Startup and Active cannot be cancelled. This rule does not apply during Hitstun.
- Jump does not cancel an attack in Startup/Active. Jump may be used during recovery when locomotion and landing state permit; Jump Buffer lasts at most ten ticks.
- Each `AttackInstanceId` may hit a target only once. Multiple active ticks do not automatically produce multiple hits.
- EchoId is captured when an attack starts. If the player switches Echo during a chain, the current attack keeps its original EchoId; the next attack uses the newly selected Echo if switched before it begins.

## 7.7 Parry State — Implementation Interpretation

To remove ambiguity in v0.2's phrase “input within ten ticks before the hit reaches Kaelen,” v0.3 uses this rule:

- When a Parry input is accepted, create a `ParryWindow` lasting 10 ticks, beginning on the input tick.
- A parryable hit is negated only if actual collision occurs during this window. Input does not predict a future hit and is not buffered across the collision.
- If several hits arrive during one window, only the first hit is parried; the window is consumed immediately on success.
- Parry cannot negate Unparryable/Crimson Overload attacks. Those attacks need readable telegraphs and a reasonable avoidance route.
- On Parry input, capture `EchoIdAtParryStart`. A successful Parry grants +15 Harmony and +18 Break to a valid attacker; if `EchoIdAtParryStart` matches `CounterEchoId`, add another +12 Break. Switching Echo during an active window does not change the Counter Echo result for that window. No hidden effects are granted.
- If the window ends without a Parry, Kaelen returns to normal after the recovery animation. There is no passive block or damage reduction.

## 7.8 Unison Action Timing

These actions have their own commitment windows; inputs are not buffered. Values are prototype baselines.

| Action | Startup | Resolve / Active | Recovery | Cancel / Interruption |
|---|---:|---:|---:|---|
| Standard Unison | 12 ticks | 1 tick | 18 ticks | Cannot be cancelled voluntarily. If Kaelen enters Hitstun before Resolve, cancel the action and refund 50 Harmony. |
| Sacred Chord | 24 ticks | 1 tick | 36 ticks | Cannot be cancelled voluntarily. If Kaelen enters Hitstun before Resolve, cancel and refund 100 Harmony. |

- Capture the target set on the Resolve tick, not the input tick. Select up to the target limit in Section 4, sorted by distance from Kaelen; ties use ascending EntityId.
- Unison grants no i-frames, applies no Resonance Mark, triggers no Signature Reaction, and grants no Harmony.
- If no valid target remains at Resolve, the action causes no Damage/Break, but its cost remains paid.

## 7.9 Simulation-Tick Processing Order

To avoid cross-system ambiguity, each fixed tick follows this logical order:

1. Sample input and create action intents.
2. Validate control state/input priority; accept at most one primary action.
3. Update locomotion, attack state, ParryWindow, Unison action state, and movement velocity.
4. Integrate physics and resolve collision against world geometry.
5. Evaluate attack-target collisions and parryable-hit/ParryWindow intersections.
6. Resolve Parry before incoming damage if both happen in the same tick for the same attack instance.
7. Apply HP Damage/Break for remaining valid hits; generate a reaction event if the mark pair is valid.
8. Resolve the reaction payload once. Secondary payloads cannot create new marks/reactions.
9. Resolve death/despawn, encounter transitions, and feedback events.
10. Update timers at tick end. Timers use `expiresAtTick = currentTick + durationTicks`; an effect is active while `currentTick < expiresAtTick`. A mark applied at tick `T` with duration 420 is valid on ticks `T` through `T+419` and expires before hit checks on tick `T+420`.

If the engine currently uses another pipeline, gameplay adapters must produce equivalent outcomes. Container/entity iteration order must not decide reaction targets or simultaneous-collision results.

---

# 8. Enemy AI & Boss Encounter Contract

## 8.1 Enemy Attack Data Contract

Each enemy attack must be data-defined with at least these fields:

| Field | Required | Meaning |
|---|---|---|
| `AttackId` | Yes | Stable ID; not reused during a running encounter. |
| `WindupTicks` | Yes | Telegraph duration before Active. Must be > 0 except for already telegraphed environmental hazards. |
| `ActiveTicks` | Yes | Time during which the attack hitbox/projectile can hit. |
| `RecoveryTicks` | Yes | Time before the attacker may start another attack after Active. |
| `HitDamage` | Yes | HP Damage on a valid hit. |
| `AttackShape` | Yes | Hitbox, projectile, or hazard region with authored size/offset. |
| `ParryClass` | Yes | `Parryable` or `Unparryable`. |
| `Interruptible` | Yes | Whether an eligible Interrupt/Break can cancel the wind-up. |
| `CounterEchoId` | No | Echo that adds +12 Break on a successful Parry; not required for basic Parry success. |
| `TargetPolicy` | Yes | Targeting/aim rule. It cannot unpredictably retarget after Active begins. |
| `MultiHitPolicy` | Yes | `SingleHit` by default, or explicit hit count/interval rules. |
| `ProjectilePolicy` | If projectile | Speed, lifetime, collision mask, and reaction/deletion eligibility. |

### Enemy State Loop

`Idle/Approach → Telegraph → Active → Recovery → Cooldown/Approach`.

A valid Interrupt during Telegraph cancels the attack and transitions to `InterruptedRecovery` as defined by data. Break State has priority over attacks. Once Active begins, an attack does not rotate or retarget unless its data explicitly defines tracking and a tracking limit.

## 8.2 Prototype Enemy Archetypes

Numbers are baselines; HP and damage can be tuned, but roles and tells must remain distinct.

| Archetype | HP | Break | Baseline Damage | Role | Main Rule |
|---|---:|---:|---:|---|---|
| **Dissonant Drifter** | 100 | 100 | 12 | Standard melee enemy | Approaches, telegraphs clearly, then uses one melee sweep; enters 45-tick Stagger when Break fills. |
| **Choir Sentry** | 100 | 100 | 10 / projectile | Ranged pressure | Fires one projectile after telegraph. Each projectile has its own AttackInstanceId and finite lifetime. Baseline projectile is Parryable. |
| **Resonance Brute** | 300 | 100 | 20 | Heavy enemy that controls space and teaches dodge/Parry | Long wind-up and small area pressure; cannot be freely pulled; uses the standard 150-tick Break State and 240-tick Recovery. |
| **Tuning Husk / Elite variant** | 300 | 100 | Per attack | Elite variant to test counters | At least one Parryable and one Unparryable attack; color alone must not distinguish them. |

### Baseline Attacks for Standard Archetypes

| Archetype / Attack | Wind-up | Active | Recovery | Damage | Parry | Exact behavior |
|---|---:|---:|---:|---:|---|---|
| Drifter — Close Swipe | 18 ticks | 4 ticks | 24 ticks | 12 | Yes | Forward arc up to `1.1 wu`; facing locks when Active begins. |
| Sentry — Single Bolt | 30 ticks | Fires once | 30 ticks | 10 | Yes | Straight projectile at `7 wu/s`, lifetime `180 ticks`, no retargeting after launch. |
| Brute — Heavy Slam | 36 ticks | 6 ticks | 36 ticks | 20 | No | Uses Crimson Overload cue; ground danger region within `1.5 wu`; player can jump or Dash out of the region. |
| Brute — Shoulder Break | 24 ticks | 5 ticks | 30 ticks | 16 | Yes | Horizontal charge of `2.0 wu`; stops at valid collision and cannot pass through level geometry. |

One AttackInstance hits Kaelen at most once unless `MultiHitPolicy` says otherwise. Projectile rules apply per projectile instance, not to the entire volley as one hit.

The entire archetype roster is not required for core validation. The vertical slice needs Drifter, Sentry, Brute, and one Boss. The Elite can reuse Brute animation with distinct, explicit attack data.

## 8.3 Telegraph and Fairness Rules

- Every damaging attack needs at least one pre-active tell: animation/pose, VFX, audio, or environmental cue.
- Parryable attacks use Gold Harmonic Wave language; Unparryable attacks use Crimson Overload Wave with distinct shape/audio as well. Color alone is insufficient.
- Telegraphs must remain visible while Kaelen attacks or Echo VFX are active. Player VFX must not completely obscure dangerous hitboxes.
- A hitbox cannot deal damage before `WindupTicks` complete. Telegraphs do not cause damage unless attack data explicitly defines a damaging hazard.
- A Boss cannot change attack class after telegraph begins.
- If several enemy hitboxes touch Kaelen on the same tick, resolve in this order: explicitly lethal hazard > higher numeric attack priority > ascending AttackId. Ordinary hit protection prevents simultaneous repeated hits unless an attack has a declared `IgnoreHitProtection` flag.

## 8.4 Prototype Boss — Ash-Sump Warden

> This is a systems-validation Boss, not a locked final Boss or confirmed canon character.

### Experience Goals

The Ash-Sump Warden tests four skills: reading telegraphs; parrying the correct class of attack; avoiding attacks that cannot be parried; and using Echoes for reactions or Counter Echo bonuses without forcing one loadout.

### Base Configuration

- **HP:** 1,200 per phase, two phases (2,400 baseline total).
- **Break Meter:** 100.
- **Resistances:** No ordinary Stun, no forced pull out of the arena, and no forced displacement.
- **Break:** 150-tick Break State (1.25 s) with `1.25x` HP Damage, followed by 240-tick Break Recovery (2 s).
- **Phase transition:** At 0 HP for the current phase, block damage for 60 ticks (0.5 s), clear encounter projectiles, reset Break Meter to 0, and clear the Boss's Resonance Mark. Then start the next phase at 1,200 HP. The transition does not damage the player.
- **Arena:** Enough horizontal room to dodge/Parry and at least one safe landing path; no deferred Echo-specific traversal is required.

### Baseline Attack Set

| Attack | Phase | Wind-up | Active | Recovery | Damage | Parry | Counter Echo | Behavior |
|---|---|---:|---:|---:|---:|---|---|---|
| **Resonant Cleave** | I & II | 30 ticks | 6 ticks | 30 ticks | 18 | Yes | Terra | Forward hitbox `2.0 wu` wide and `1.4 wu` tall; one hit. Parrying with Terra adds +12 Break on top of the normal Parry reward. |
| **Choir Bolts** | I & II | 24 ticks | Fires once | 36 ticks | 12 per projectile | Yes, per projectile data | Fulgur | Fires three projectiles at `-15°`, `0°`, and `+15°`, speed `8 wu/s`; each hits once and expires after at most 180 ticks. Parry succeeds if contact occurs in the ParryWindow. |
| **Crimson Collapse** | I & II | 42 ticks | 60 ticks | 36 ticks | 24 | No | None | A low shockwave `0.35 wu` high remains active while travelling forward up to `4.5 wu` at `9 wu/s` (up to 60 ticks). Jump over it or leave its lane; it does not cover the entire arena. |
| **Refracted Sweep** | II | 30 ticks | 6 ticks | 26 ticks | 16 | Yes | Glacies | Forward hitbox `2.4 wu` wide and `1.5 wu` tall; one horizontal sweep. A second afterimage is visual-only, not a second hitbox. |

### Phase II

Phase II adds Refracted Sweep to the attack set. It does not automatically shorten telegraphs or speed up the entire Boss; any phase-specific parameter must be present in encounter data. The AI must not choose the same Unparryable attack twice in a row if another valid choice exists. Telegraphs always finish before damage begins.

### Boss Test Rules

- Every attack can be avoided by at least one action available in the prototype (movement, jump, Dash, or Parry where allowed).
- Counter Echo adds Break only; the Boss remains beatable without the matching Echo.
- Break/Recovery prevents permanent control lock through repeated Stun or Break.
- Phase transitions deal no damage, retain no old projectile, do not remove player HP, and do not trigger a reaction.
- No arena route requires a deferred Echo-specific traversal ability.

---

# 9. UI, Feedback, Audio & Accessibility

## 9.1 Minimum HUD

| Element | Content | Required behavior |
|---|---|---|
| HP | Current HP / effective maximum | Show the Lost Wick Husk max-HP lock separately from lost current HP. |
| Harmony Gauge | 0–100 | Communicate +10 Reaction, +15 Parry, and Unison costs; never exceed 100. |
| Triad Deck | Three equipped Echoes and active slot | Each Echo has icon/glyph plus palette; color is not the sole identifier. |
| Resonance Mark | Echo icon and remaining time on relevant/locked target | Expiry must provide feedback; a decimal timer is not required. |
| Boss HP | Current phase HP | Phase transitions must be explicit and follow the Boss contract. |
| Boss Break | 0–100 and Break/Recovery state | Distinguish Break State from Break Recovery using more than color alone. |
| Interact prompt | Action and current input glyph | Follows remapping and appears only when interaction is valid. |

## 9.2 Feedback Language

Each event requires at least one visual response and one audio response or an optional equivalent:

- **Normal hit:** brief impact flash and light hit sound.
- **Break gain:** feedback distinct from HP hits so Break is not mistaken for damage.
- **Break State:** pose/break animation, clear sound cue, and UI state.
- **Successful Parry:** immediate feedback on the confirmation tick and a unique sound; Harmony updates accordingly.
- **Failed Parry:** never plays success VFX/sound.
- **Reaction:** brief reaction name/icon or combination glyph; distinguish it from Standard Unison.
- **Mark expiry/replacement:** subtle feedback to explain why no reaction occurred.
- **Gold/Crimson telegraph:** distinct shapes, attack poses, and audio cues; never rely on color alone.

Hit-stop, if added, is presentation-only and must preserve Section 4.6 invariants. It must not create fake combat ticks or desynchronize gameplay timers.

## 9.3 Echo Visual Language

- Every Echo has `EchoId`, short name, glyph, palette, and signature VFX.
- VFX does not alter hitboxes, attack direction, or Damage.
- Reaction VFX must reflect the actual Echo pair. If an attack captured a previous EchoId, its VFX must not use the newly selected Echo's identity.
- Dangerous projectile/hazard cues need outline/contrast and visual priority over Kaelen's decorative VFX.
- Reducing VFX must not remove telegraphs, mark icons, or essential gameplay information.

## 9.4 Audio and Accessibility

- Separate Master, Music, SFX, and Combat Cue volume.
- Optional subtitles with speaker labels; no essential combat information is communicated only through audio.
- Screen Shake at minimum has 0%, Reduced, and Full settings.
- Provide reduced flash/repeated-flash intensity. High-frequency flashing cannot be the only gameplay signal.
- Colorblind-safe telegraphs: each Parryable/Unparryable class also uses shape/animation/audio.
- Support keyboard/gamepad remapping, binding conflict warnings, and correct glyphs for the current input.
- If hold-based actions are added later, provide hold/toggle options where feasible; v0.3 does not require any action to be held for gameplay execution.

---

# 10. EngineN / C++23 Implementation Boundaries

This section defines implementation-facing concepts, not a requirement to create a separate source file for every item. Keep code responsibilities clear without over-fragmenting the project.

## 10.1 Runtime Baseline

- Language: C++23.
- Runtime: EngineN custom runtime.
- Simulation target: fixed 120 Hz step.
- Suggested compiler baseline from the project: `-std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror` where supported by the build toolchain.
- A fixed simulation rate does not by itself guarantee cross-platform determinism. Randomness, ordering, collision resolution, timer updates, and input sampling must also use documented deterministic rules if deterministic replay is required.

## 10.2 Core Data Concepts

The combat implementation requires concepts equivalent to:

- `EchoId` with twelve valid values (`0..11`).
- `TriadDeck` containing three distinct or explicitly validated Echo slots.
- `ResonanceMark` with `EchoId`, remaining ticks, and target identity.
- `WaveReactionId` for the ten named reaction types.
- A reaction-recipe table containing twelve valid unordered pair recipes.
- Combat attributes for HP Damage, Break, target class, CC resistance, displacement resistance, and damage modifiers.
- `HarmonyGauge` in the bounded range `0..100`.
- Boss attack metadata, including telegraph classification and optional `CounterEchoId`.
- Persistent world progression flags and checkpoint/save data.

## 10.3 Suggested Responsibilities

- **Echo/Resonance data:** immutable definitions for Echo IDs, names, glyphs, palettes, and valid reaction recipes.
- **Weapon combat:** shared attack states, sampled Echo ID per attack instance, hit validation, base Damage, and base Break.
- **Mark/reaction resolution:** mark expiry, valid-pair lookup, mark consumption/replacement, reaction payload application, and anti-recursion rules.
- **Defense and posture:** Parry window validation, Break meter progression, Break State, Break Recovery, and explicit CC rules.
- **Harmony/Unison:** resource caps, gain rules, costs, target caps, and Sacred Chord validation.
- **World progression:** checkpoints, death recovery, save state, persistent traversal upgrades, and gate conditions.

These responsibilities may be organized into a small number of cohesive modules. Avoid a single oversized combat function that hides rules, but do not create one-file-per-small-helper abstractions without a demonstrated need.

## 10.4 Implementation Invariants

1. A weapon attack instance uses the Echo ID sampled when the attack begins.
2. A target has at most one active Resonance Mark in the current scope.
3. A valid reaction is resolved from the canonical unordered pair table; order does not matter.
4. Reaction-generated secondary damage cannot apply marks, recursively trigger reactions, or grant Harmony.
5. Break is bounded to `0..100`; Harmony is bounded to `0..100`.
6. Every timer is represented in simulation ticks, and expiry ordering is specified consistently.
7. A Boss cannot be retriggered into Break State during Break Recovery.
8. A missed Parry grants no hidden block or invulnerability.
9. No gameplay rule in Sections 3–4 depends on deferred unique weapons, Wing Assists, or traversal mutations.
10. Checkpoint and death transitions preserve valid save/world state and cannot duplicate a Lost Wick Husk stash.

## 10.5 Recommended Test Cases

- Mark refresh with the same Echo; valid and invalid Echo pair; exact 420-tick expiry boundary.
- Every one of the twelve valid recipes, plus at least one test for all invalid pair classes.
- A reaction secondary target cannot receive a mark or trigger a second reaction.
- Reaction damage and Break are additive to the triggering weapon hit exactly once.
- Boss reaches 100 Break; enters Break State; rejects Break gain during Break State/Recovery; then can be Broken again after recovery.
- Successful Parry and missed Parry at the boundaries of the 10-tick timing window.
- CounterEchoId adds only the documented +12 Break and does not grant unlisted Tier/Harmony effects.
- Harmony cannot exceed 100; Unison pays the correct cost; Sacred Chord requires the exact unordered three-Echo set.
- Death before recovery retains exactly one Lost Wick Husk and does not duplicate stored Shards.
- Switching Echo during an attack does not change that attack's sampled Echo ID.

---


# 11. Vertical Slice & Acceptance Criteria

## 11.1 Vertical Slice Goal

The vertical slice must prove that a small section can function as a complete experience, not merely a combat-technology demo. It needs exploration, combat, one Echo reaction, a checkpoint, a shortcut, and a compact Boss encounter.

### Content Scope Baseline

- One connected route: **Ash-Sump Slums → Collapsed Mine → Lantern Chamber → Warden Arena → shortcut back to Resonator**.
- Approximately 8–12 genuinely connected gameplay rooms; this is an initial organization target, not a requirement to fill space with empty rooms.
- One Tuning Resonator, one Lore/Memory reveal, and at least one shortcut opened from the far side.
- Three Echoes available in the test build: Ignis, Glacies, and Terra. Valid pairs include Melt-Shock and Tectonic Shatter; all other pairs use the default no-reaction rule.
- One Resonance Blade with ground Light 3-hit, air Light 2-hit, Heavy, Dash, Parry, and Unison.
- Dissonant Drifter, Choir Sentry, Resonance Brute, and the two-phase Ash-Sump Warden.
- HP/Harmony/Triad Deck/Mark HUD, basic map UI, Lost Wick Husk, and progress save/load.
- Initial playtime target: 15–25 minutes for a new player after a short introduction; measure rather than treating this as guaranteed duration.

## 11.2 Gameplay Acceptance Criteria

1. The player can complete the slice's main route without using an Echo outside the equipped trio.
2. Staying on one Echo maintains the weapon's baseline Damage; there is no hidden Damage penalty.
3. All twelve valid reaction recipes pass automated tests; an invalid pair cannot trigger a named reaction accidentally.
4. A target never has two simultaneous Resonance Marks; mark expiry respects exact tick boundaries.
5. Reaction payloads do not apply marks, recursively trigger reactions, or grant Harmony beyond the defined +10.
6. ParryWindow lasts exactly ten ticks; parryable hits inside are parried, hits outside are not, and Unparryable hits are never negated.
7. The Boss cannot receive ordinary Stun/forced displacement and cannot be kept in Break indefinitely across Recovery.
8. Switching Triad selection during an attack does not change the running attack instance's EchoId.
9. Dash grants no i-frames; one Air-Dash is restored only by a valid landing.
10. Death/Lost Wick Husk/checkpoint cannot duplicate or lose Shards; save/load reconstructs the same state.

## 11.3 Controls and Readability Acceptance Criteria

- Every action in Section 6 can be remapped; no gameplay action only works with its default key.
- Important actions show the correct glyph for the active Keyboard/Gamepad layout.
- Players can distinguish Parryable and Unparryable attacks through at least two of: pose/shape, VFX, audio, or color.
- After the introduction, internal playtest target: at least 80% of testers correctly classify both telegraph types in a short check.
- Internal playtest target: at least 70% of testers deliberately trigger one Signature Reaction without being told the exact button sequence. If the target is missed, improve tutorial/feedback before increasing reaction damage.
- The 80%/70% thresholds are internal targets to validate, not collected user data.

## 11.4 Technical Acceptance Criteria

- Simulation updates at fixed 120 Hz while gameplay is unpaused; render rate may differ without changing gameplay tick count.
- Replaying the same input log, seed, and initial state in the same build produces the same combat/world-progress event sequence. Cross-platform determinism must not be claimed without specific tests.
- No attack instance produces duplicate hits on a target outside its `MultiHitPolicy`.
- No assertions/test failures for `Harmony 0..100`, `Break 0..100`, expiry ticks, death state, or invalid target references.
- Save/load is tested after Echo unlock, after checkpoint, while a Lost Wick Husk exists, after recovering the Husk, and after Boss defeat.
- VFX/HUD reductions do not make telegraphs or essential gameplay data unreadable.

## 11.5 Implementation Order and Scope Lock

1. **Movement sandbox:** move/jump/coyote/buffer/ground dash/air dash/collision.
2. **Weapon sandbox:** frame data, combo buffer, per-attack hit registration, HP/Break/Hitstun.
3. **Defense sandbox:** ParryWindow, parryable/unparryable attacks, Break State/Recovery.
4. **Echo sandbox:** Triad switching, mark timing, valid/invalid pairs, all twelve reaction recipes.
5. **Encounter sandbox:** Drifter + Sentry + Brute, then the two-phase Boss.
6. **World slice:** Resonator, save/load, shortcut, map, Lost Wick Husk.
7. **Polish/playtest:** feedback, cues, accessibility, and test metrics.

Do not expand to twelve full Echo traversal kits, twelve Wing Assists, Amplitude Tiers, or multiple Bosses before the core acceptance criteria above are met.


# Appendix A — Current Scope Summary

## In Scope

- One shared Resonance Blade and shared core moveset.
- Twelve Echo identities with distinct visual palettes/glyphs and Resonance Mark IDs.
- A three-Echo quick deck configured at Tuning Resonators.
- One mark per target, 420-tick mark duration, ten named reaction types across twelve pair recipes.
- Explicitly separated HP Damage, Break, and CC.
- Parry, Phase-Inversion bonus, Break State/Recovery, Harmony, standard Unison, and four Sacred Chords.
- Vaelen lore baseline, three endings, checkpoints, death recovery, map pins, and progression-gate rules.

## Deferred

- Twelve unique weapon types and weapon-specific movesets.
- Twelve unique Wing Assist abilities.
- Twelve unique Echo traversal mutations.
- Weapon reach/damage tiers and Amplitude Ascension.
- Any unlisted reaction, automatic Air-Dash refund, uncapped healing, full-screen reaction, or unlimited control loop.
- Final tuning of controls, physics, frame data, and damage values; v0.3 values remain test baselines.
- The complete enemy/Boss roster and encounters beyond the Ash-Sump Warden prototype.

## Change Control

Any future change to reaction values, mark lifetime, Break rules, Parry rewards, Harmony economy, or Sacred Chord effects must update the relevant GDD table and its implementation tests together. The GDD is the gameplay contract; code should not infer undocumented effects from a name or visual effect.

# Appendix B — Open Decisions

The following items have not been locked through playtest data and must not be treated as completed decisions:

1. **Shared weapon name and silhouette:** `Resonance Blade` is a working name; final visual silhouette is not locked.
2. **Final physics tuning:** speed, gravity, Dash duration, and collider size need tests against actual room heights, gap widths, and sprite scale.
3. **Hit-stop and camera shake:** exact timing is not defined; either effect must preserve simulation timers.
4. **Full-game Boss roster and region count:** not locked; Ash-Sump Warden is a prototype Boss only.
5. **Memory Tuning questline and True Ending conditions:** narrative direction exists, but mission/flag breakdown is not yet specified.
6. **Resonance Sigils and non-Echo upgrades:** effects, costs, and progression remain undefined.
7. **Tuning Shards economy:** sources, upgrade prices, and carry limits remain undefined.
8. **Target hardware baseline:** not locked; performance targets must be added once build hardware is selected.

---
