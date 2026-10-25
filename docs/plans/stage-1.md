# Stage 1: the playable prototype

_Planned 2026-10-24. Builds on [`architecture.md`](../architecture.md) as locked after Stage 0.
Target: the "Prototype" milestone in [`game-design.md`](../game-design.md), weeks 3 to 4._

## What "done" looks like

Stage 1 is done when sneed can play this in a packaged build:

- A seeded 512 m map. You start as the protagonist with 5 colonists and crashlanded gear.
- Colonists mine, chop, harvest wild plants, haul to stockpiles, and build a walled, roofed room
  with a door, beds, a table and a stove. They cook, eat, sleep on a schedule, and have a mood.
- You set work priorities, schedules and stove bills, place buildings and designate mining and
  chopping from the overseer view.
- You walk the colony in 1st or 3rd person, chop and build by hand, and switch to the overseer
  camera and back with one key.
- One raid of armed raiders arrives. Both sides take cover, shoot real projectiles, fall back and
  rout. Pawns get hurt in the body part that was hit, go down and die.
- F5 quicksaves, F9 loads.
- Packaged build on sneed's PC: 60 fps at 1440p with 100 animated pawns, and with 30 colonists
  fighting 40 raiders; sim under 5 ms per frame at 3x.
- CI runs a 5-day colony scenario and a raid scenario on every PR, both green.

**Not in Stage 1** (vanilla core, Stage 2 onwards): upper floors and stairs, temperature, fire,
power, research, medicine and doctors, rescue, prisoners, animals, growing zones, apparel beyond
starting gear, mental break behaviour, trade, events other than the one raid, playing on 1 km maps
(the sim supports them; the prototype plays on 512 m), the styled UI.

## Rules for every Stage 1 task

1. Read `STATUS.md`, your task's row below, the architecture sections it names, and the
   `docs/design/` doc of the system you touch. Nothing else unless you need it.
2. A task that starts a new system writes its short doc in `docs/design/<system>.md`.
3. Numbers come from the hand-written defs in `data/defs/` (`docs/design/starting-data.md`). If a
   def or number you need isn't there yet, add it, with RimWorld's number from the RimWorld wiki
   (numbers only) rather than making one up; mark recalled numbers `unverified` and our own `ours`.
4. After S6 (saves), every task that adds sim state adds it to the save and to the save
   round-trip test.
5. **Unreal tasks** (marked PC Unreal): write the code in the cloud first, then ask sneed
   "Is the PC free for about 45 minutes?" and compile, run the smoke test and take screenshots in
   one Remote Control session at low priority. Never run Unreal while sneed is gaming or running
   local AI tools.
6. Finish with: tick your row (✓) in this file, update `STATUS.md`, open the PR, merge it once CI
   is green, and tell sneed in plain words what changed and which task and model come next.

**Where it runs:**
**Cloud** needs nothing from sneed.
**PC light** uses Remote Control on sneed's PC but no Unreal, so it's fine while sneed is using the PC.
**PC Unreal** needs the PC free (no game or local AI running) for up to about 45 minutes.

**Model** is the model and effort level to pick when starting the thread.

## Tasks

Times are one sitting of thread work. "Needs" lists tasks that must be merged first.

### Data (D)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | D1 | **Def database.** Vendor nlohmann/json. Load every `data/defs/<Kind>/*.json` at sim start, resolve `parent` and `abstract`, give each def a compact id (saves still use `defName`), apply `data/tuning.json` multipliers. Same rules as `check_defs.py`. | Cloud | Sonnet, medium | 60 min | none |
| ✓ | D2 | **RimWorld importer.** `tools/rimworld_import.py`: reads RimWorld's Core Defs XML (expansions later) and writes JSON defs plus short summaries in `docs/reference/`. Prototype list: terrain, rock and ore, wood, steel, components, wall, door, bed, table, stove, raw food, berries, simple meal, packaged survival meal, food and rest needs, the thoughts the prototype uses, skills, a set of traits and backstories, the Human body, 3 or 4 guns, the raider pawn kind and faction. Converts ticks to game seconds (`Clock.h`). Tested on small XML samples in `tools/tests/`. | Cloud | Sonnet, high | 90 min | none |
| ✓ | D3 | **Starting data, written by hand** (replaced "run the import on sneed's PC": no RimWorld install to read, `decisions/0003`). Defs for C1, C2, C8 and F1 with RimWorld's numbers from the RimWorld wiki, converted like D2; our own backstories and names; the data checker also checks references between defs. Doc: `design/starting-data.md`. | Cloud | Opus, high | 90 min | D1 |

