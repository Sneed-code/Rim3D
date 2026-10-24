# Status

Updated 2026-10-24.

## Where we are

Stage 0 (foundation), step 2 of 4.

Done:
- Step 1: game design and architecture docs (`docs/`).
- Step 2, cloud part: repo layout, `CLAUDE.md`, the sim plugin with a seeded map generator,
  clock, random numbers, tests, `simrun`, CI on Linux and Windows, data checker, Unreal project
  files and the `USimSubsystem` bridge (not yet compiled in Unreal).

## Next

- Step 2, PC part (needs sneed's PC): install tools (`docs/setup/pc-setup.md`), then a Remote
  Control session clones the repo, limits build cores, compiles the Unreal project, brings in
  Epic's Game Animation Sample and checks the sim log appears in play mode.
- Step 3: the three Stage 0 tests (pawns, 1 km map, sim speed). See `docs/architecture.md`.
- Step 4: lock decisions and plan Stage 1 as one-sitting tasks.

## Waiting on sneed

- PC setup, steps 1 to 6 in `docs/setup/pc-setup.md`.
