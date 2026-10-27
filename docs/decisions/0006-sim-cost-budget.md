# 0006: Sim cost budget

_Decided 2026-10-26 in task S9 (Claude). Easy to change: the numbers are in `.github/workflows/nightly.yml`
and here._

**What.** On the performance colony (`simrun --perf`: 1 km map, 30 colonists, 300 tame and about 290 wild
animals, crops, two walled rooms, weather) the **average sim step must cost no more than 2.0 ms** on a
GitHub Linux runner, and the **target is 1.0 ms**. The nightly workflow runs a whole game day and fails
over 2.0. A system that runs every step should cost under 0.1 ms at this size; movement may take up to 1.0 ms.

**Why.** At 3x the sim runs 90 steps a second, 1.5 per frame at 60 fps. The budget is a quarter of a
frame (4 ms) for the sim, and the game thread also draws, animates and runs the UI. Measured at S9 on a
slower, shared cloud core: 0.96 ms a step before the fixes, 0.66 ms after (`docs/design/sim-cost.md`),
so the target has room and 2.0 ms catches a real regression without CI noise (runs vary by 10 to 30%).
G3 (the packaged-build check on the PC) decides whether the sim moves to its own thread; this
file only says what the sim itself may cost.

**What it replaces.** Nothing. `docs/decisions/0001` measured a step at 0.8 ms with 100 pawns on
the Stage 0 test; this is the same kind of number at Stage 2 size.

**Known hitch.** The hourly plant and crop pass costs about 10 ms in one step on 1 km. It is one frame
every two real minutes at 1x, not in the average. It is a candidate to spread over a minute if G3
shows it.