### Sim foundations (S)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | S1 | **Things and the world layout.** Vendor EnTT. Things (pawns, items, buildings) become entities with a def, position and components. Grid functions take `CellPos{x, y, level}`; add the building layer (cell to entity). Replace the Stage 0 `Pawn`/`Item` placeholders; the hauling test stays green. Doc: `design/things.md`. | Cloud | Opus, high | 90 min | D1 |
| ✓ | S2 | **Regions and reachability.** Regions inside 16 m blocks, doors as their own region, links between regions, reachability groups, rebuilding only the dirty block when a cell changes. Unit tests plus a simrun picture of regions. Doc: `design/regions.md`. | Cloud | Opus, high | 90 min | S1 |
| ✓ | S3 | **Hierarchical pathfinding.** Region-graph A\* then cell A\* inside the route's regions; queue with a per-step budget in cells expanded; unreachable requests rejected by reachability groups. Done when the 1 km simrun runs (`--size 1024 --pawns 100`) have no step over about 3 ms and no failed jobs. Doc: `design/pathfinding.md`. | Cloud | Opus, high | 90 min | S2 |
| ✓ | S4 | **Smooth movement.** Smooth cell paths into straight lines where the ground allows, slope and terrain speed, deterministic local avoidance so pawns step around each other. | Cloud | Sonnet, high | 90 min | S3 |
| ✓ | S5 | **Commands, events, snapshot and replays.** The bridge contract from `architecture.md`: commands applied at step boundaries and logged, events per step, a read-only snapshot, replay files (seed plus command log) and `simrun --replay`. Test: same seed and commands give the same state hash. Doc: `design/bridge.md`. | Cloud | Opus, high | 90 min | S1 |
| ✓ | S6 | **Save and load.** Versioned binary save of all sim state, `defName` references, JSON dump for debugging. Test: save, load and run on gives the same hash as never saving. Doc: `design/saves.md`. | Cloud | Sonnet, high | 90 min | S5 |

### Colony life (C)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | C1 | **Job system.** RimWorld-style think list (emergencies, orders, needs, schedule, work), work types and priorities from data, work scanners that weigh distance and skip unreachable targets, reservations, jobs as steps with fail checks. Stockpile zones and hauling move onto it. Doc: `design/jobs.md`. | Cloud | Opus, high | 90 min | S3, D3 |
| | C2 | **Plants.** Plant grid layer from defs; map generation places trees, berry bushes and grass by fertility; hourly growth. Shows in the simrun picture. | Cloud | Sonnet, medium | 60 min | S1, D3 |
| | C3 | **Mining, chopping and harvesting.** Designations and jobs: rock cells become rock chunks or ore, trees become wood, bushes give berries. Mining rebuilds regions. Work speed from skill. | Cloud | Sonnet, high | 90 min | C1, C2 |
| | C4 | **Building.** Blueprints, frames, hauling materials to frames, build jobs, deconstruct. Wall, door, bed, table and stove from defs. Buildings block cells and rebuild regions. Doc: `design/construction.md`. | Cloud | Sonnet, high | 90 min | C1 |
| | C5 | **Doors, roofs and rooms.** Doors open for pawns and cost path time; auto-roof over enclosed areas; rooms from enclosed regions with a role (bedroom, kitchen, dining room) and size. | Cloud | Sonnet, high | 90 min | C4 |
| | C6 | **Food, rest and schedules.** Food and rest needs at RimWorld rates, eat job (best reachable food, at a table if there is one), sleep job (own bed, else the ground), bed ownership, hourly schedules. Doc: `design/needs.md`. | Cloud | Sonnet, high | 90 min | C1 |
| | C7 | **Bills and cooking.** Minimal bill and recipe system on the stove ("cook simple meal" do-forever and do-X-times), fetching ingredients, cooking skill. | Cloud | Sonnet, high | 90 min | C4, C6 |
| | C8 | **Colonists from a seed.** Name, adult age, backstories, traits, skills with passions and experience, from the defs (`design/starting-data.md`); crashlanded start (protagonist plus 5 colonists, starting gear). The protagonist is flagged for 2x part hit points and the talent tree later. | Cloud | Sonnet, medium | 60 min | S1, D3 |
| | C9 | **Mood.** Thoughts (ate without table, slept on the ground, slept outside, hungry, tired, ate raw food, ate a fine meal), mood value, mental break thresholds recorded (breaks themselves come later). | Cloud | Sonnet, medium | 60 min | C6, C8 |
| | C10 | **Colony scenario test.** `simrun --scenario colony`: 6 colonists on a 512 m map, a script of commands builds a room with beds, a table and a stove, designates mining and chopping, and runs 5 days. Checks: nobody starves, nobody stuck over a game hour, all designations done. Runs in CI. | Cloud | Sonnet, high | 90 min | C3, C5, C7, C9 |

