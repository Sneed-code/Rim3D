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
34,000 (`docs/design/pathfinding.md`). The Stage 0 hauling job (now in `Sim.cpp`) is still a
placeholder for C1. Since F3 buildings and ground have heights, so the
sim can say how hidden a pawn is from each of eight sides (`Map::cover()`), whether one pawn can see
another (`lineOfSight`, with standing, crouching and prone) and which ground a faction's enemies
can shoot at (`Sim::threatMap`); `docs/design/cover.md`.

## Stage 1 progress

8 of 43 tasks done (D1, D2, U1, S1, S2, S5, F3, S3). Tick tasks in `docs/plans/stage-1.md` as they merge.

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
- **D3** (PC light, Sonnet medium): runs the importer on sneed's PC. Until then `simrun
  --pawns/--items` needs `--data game/Plugins/ColonySim/Tests/data` (test fixture defs).

Wave 3, both cloud:
- ~~**S2** regions and reachability~~ done: `docs/design/regions.md`. A door is a Things def with
  `thingClass` `Building_Door`. `simrun --regions FILE` draws the regions.
- ~~**S5** commands, events, snapshot and replays~~ done: `docs/design/bridge.md`. Only two dev
  commands so far (spawn and destroy a thing); each later system adds its own. New sim state
  must go into `Sim::stateHash()`.

Wave 4, now open: ~~**S3** hierarchical pathfinding~~ done (`docs/design/pathfinding.md`; CI runs
the 1 km, 100 pawn check as `simrun_paths`; the save must store the path queue, see that doc),
**S6** save and load (cloud,
Sonnet high), ~~**F3** cover, sight and threat maps~~ done (`docs/design/cover.md`; `simrun --sight
FILE` draws what a pawn sees; sandbags and barricades were added to the importer list, and
trees and rocks get their height from `fillPercent` when C2 adds plants) and **U2** bridge v2 (PC
Unreal, Sonnet high; the first Unreal compile of EnTT and the S5 bridge, plus a dev key that saves
a replay to `Saved/Replays/`). C2, C8 and F1 in wave 4 also wait for **D3** (the RimWorld import
on sneed's PC).

Wave 5: **S4** smooth movement (cloud, Sonnet high) is open now that S3 is in. **C1** job system
(cloud, Opus high) waits for D3.

## Waiting on sneed

- Megascans: download terrain material packs from Fab for task A2 (not needed before wave 10;
  that thread sends the steps).
