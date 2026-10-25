# Status

Updated 2026-10-25.

## Where we are

Stage 0 (foundation) is done. Stage 1 (the playable prototype) is planned and starting.

Stage 0 in short:
- Step 1: game design and architecture docs (`docs/`).
- Step 2: repo, sim plugin, tests, `simrun`, CI on Linux and Windows, data checker, Unreal
  project and the `USimSubsystem` bridge. sneed's PC has Unreal 5.8.3 and Visual Studio 2026;
  the project is at `C:\Dev\Rim3D` and compiles (`docs/setup/pc-setup.md`).
- Step 3: Stage 0 tests, results in `docs/decisions/0001` (sim speed) and `0002` (characters
  and terrain). The Game Animation Sample is in `game/Content/` (not in Git, see
  `assets/manifest.json`).
- Step 4: the Stage 0 results are locked into `docs/architecture.md` (hierarchical pathfinding
  with a per-step budget counted in work, not time; animation detail by distance; real body
  types and our own skin material; runtime terrain; levels in every grid interface; sim on the
  game thread through Stage 1). Stage 1 is planned as 43 one-sitting tasks in
  `docs/plans/stage-1.md`.

Things (pawns, items, buildings) are EnTT entities made from defs since S1. Since S2 the map has
regions (16 m blocks, doors as their own regions, links, reachability groups; `Map::regions()`).
Since S5 Unreal talks to the sim only through commands (`Sim::submit`), per-step events and a
read-only snapshot; every session can be saved as a replay file and played back with
`simrun --replay`, and CI checks Linux and Windows give the same state hashes
(`docs/design/bridge.md`). Since S3 paths are hierarchical: region route, then cell A\* inside that
corridor, asked for through a queue with a work budget per step (`sim.pathfinder().request`, an
answer the next step at the earliest); a long 1 km search costs about 1,500 cells instead of
34,000 (`docs/design/pathfinding.md`). The Stage 0 hauling job (`Sim.cpp`, `HaulJob.h`) is still a
placeholder for C1. Since F3 buildings and ground have heights, so the
sim can say how hidden a pawn is from each of eight sides (`Map::cover()`), whether one pawn can see
another (`lineOfSight`, with standing, crouching and prone) and which ground a faction's enemies
can shoot at (`Sim::threatMap`); `docs/design/cover.md`. Since S6 the whole game state saves to
one binary file and loads back to exactly the same game (`saveSim` / `loadSim`, `simrun --save`,
`--load`, `--save-json`; `docs/design/saves.md`). Since S4 pawns walk instead of hopping from cell to
cell: the cell path is straightened where a pawn fits, speed follows the ground (soil 87%, sand 76%, as
RimWorld) and the slope, and pawns steer around each other deterministically; the snapshot has each
pawn's velocity, ground height and route ahead (`docs/design/movement.md`). Since U2 `USimSubsystem` steps the sim one second at a
time, passes events to `OnSimEvent`, keeps the last two snapshots and blends positions between them
(`GetBlendedLocation`, the one place sim metres become Unreal centimetres), loads `data/defs`, shows
sim timings on screen, and has keys: Space pause, 1/2/3 speed, `.` step, F7 saves a replay to
`Saved/Replays/` (that is the bug-replay key U12 planned; U12 keeps F5/F9 saves and autosave). Since U6 there is an overseer camera (`-Overseer` or console `Overseer.Enter`): WASD pan, Q/E turn, wheel zoom, middle-drag orbit, click to select a pawn, thing or ground cell (picked against the sim, not Unreal collision), with an ImGui speed bar and inspect panel; ImGui is the free VesCodes plugin in `game/Plugins/ImGui/` (not in Git, `assets/manifest.json` says how to get it back) (`docs/design/overseer.md`). **Every task that adds sim state must add it to the
save** (the doc has the steps) and to the save round-trip test.

Since D3 the game has real starting data in `data/defs/`, written by hand with RimWorld's numbers
from the RimWorld wiki (there is no RimWorld install to import from; `docs/decisions/0003`):
terrain fertility, wild plants and the temperate forest, basic items, the Human, skills, work
types, traits, our own backstories and names, the Crashlanded start, the Human body, capacities,
injuries and blood loss (`docs/design/starting-data.md`). `check_defs.py` now also checks that
defs name defs that exist. `simrun --pawns/--items` runs on the real data.
Not written yet: buildings (wall, door, bed, table, stove, sandbags), needs, thoughts, recipes,
guns and raiders; the tasks that use them (C4, C6, C7, C9, F2, F6) add them the same way.

