# Status

Updated 2026-10-24.

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

Things (pawns, items, buildings) are EnTT entities made from defs since S1. The Stage 0 hauling
job (now in `Sim.cpp`) and cell A* (`Pathfinder`) are still placeholders; C1 and S3 replace them.

## Stage 1 progress

4 of 43 tasks done (D1, D2, U1, S1). Tick tasks in `docs/plans/stage-1.md` as they merge.

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
- **D3** (PC light, Sonnet medium): runs the importer on sneed's PC. `simrun --pawns/--items`
  needs its `Human` and `Steel` Things defs.

Wave 3, both cloud, can run at once now that S1 is merged: **S2** regions and reachability (Opus
high) and **S5** commands, events, snapshot and replays (Opus high).

## Waiting on sneed

- Megascans: download terrain material packs from Fab for task A2 (not needed before wave 10;
  that thread sends the steps).
