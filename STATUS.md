# Status

Updated 2026-10-24.

## Where we are

Stage 0 (foundation): steps 1 to 3 of 4 are done.

Done:
- Step 1: game design and architecture docs (`docs/`).
- Step 2, cloud part: repo layout, `CLAUDE.md`, the sim plugin with a seeded map generator,
  clock, random numbers, tests, `simrun`, CI on Linux and Windows, data checker, Unreal project
  files and the `USimSubsystem` bridge.
- Step 2, PC part: sneed's PC has Unreal 5.8.3, Visual Studio 2026 with C++ game tools, git and
  git-lfs. Project cloned to `C:\Dev\Rim3D`, Defender skips `C:\Dev` and the UE 5.8 folder,
  Unreal builds limited to 6 parallel actions, and the editor target compiles cleanly.
- Step 3: Game Animation Sample copied into `game/Content/` (not in Git, see
  `assets/manifest.json`), its config and plugins in the project. The three Stage 0 tests ran:
  - Sim speed: `docs/decisions/0001-stage0-sim-speed.md`. 1 km map stays; region pathfinding and
    a per-step path budget are required.
  - Characters and terrain: `docs/decisions/0002-stage0-characters-and-terrain.md`. MetaHumans
    stay with animation LOD; body variety needs real body types and our own skin material;
    runtime terrain mesh is cheap.
  - Placeholder hauling pawns and cell A* are in the sim (`Pathfinder`, `Pawn.h`); they are
    replaced by the real job system and region pathfinding in Stage 1.

## Next

- Step 4: lock decisions (update `architecture.md` from 0001 and 0002) and plan Stage 1 as
  one-sitting tasks.

## Waiting on sneed

- Megascans: download terrain material packs from Fab for the terrain look comparison
  (can wait until Stage 1).