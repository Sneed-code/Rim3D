# Sim cost

Task S9. What a sim step costs at Stage 2 size, how to measure it, and what was fixed.

## Measuring

```
./build/simrun --perf [--seed 42] [--size 1024] [--days 1] [--budget MS]
```

Runs the `PerfColony` scenario (`data/defs/Scenarios`): 30 colonists on a 1 km map who build two walled
rooms (steel walls, doors, beds, stoves, wood floors), sow and harvest a 24 by 24 rice field, haul, mine
and fell, plus 300 tame animals (chickens, cows, muffalo, alpacas) fed every game hour, and the wild
animals the biome makes (about 290). Weather, light and room temperatures run as always. The colony is set
up by `colony::setUpPerfColony` (`ColonySim/PerfColony.h`), so the game's `perf` smoke test (G3,
[perf-check.md](perf-check.md)) draws the same colony and adds rain, a fire and a raid; `simrun --perf` has
none of those.

It prints, on stderr, a table of ms per step for each system, dearest first, and on stdout one JSON line
(`avgStepMs`, `p99StepMs`, `maxStepMs`, `msPerFrameAt3x`, `worstStep` and `worstSystem`, `stateHash`, and
`systems` with the same numbers as the table). With `--budget MS` it exits 1 if the average step is over.
The nightly workflow (`.github/workflows/nightly.yml`) runs a whole game day against the budget in
`docs/decisions/0006-sim-cost-budget.md` and keeps the table as a build artifact. ctest runs a short one
(`simrun_perf`, budget 10 ms) that only catches a step that has become several times dearer.

**The same state hash before and after a change is the proof that a speed-up changed nothing**:
`stateHash` for a given seed and `--days` must not move when you only make the sim faster.

The per-system times come from `Sim::setProfile(StepProfile*)`: a lap timer round each system in
`Sim::step`, off (null) unless set. It is not saved or hashed. A new system that runs every step
gets its own `SimLap` line in `Sim::step`.

## Where the time went

Before (1 game day, this machine, one core): 0.96 ms a step, 1.45 ms of every 60 fps frame at 3x.

| system | ms/step before | after |
|---|---|---|
| movement | 0.644 | 0.385 |
| mood | 0.112 | 0.071 |
| filth | 0.051 | 0.056 |
| roomStats | 0.044 | 0.044 |
| jobs | 0.038 | 0.034 |
| animals | 0.027 | 0.025 |
| the rest | 0.04 | 0.03 |
| **step** | **0.964** | **0.662** |

(Timings vary 10% from run to run; the machine matters more than the code, so compare runs on the same one.)

## What was done

- **Movement** (`MovementSystem::movePawns`, 67% of the step):
  - A pawn that stands still with nowhere to go and nobody in its space skips the neighbour list and the
    steering altogether (what the full steering comes to for it).
  - A pawn that is holding still (working) or knocked down no longer works out its speed first.
  - The steering tries its 29 candidate velocities in the order of the least their cost can be (their turn
    penalty) and stops as soon as that is over the best total found. The winner and the tie-break are
    the same, so every pawn moves exactly as before.
- **Mood** (11%): the mood target is a sum of offsets, and it built every thought's label (several string
  concatenations per pawn per step) only to throw them away; and each trait was looked up in the def by
  building path strings every step. Labels are now only built when something asks for the list, and the
  trait numbers are read from the defs once.
- **Def lookups**: `Def::number("a.b.c")` and friends no longer allocate a string for every part of the path.
- **Filth** looks only at pawns (those with a Mover), not at every thing.

## Not fixed

- ~~The hourly growth pass is a hitch.~~ Fixed in G3: once a game hour `Plants::growHour` walked the whole
  map, 14 ms in one step on 1 km, and in the game that frame took 22 to 27 ms (a dropped frame every 40 real
  seconds at 3x). The sim now grows a sixtieth of the map's cells in each of the next hour's first 60 steps
  (`Plants::growHourSlice`); every plant still grows once an hour, up to a minute later than before.
  `maxStepMs` of `simrun --perf` is now the first step (30 colonists' first jobs, a one-off at load).
- **Filth** is 8% of the step: a map lookup per pawn per step to see whether it entered a new cell. Keeping
  each pawn's last cell in a flat array would make it nearly free but changes what a save holds.
- Mood still builds the thought list per colonist per step; the mood only moves a little a second, so
  it could look every few steps.
- Steering for walking pawns is about 1 microsecond each; 300 of them walking at once cost 0.3 ms. A pawn
  with nobody within 4 m and a clear path could skip the candidates entirely (costs exactness, so not done).
