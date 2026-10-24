# VEIL OF THE SILENT — GDD v0.4
## World & Progression Blueprint

**Document type:** Incremental GDD update; this is not a rewrite of v0.3.  
**Source of truth:** `VeilOfTheSilent_GDD_v0.3_EN.md` remains authoritative for combat, input, movement baseline, enemy/Boss contracts, and runtime. Used together, v0.3 + this document form GDD v0.4.

## 1. Scope and design decisions

V0.4 defines the macro-region structure, Echo cadence, narrative beats, ending conditions, and minimum content budget per region. It does not change combat formulas or prototype values defined in v0.3.

- **One weapon, twelve Echoes:** Echoes change resonance mark, palette, and VFX; they do not add separate movesets or traversal mutations.
- **Triad Deck:** three Echoes are equipped at a Resonator. No main-path gate requires a specific Echo to be currently equipped.
- **World progression:** uses persistent world flags, boss seals, and permanent traversal capabilities to be specified separately after the movement prototype; it does not rely on Echo color changes.
- **Exploration order:** the opening is guided, three midgame regions can be completed in flexible order, and the final region opens after all three are complete.
- Boss names other than Ash-Sump Warden are working names and may change without affecting progression structure.

## 2. High-level world map

```text
Ash-Sump Slums
      ↓
Collapsed Mine → Lantern Chamber → Ash-Sump Warden
                                      ↓
                                Lower Junction
                                      ↓
                              Glass Conservatory
                                      ↓
                  ┌───────────────────┼───────────────────┐
                  ↓                   ↓                   ↓
              Choirworks        Drowned Archive     Gravity Foundry
                  └───────────────────┼───────────────────┘
                                      ↓
                              Silent Reliquary
                                      ↓
                              Spire of Unison
```

This is a logical topology, not a distance map. The three branches after Glass Conservatory can be explored in any order. Each branch must provide a shortcut or shortened return route; the player should not have to replay the entire region to return to Lower Junction.

## 3. Region sheets

| Region | Planned Echoes | Core experience | Boss / progression lock | Narrative evidence |
|---|---|---|---|---|
| **R0 — Ash-Sump Slums & Collapsed Mine** | Ignis, Terra | Movement/combat onboarding, Resonance Mark, Lantern discovery, first shortcut | **Ash-Sump Warden**; opens Lower Junction | Tuning Collars classify citizens by their ability to synchronize; they are not merely protective devices |
| **R1 — Glass Conservatory** | Glacies, Lux | Reflective surfaces, brittle crystal, visibility, environmental pattern recognition | **Prism Keeper** *(working name)*; opens three midgame branches | First physical evidence that the surface was not destroyed as claimed |
| **R2 — Choirworks** | Fulgur, Stridor | Machinery, resonance routing, coordinated enemies, readable attack cues | **Relay Cantor** *(working name)*; grants 1 Conductor Seal | How the Spire routes voices and memories through Tuning Collars |
| **R3 — Drowned Archive** | Aqua, Aether | Flooded spaces, inertia, target links, memory storage | **Drowned Curator** *(working name)*; grants 1 Conductor Seal | Stored memories were edited; some records contradict official history |
| **R4 — Gravity Foundry** | Ventus, Gravitas | Vertical spaces, airflow, mass manipulation, industrial machinery | **Weightless Bailiff** *(working name)*; grants 1 Conductor Seal | Evidence of the Twelve Resonators' rebellion and the consequences of interfering with the Veil |
| **R5 — Silent Reliquary & Spire** | Umbra, Tempus | Remnants of the Twelve Resonators, distorted resonance rules, final approach | High Conductor; ending choice | Full truth of the Veil, the twelve living anchors, and the Lantern's original purpose |

**Region rule:** Every region needs a memorable landmark, a readable main route, at least one shortcut or loop-back, verifiable narrative evidence, an optional challenge, and a climax encounter. Do not pad regions to meet an arbitrary room count.

## 4. Echo unlock cadence

