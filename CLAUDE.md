# Rim3D

A private, single-player 3D colony game in Unreal Engine 5, closely based on RimWorld and its
expansions. The owner, sneed, does not program: Claude does all the work, sneed gives ideas and playtests.

## Read first

- `STATUS.md`: where we are and what's next. Read it at the start of every task.
- `docs/game-design.md`: what the game is. `docs/architecture.md`: how it is built.
- `docs/plans/stage-<N>.md`: the current stage's task list. Read your task's row and its rules.
- `docs/design/<system>.md`: read only the one your task needs.
- `docs/decisions/`: decisions made after the two main docs; newer ones win.

## Ground rules

1. **The sim decides, Unreal shows.** All game state lives in the sim. Unreal sends commands and
   draws what the sim says. Physics, particles and animation are cosmetic.
2. **The sim has no Unreal code.** Everything in `game/Plugins/ColonySim/Source/ColonySim/` except
   `ColonySimModule.cpp` is plain C++20 that also builds with CMake.
3. **Content is data.** Items, buildings, creatures, recipes and numbers live in `data/defs/*.json`.
4. **Text over binary.** Logic and UI in C++; Blueprints only as unavoidable glue. Editor work by
   Python editor scripts where possible.
5. **RimWorld numbers are the baseline.** Never invent a number RimWorld has; tune with
   `data/tuning.json` multipliers.
6. **Deterministic sim.** All randomness through `colony::Rng`. Same seed plus same commands gives
   the same game.
7. **Keep this file under 150 lines.** Put detail in `docs/`.

## Layout

```
game/Rim3D.uproject            Unreal project
game/Source/Rim3D/             Unreal-side C++ (USimSubsystem bridge, rendering, UI, input)
game/Plugins/ColonySim/        the sim: Source/ (plugin module), Tests/, Tools/simrun/, ThirdParty/
data/defs/<Kind>/*.json        content defs, one folder per def kind
tools/                         Python: check_defs.py, the unused RimWorld importer, later editor scripts
assets/manifest.json           every third-party asset pack (packs themselves are not in Git)
```

## Build and test (cloud, no Unreal needed)

```
cmake -S game/Plugins/ColonySim -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/simrun --seed 42 --size 512 --days 1 --png map.png   # then look at map.png
python3 tools/check_defs.py
```

GitHub Actions runs all of this on Linux and Windows (MSVC) for every PR (`.github/workflows/sim.yml`).
Unreal-side code compiles only on sneed's PC, through a Remote Control session.

## Sim code conventions

- Namespace `colony`. Types `PascalCase`, functions `camelCase`, members `trailing_`, constants `kName`.
- Units: metres, game seconds. One sim step = one game second; 30 steps per real second at 1x.
- No exceptions, no RTTI, warnings are errors (GCC/Clang and MSVC /W4).
- Mark classes used from Unreal with `COLONY_API` (from `ColonySim/Api.h`).
- Unreal uses unity builds: helper names in anonymous namespaces must be unique across files
  (prefix them, e.g. `mapGen...`). Don't name anything `check`, `verify`, `ensure` or `TEXT`.
- Every sim change comes with doctest tests in `Tests/`. Behaviour across many steps gets a
  scenario test or a `simrun` check.
- Unreal-side code follows Unreal style (`F`/`U`/`A` prefixes, `PascalCase`).

## Workflow

- One task per thread, under about 2 hours. Branch, commit, open a PR.
- **Merge your own PR once CI is green** (sneed's standing decision), then tell sneed in plain
  words what changed.
- Update `STATUS.md` in the same PR. New ideas from sneed go in `BACKLOG.md`.
- Routine work: Sonnet, medium effort. Architecture, combat AI, hard bugs: Opus, high effort.

## Working with sneed

- Plain language, no jargon. Never ask sneed to edit code.
- Anything sneed has to do comes with copy-paste text or exact click-by-click steps.
- At the end of each step, say which model and effort level to use for the next thread.

## sneed's PC (Windows, RTX 5080 16 GB, 32 GB RAM)

- Don't get in the way: sneed games, runs local AI tools and keeps many browser tabs open.
- Run Unreal only when sneed says the PC is free. Never alongside VRAM-heavy tools (they crash).
- Keep Unreal sessions short and batched. Builds run at low priority with limited cores.
- Project lives at `C:\Dev\Rim3D`. Disk budget for the project: 250 GB.
