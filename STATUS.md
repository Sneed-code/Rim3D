# Status

Updated 2026-10-24.

## Where we are

Stage 0 (foundation), step 2 of 4.

Done:
- Step 1: game design and architecture docs (`docs/`).
- Step 2, cloud part: repo layout, `CLAUDE.md`, the sim plugin with a seeded map generator,
  clock, random numbers, tests, `simrun`, CI on Linux and Windows, data checker, Unreal project
  files and the `USimSubsystem` bridge.
- Step 2, PC part: sneed's PC has Unreal 5.8.3, Visual Studio 2026 with C++ game tools, git and
  git-lfs. Project cloned to `C:\Dev\Rim3D`, Defender skips `C:\Dev` and the UE 5.8 folder,
  Unreal builds limited to 6 parallel actions, and the editor target compiles cleanly.

## Next

- Step 3: bring in Epic's Game Animation Sample (sneed creates it from the launcher first), then
  the three Stage 0 tests (pawns, 1 km map, sim speed). See `docs/architecture.md`.
- Step 4: lock decisions and plan Stage 1 as one-sitting tasks.

## Waiting on sneed

- Create the Game Animation Sample project from the Epic launcher (step 6 of
  `docs/setup/pc-setup.md`, Unreal 5.8, saved to `C:\Dev\GameAnimationSample`).