| Milestone | New Echoes | Design note |
|---|---|---|
| Lantern Chamber | Ignis | First Echo; teaches Resonance Mark |
| After Ash-Sump Warden | Terra | Adds a Break/heavy-attack interaction aligned with v0.3 |
| Glass Conservatory | Glacies, then Lux | Enables Ignis + Glacies; neither Echo is mandatory for main-path completion |
| Choirworks | Fulgur, Stridor | Adds precision/target interactions and their valid recipes |
| Drowned Archive | Aqua, Aether | Adds eligible target-link recipes |
| Gravity Foundry | Ventus, Gravitas | Adds recipes requiring these Echoes; does not automatically grant traversal mutations |
| Silent Reliquary | Umbra, Tempus | Completes the set of twelve before the ending |

The vertical slice may grant Ignis, Glacies, and Terra together as a test fixture. This is a testing exception, not the full-game unlock order.

**Do not force deck changes:** after unlocking an Echo, the player may keep their current Triad. At a Resonator, any three unlocked Echoes can be equipped. A reaction tutorial may require a valid equipped recipe, but must first explain deck swapping.

## 5. Narrative gates & Memory Tuning

Every major narrative revelation needs at least two evidence types in the game (for example, environmental evidence + Echo imprint, or a record + witness). Do not put the entire truth in one optional dialogue.

Memory Tuning contains four progression flags:

1. **Surface Lie:** establish that the surface still exists and access to it was concealed.
2. **The Collars:** prove that Tuning Collars harvest identity/data rather than merely protect citizens.
3. **The Veil:** establish that the Veil was a suppression protocol whose failure became a catastrophe; do not attribute every consequence to one person alone.
4. **The Twelve:** confirm the Twelve Resonators' goal and the difference between Echo imprints and their imprisoned original consciousnesses.

Each flag needs a concrete objective/interaction, evidence reviewable in the Journal, and a clear change in Kaelen's understanding. These must not become anonymous collectibles without narrative development.

## 6. Ending conditions

| Ending | Minimum condition | Design outcome |
|---|---|---|
| **A — The False Dawn** | Defeat the High Conductor; choose to leave Vaelen through the escape route | Kaelen escapes; the extraction system below remains unresolved |
| **B — The Shattered Spire** | Defeat the High Conductor; reach the core and confirm its destruction | The extraction network is severed; Vaelen's infrastructure destabilizes and survivors must rebuild |
| **C — Symphony of the Off-Beat** | Collect all 12 Echoes + complete all four Memory Tuning flags + defeat the High Conductor | Unlock the option to rewrite the relay; Kaelen loses the ability to use the Lantern as a weapon; the dead are not resurrected and all memories are not restored |

Conditions must be visible through Journal/quest state, not hidden guesses. Ending C is unavailable until its conditions are met; the game must not falsely report full completion.

## 7. Content & reward budget

Every region needs only these content layers to function:

- **Main path:** enough information to locate the next objective without an external guide.
- **Echo reward:** unlocks the Echo listed in Section 4; does not increase baseline damage.
- **Memory evidence:** advances Memory Tuning or adds to the Lore Journal.
- **Shortcut:** reduces backtracking and connects the region to the hub or a known route.
- **Optional mastery:** combat/platforming challenge for a secondary reward.
- **Tuning Shards / Sigils:** only use them when source, price, effect, and limit are specified. V0.4 does not lock untested economy values.

Resonance Sigils and Tuning Shards economy remain separate design tasks. Do not place undefined upgrades or rewards on the main path as if they are implemented.

## 8. V0.4 acceptance criteria

- The player can identify the main-path objective from the Journal/map regardless of equipped Echoes.
- The three middle branches can be completed in different orders without corrupting narrative flags or save/load.
- Each Echo unlocks once and persists through death/load.
- No main-path gate requires an Echo or a traversal mutation deferred in v0.3.
- All four Memory Tuning flags have clear evidence, completion conditions, and Journal state.
- Ending C appears only after all 12 Echoes and all four flags; Endings A/B remain possible without all optional content.
- Every region has a landmark, shortcut/loop-back, narrative evidence, and climax encounter; optional content does not obscure the main route.

## 9. Not locked yet

Not yet locked: room-by-room layouts, final names for bosses other than Ash-Sump Warden, total side-quest count, economy values, Resonance Sigil list, and names/stats for permanent traversal upgrades. Decide these after validating the macro-map and movement prototype; do not infer them from v0.4.

---

**Versioning rule:** v0.4 adds world/content progression. It does not copy or replace v0.3's gameplay/technical contracts. As region details are prototyped and playtested, confirmed changes should be rolled into the next GDD version.
