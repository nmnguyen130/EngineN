# VEIL OF THE SILENT
## Master Game Design Document — Design Baseline v0.2

**Document status:** Revised design baseline; Sections 1–4 are the current reviewed scope. Values marked *prototype baseline* are explicit starting values for implementation and playtesting, not claims of final balance.

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

**Reference points:** *Hollow Knight* for interconnected exploration; *Nine Sols* for readable deflection-focused combat; *TEVI* and *BlazBlue Entropy Effect* for expressive action-system ideas. These are design references, not features to reproduce directly. The goal is to establish a distinct identity through Vaelen's resonance rules, narrative, exploration, and combat decisions.

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

- **Parry timing window:** A Parry input is successful only if it is issued within the 10 ticks (approximately 83 ms) before a parryable hit connects. The exact animation active frames must be aligned to this rule in implementation.
- **Successful Parry:** Negates that incoming hit, interrupts the attacker if interruptible, grants 15 Harmony, and adds 18 Break to the attacker if it has a Break meter.
- **Phase-Inversion bonus:** A Boss attack may declare one `CounterEchoId` in its authored attack data. If the active Echo sampled at Parry input matches that ID, the Parry adds another 12 Break. The matched Parry therefore adds 30 total Break (18 base + 12 bonus), compared with 18 Break for a normal successful Parry. This is a bonus, not a requirement for a successful Parry.
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

- **Cost:** 50 Harmony, paid at activation.
- **Area:** 3 m radius around Kaelen.
- **Targets:** Up to five eligible enemies, selected by the combat targeting rule.
- **Effect per target:** 18 HP Damage and 15 Break.
- **CC:** Standard enemies are interrupted for 30 ticks (0.25 s). Bosses receive no Stun or forced displacement.
- **Marks and reactions:** Unison damage does not apply Resonance Marks and cannot trigger Signature Reactions.

### Sacred Chords

A Sacred Chord is available only at exactly 100 or more Harmony and only when the three distinct Echoes in the Triad Deck match one of the exact sets below; slot order does not matter. Activating the separate Sacred Chord action consumes all 100 Harmony. The Standard Unison action remains independently available and consumes 50 Harmony when used instead.

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

# 6. EngineN / C++23 Implementation Boundaries

This section defines implementation-facing concepts, not a requirement to create a separate source file for every item. Keep code responsibilities clear without over-fragmenting the project.

## 6.1 Runtime Baseline

- Language: C++23.
- Runtime: EngineN custom runtime.
- Simulation target: fixed 120 Hz step.
- Suggested compiler baseline from the project: `-std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror` where supported by the build toolchain.
- A fixed simulation rate does not by itself guarantee cross-platform determinism. Randomness, ordering, collision resolution, timer updates, and input sampling must also use documented deterministic rules if deterministic replay is required.

## 6.2 Core Data Concepts

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

## 6.3 Suggested Responsibilities

- **Echo/Resonance data:** immutable definitions for Echo IDs, names, glyphs, palettes, and valid reaction recipes.
- **Weapon combat:** shared attack states, sampled Echo ID per attack instance, hit validation, base Damage, and base Break.
- **Mark/reaction resolution:** mark expiry, valid-pair lookup, mark consumption/replacement, reaction payload application, and anti-recursion rules.
- **Defense and posture:** Parry window validation, Break meter progression, Break State, Break Recovery, and explicit CC rules.
- **Harmony/Unison:** resource caps, gain rules, costs, target caps, and Sacred Chord validation.
- **World progression:** checkpoints, death recovery, save state, persistent traversal upgrades, and gate conditions.

These responsibilities may be organized into a small number of cohesive modules. Avoid a single oversized combat function that hides rules, but do not create one-file-per-small-helper abstractions without a demonstrated need.

## 6.4 Implementation Invariants

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

## 6.5 Recommended Test Cases

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
- Final input bindings, full player movement spec, animation frame data, enemy-specific data tables, and boss-by-boss encounter scripts.

## Change Control

Any future change to reaction values, mark lifetime, Break rules, Parry rewards, Harmony economy, or Sacred Chord effects must update the relevant GDD table and its implementation tests together. The GDD is the gameplay contract; code should not infer undocumented effects from a name or visual effect.
