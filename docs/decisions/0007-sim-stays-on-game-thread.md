# 0007: The sim stays on the game thread

_Decided 2026-10-27 in task G3 (Claude). Easy to change: nothing was built for it either way._

**What.** The sim keeps running on Unreal's game thread, stepped by `USimSubsystem` as now. It does not move
to its own thread at Stage 2. Revisit when, in the packaged `perf` test ([perf-check.md](../design/perf-check.md)),
the sim costs more than **4 ms a frame at 3x** (decision 0006's budget) or the game thread passes **12 ms**
(under 80 fps with room for a slower PC).

**Why.** Measured in a packaged build at 2560 x 1440 on the Stage 2 sized colony (1 km map, 30 colonists, about
580 animals, rain, a fire, a 40-pirate raid at 3x): 133 to 219 fps, a 4 to 6 ms game thread, and the sim
about 1 ms of it per frame (1.1 to 1.4 ms a step). A sim thread would save at most that millisecond, and it
costs a lot: every system that reads the sim from Unreal (picking, panels, direct control, the smoke tests'
direct changes) would need locking or copies, and replays must stay exact. The snapshot double buffer in
`USimSubsystem` already keeps that door open if the numbers change.

**Also decided.** The hourly plant pass, a 14 ms step once a game hour on 1 km (22 to 27 ms frames), is spread
over the next 60 steps (`Plants::growHourSlice`, [sim-cost.md](../design/sim-cost.md)); its frames now cost
3 to 5 ms. Performance is judged on packaged builds: the editor's `-game` mode ran the same scene at 57 to 72
fps with a 13 to 17 ms game thread.

**What it replaces.** Nothing; `0006` set the sim's own cost budget and left this question to G3.
