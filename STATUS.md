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

The Stage 0 placeholder hauling pawns and cell A* (`Pawn.h`, `Pathfinder`) are still in the sim;
tasks S1 and S3 replace them.

## Stage 1 progress

0 of 43 tasks done. Tick tasks in `docs/plans/stage-1.md` as they merge.

## Next

Wave 1, all three can run at once:
- **D1** def database (Cloud, Sonnet medium).
- **D2** RimWorld importer (Cloud, Sonnet high).
- **U1** smoke test harness (PC Unreal, Sonnet high; needs the PC free for about 45 minutes).

Then S1 (Opus high) once D1 is merged, and D3 (PC light) once D2 is merged.

## Waiting on sneed

- Megascans: download terrain material packs from Fab for task A2 (not needed before wave 10;
  that thread sends the steps).