### Fighting (F)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | F1 | **Health basics.** Body parts from the Human body defs, hit points per part (2x for the protagonist), injuries with bleeding and pain, downed, death, slow natural healing. Doc: `design/health.md`. | Cloud | Sonnet, high | 90 min | S1, D3 |
| | F2 | **Guns, projectiles and hitboxes.** Weapon defs, firing with spread and recoil, projectiles with speed and drop, hits against terrain, walls and per-part hitboxes posed for standing, crouching and prone; armor penetration, then injury to the part hit. Doc: `design/combat.md`. | Cloud | Opus, high | 90 min | F1, S4 |
| ✓ | F3 | **Cover, sight and threat maps.** Cover strength per cell in 8 directions, rebuilt only where something is built or destroyed; line-of-sight queries; a threat map per faction. | Cloud | Sonnet, high | 90 min | S2 |
| | F4 | **Fighter AI: where to stand.** Re-plan about twice a second by scoring nearby cells (cover from known enemies, sight to a target, weapon range, distance, crowding); crouch and lean at cover; target choice per the design doc; draft, move and attack orders. | Cloud | Opus, high | 120 min | F2, F3, C1 |
| | F5 | **Fighter AI: aiming.** Reaction time, spread, recoil and leading moving targets set by Shooting skill; reloading; sidestepping slow projectiles. | Cloud | Opus, high | 90 min | F4 |
| | F6 | **Raid and defense.** Raider faction and pawn kind, raid enters at a map edge sized by RimWorld threat points, assault plan; squad morale (advance, hold, fall back, rout and flee). Colony alarm command with Defender, Shelter and Keep working roles. | Cloud | Opus, high | 120 min | F5, C8 |
| | F7 | **Raid scenario tests and nightly soak.** Scripted raids over many seeds checking the design doc's fighter rules (for example under 10% of a firefight in the open when cover is within 10 m); a nightly GitHub Actions run reporting deaths, stuck pawns and fight length. | Cloud | Sonnet, high | 90 min | F6 |

### Unreal side (U)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | U1 | **Smoke test harness.** Turn `Stage0Test` into `-SmokeTest=<name>` runs (load, run, screenshots and timings to `Saved/Smoke/`, quit) and a PowerShell script that builds at low priority, runs a smoke test and reports. Every later U task uses it. | PC Unreal | Sonnet, high | 60 min | none |
| ✓ | U2 | **Bridge v2.** `USimSubsystem` on commands, events and snapshots; loads `data/defs`; blends between snapshots; pause and 1x/2x/3x keys; on-screen sim timings. | PC Unreal | Sonnet, high | 60 min | S5, U1 |
| | U3 | **Terrain and plants.** Terrain chunk component (from the Stage 0 code), one chunk rebuilt when a cell is mined; plants as instanced meshes from the plant layer. | PC Unreal | Sonnet, high | 90 min | U2, C2 |
| | U4 | **Buildings and items.** Instanced meshes per type per chunk from each def's `visual`; blueprints as see-through ghosts, frames part-built; items on the ground and in stockpiles. | PC Unreal | Sonnet, high | 90 min | U3, C4 |
| | U5 | **Pawns with animation by distance.** Pawn actors without CharacterMovement, driven by snapshots; sim velocity and path as the motion-matching trajectory; full motion matching for the 20 nearest, cheaper and slower updates farther out, no strand hair far away; action montages from sim events. Smoke test with 100 sim pawns. | PC Unreal | Opus, high | 90 min | U2, S4 |
| ✓ | U6 | **Overseer camera and selection.** Free top-down camera (pan, zoom, rotate), click to select pawns, things and cells; add a free ImGui plugin (logged in `assets/manifest.json`) for the inspect panel and speed controls. | PC Unreal | Sonnet, high | 90 min | U2 |
| | U7 | **Orders.** Architect menu (wall, door, bed, table, stove, stockpile zone) with grid-snapped ghost placement; mine, chop, harvest and cancel designations; draft, move and attack orders; alarm button. | PC Unreal | Sonnet, high | 90 min | U4, U6, C3 |
| | U8 | **Colony panels.** Work tab, schedule tab, stove bills, colonist panel (needs, mood and thoughts, skills, health), message log. All ImGui. | PC Unreal | Sonnet, medium | 90 min | U6, C7, C9 |
| | U9 | **Protagonist.** Game Animation Sample character as the protagonist, position sent to the sim each step, 1st/3rd person toggle, one key between direct and overseer mode (the AI takes over), possessing another colonist. | PC Unreal | Sonnet, high | 90 min | U5, U6, C8 |
| | U10 | **Hands-on work.** Look at a thing and hold interact to get the sim job (chop, mine, build, pick up); in direct mode, a build ghost that snaps to the grid. | PC Unreal | Sonnet, high | 90 min | U9, U7 |
| | U11 | **Fights on screen and shooting.** Tracers, muzzle flashes, impacts, hit reactions, cosmetic ragdolls, placeholder wound decals; the protagonist aims and fires through the sim; a hitbox debug view to check hits match bodies. | PC Unreal | Sonnet, high | 90 min | U9, F2 |
| | U12 | **Saves and bug replays in game.** F5 quicksave, F9 quickload, daily autosave; a bug key that writes a replay file I can run in the cloud. | PC Unreal | Sonnet, medium | 60 min | S6, U2 |