Since 0004 the game has its own setting (`docs/design/lore.md`: the year 6650, slow star travel,
the fallen Lattice, magic called resonance) and our own names for RimWorld's proper names
(`docs/design/naming.md`: Impids are Emberkin, mechanoids the Remnant, insectoids chitinids,
storytellers are narrators named Orrin, Maren and Jinx). **Use those names in new data, code and
docs**; plain words like steel and rifle stay. Item words like plasteel are only proposed so far.

## Stage 1 progress

13 of 43 tasks done (D1, D2, D3, U1, S1, S2, S5, S6, F3, S3, U2, S4, U6). Tick tasks in `docs/plans/stage-1.md` as they merge.

## Next

Wave 1:
- ~~**D1** def database~~ done: `docs/design/defs.md`.
- ~~**D2** RimWorld importer~~ done: `docs/design/rimworld-import.md`. It has only run on test
  fixtures; nothing is imported yet.
- ~~**U1** smoke test harness~~ done: `tools/smoke.ps1`, `docs/design/smoke-tests.md`.

Wave 2:
- ~~**S1** things and the world layout~~ done: `docs/design/things.md`. `ColonySim.Build.cs` now
  exposes `ThirdParty/` (EnTT) as a public system include path; the next PC Unreal task is the
  first to compile it in Unreal.
- ~~**D3**~~ replaced: hand-written starting data instead of importing RimWorld's files
  (`docs/design/starting-data.md`). The importer stays in `tools/` unused.

Wave 3, both cloud:
- ~~**S2** regions and reachability~~ done: `docs/design/regions.md`. A door is a Things def with
  `thingClass` `Building_Door`. `simrun --regions FILE` draws the regions.
- ~~**S5** commands, events, snapshot and replays~~ done: `docs/design/bridge.md`. Only two dev
  commands so far (spawn and destroy a thing); each later system adds its own. New sim state
  must go into `Sim::stateHash()`.

Wave 4, now open: ~~**S3** hierarchical pathfinding~~ done (`docs/design/pathfinding.md`; CI runs
the 1 km, 100 pawn check as `simrun_paths`; the path queue is in the save, save format 2),
~~**S6** save and load~~ done
(`docs/design/saves.md`; saves keep the command log, so a loaded session still records a full replay;
the entity-registry hash order changed, so replays recorded before S6 have different hash values),
~~**F3** cover, sight and threat maps~~ done (`docs/design/cover.md`; `simrun --sight
FILE` draws what a pawn sees; sandbags and barricades were added to the importer list, and
trees and rocks get their height from `fillPercent` when C2 adds plants) and ~~**U2** bridge v2~~
done (PC Unreal; `docs/design/bridge.md`, smoke test `sim`; the first Unreal compile of EnTT and the
S5 bridge, so S1's include path is confirmed). **C2** plants (cloud, Sonnet medium), ~~**C8** colonists
from a seed~~ done (`docs/design/colonists.md`; `simrun --start Crashlanded` lists the six; save format 4) and **F1** health basics (cloud, Sonnet high) are now unblocked;
their defs are in `data/defs/`. Numbers marked `unverified` in a def were recalled, not read from the
wiki; check them when a task leans on them.

Wave 5: ~~**S4** smooth movement~~ done (`docs/design/movement.md`; save format 3; the hauling
placeholder asks for a new path when a pawn gives up a walk; path *choice* still ignores slopes and
sand, the doc says why). **C1** job system (cloud, Opus high) is open: its work types and skills are
in `data/defs/`. **U5** pawns with animation by distance (PC Unreal, Opus high) has both its needs
(U2, S4) and is open; it needs the PC free. ~~**U6** overseer camera and selection~~ done (PC Unreal;
smoke test `overseer`; `docs/design/overseer.md`). **U3** terrain and plants (PC Unreal, Sonnet high)
needs C2 as well.

## Waiting on sneed

- Megascans: download terrain material packs from Fab for task A2 (not needed before wave 10;
  that thread sends the steps).