### Art (A)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | A1 | **Placeholder art.** Free meshes for wall, door, bed, table, stove, logs, steel, meals, berries, trees and bushes; set in the defs' `visual` fields by an editor script; logged in `assets/manifest.json`. | PC Unreal | Sonnet, medium | 60 min | U4 |
| | A2 | **Terrain materials.** Megascans ground, rock and sand on the runtime terrain by terrain type and slope; side-by-side screenshots against a Landscape so sneed can judge. | PC Unreal | Sonnet, high | 90 min | U3, sneed's Megascans download |
| | A3 | **Seeded pawn looks.** Our own skin material (tint, lineage colour), MetaHuman body types and morph targets mixed by seed; Emberkin test with red skin and horns readable from the overseer camera. May take two sittings. | PC Unreal | Opus, high | 90 min | U5 |

### Wrap-up (G)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | G1 | **Performance check in a packaged build.** 100 animated pawns, and 30 colonists against 40 raiders; record fps, game thread, GPU and sim cost in the next free `docs/decisions/` file. Fix or plan fixes for any miss. | PC Unreal | Sonnet, high | 90 min | U11, F6, U5 |
| | G2 | **Playtest build.** Package the prototype, write `docs/setup/playtest.md` (how to start it, keys), sneed plays; notes go to `BACKLOG.md` and the Stage 2 plan. | PC Unreal | Sonnet, medium | 60 min | everything above it except A2 and A3 |

## Order

43 tasks, roughly 60 hours of thread work. Cloud tasks with their needs met can run side by side
when they touch different systems; Unreal tasks go one at a time. The waves below are the
suggested order; within a wave, start any task whose needs are merged.

| Wave | Cloud | PC |
| --- | --- | --- |
| 1 | D1, D2 | U1 |
| 2 | S1, D3 | |
| 3 | S2, S5 | |
| 4 | S3, S6, C2, C8, F1, F3 | U2 |
| 5 | S4, C1 | U3, U6 |
| 6 | C3, C4, C6, F2 | U5 |
| 7 | C5, C7, C9 | U4 |
| 8 | C10, F4 | U7, U8, U9, A1 |
| 9 | F5 | U10, U12, A3 |
| 10 | F6 | U11, A2 |
| 11 | F7 | G1 |
| 12 | | G2 |

The long chain is D1, S1, S2, S3, C1, then the colony and fight tasks. Opus is used only on that
chain's architecture tasks and the combat AI (11 tasks); everything else is Sonnet.

**If we fall behind** at the week 2 pace check, these move to Stage 2 first, in this order: A2
terrain materials, A3 seeded looks, U12 bug replays in game, F7 nightly soak, U10 hands-on work.
The prototype still plays without them.

## What sneed does in Stage 1

- **Say when the PC is free** when a thread asks. Unreal tasks need up to about 45 minutes with no
  game or local AI running. (No PC light tasks are left: D3 became a cloud task.)
- **Approve Remote Control** when the prompt appears.
- **Download the Megascans packs** for A2 when that thread sends the click-by-click steps. It isn't
  needed before wave 10.
- **Look at screenshots** along the way, and **play the prototype** at G2.
