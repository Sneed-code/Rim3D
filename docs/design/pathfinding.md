# Pathfinding

Code: `ColonySim/Pathfinder.h`, `Private/Pathfinder.cpp`. Tests: `Tests/PathfinderTests.cpp`, the
hauling tests and the `simrun_paths` CI run. Task S3. Builds on [regions](regions.md).

## How a path is found

Stage 0 measured plain cell A\* across a 1 km map at 10 to 100 ms for one search. Now a path is
found in three passes, all on one level:

1. **Refuse the impossible.** If the start and goal are in different reachability groups (or off
   the map, or on different levels), the answer is "no path" at once, with no search.
2. **Route.** A\* over the region graph from the start's region to the goal's region. A region is
   entered at the middle of the border it was reached through (`RegionLink::x/y`), and each hop
   costs the octile distance between those points.
3. **Guide.** The **corridor** is the route's regions plus every region linked to one of them.
   A Dijkstra walk back from the goal over the corridor gives each corridor region the point it is
   left through towards the goal and the cost from there on.
4. **Cells.** A\* over the 8-connected cells, no corner cutting, using only corridor cells. Its
   heuristic for a cell is the walk to its region's exit point plus the guide's cost from there,
   so the search follows the corridor instead of filling it.

The extra ring of regions lets a path cut across a neighbouring region where the route's border
middles zig-zag. The guide heuristic can overestimate a little (a border is wider than its middle
point), so paths are not always the shortest:

| 1 km map, random reachable pairs (cloud) | Plain cell A\* | Hierarchical |
| --- | --- | --- |
| Long paths: work per search | 34,000 cells, 11 ms | 1,500 regions and cells, 0.5 ms |
| Long paths: length vs the best | 1.00 | 1.02 on average, worst 1.12 |
| Paths under 100 m: length vs the best | 1.00 | 1.05 on average, worst 1.36 |
| After straightening (S4's `smoothPath`) | | 1.005 on average, worst 1.10 |

The extra length is mostly zig-zag in open ground, which smoothing (S4) removes. A blend of the
guide with the straight-line distance gave slightly shorter cell paths for six times the work, so
it was not used. `findPathFullGrid` keeps plain cell A\* for tests to compare against.

## The queue and the budget

Game code never searches directly. It asks the queue and checks back:

```
PathTicket t = sim.pathfinder().request(from, to);  // Failed at once if unreachable
...
if (pathfinder.status(t) != PathStatus::Queued) {
    bool found = pathfinder.takePath(t, mover.path);  // frees the ticket
}
pathfinder.cancel(t);  // when the pawn no longer wants it
```

`Sim::step` calls `Pathfinder::update(budget)` once, after the pawns have acted, so a path asked
for in one step is ready in the next at the earliest. `update` runs requests in the order they
came, until it has done `budget` work: one unit per region or cell expanded, never time
(architecture rule: budgets in work, so the game stays deterministic). A search that runs out of
budget stops and goes on in the next step from where it was. Every ticket must be taken or
cancelled, or its slot stays in use.

If the map's regions change while a search is half done (a wall built or removed anywhere;
`Regions::changeCount`), that search starts again, since its region ids are stale. If the change
cut the goal off, it fails without a search. A path handed out earlier is not checked again; a
pawn whose path gets blocked has to ask again (the job system, C1, will do that).

**Determinism, replays and saves.** The queue is sim state. `Pathfinder::hashState` adds the
requests, the queue and the work done so far on the search in hand to `Sim::stateHash`; replays
store `pathBudget` with the other settings; saves (format 2) store the requests, the queue and that
work count (`saveState` / `loadState`). A half-done search is fully decided by its request, the map
and the work done, so loading starts it again and runs it for that much work. A loaded map numbers
its regions afresh, so no search lets a region id decide anything: heap ties go to the region with
the lower anchor cell, never the lower id.

**Budget:** `Pathfinder::kDefaultBudget` = 3,000 units, `SimSettings::pathBudget` to change it. In
the cloud 3,000 units take 1 to 1.5 ms; sneed's PC should be faster. A long 1 km search is about
1,500 units, so a step finishes one or two long searches or dozens of short ones. When 100 pawns
all ask at once, the last waits about 50 steps (under 2 seconds at 1x).

## The 1 km check

```
simrun --seed 42 --size 1024 --days 1 --pawns 100 --items 2000 --data game/Plugins/ColonySim/Tests/data --check
```

The test fixture defs in `Tests/data` (`Human`, `Steel`) keep the check independent of the game data; `data/defs` has them too since D3.
`--check` fails the run if any job or path search failed; CI runs it as `simrun_paths`. Result in
the cloud, seeds 42 and 7: no failed jobs, no failed searches. Taking the fastest of three runs
for each step (the cloud machine is shared, and single runs show random spikes of 4 to 7 ms on
steps with little work), the slowest step is step 0 at 2.3 ms (all 100 pawns choose an item and
the first searches run), and every other step is under 1.6 ms. Regions are built when the sim is
made, not in the first step (about 30 ms on a 1 km map).

simrun prints a second line of path numbers: requests, refused, searches, failed searches,
restarts, regions and cells expanded, the most work in one step, the longest queue and the longest
wait in steps.

## Not done here

- **Costs.** Every walkable cell costs the same, except a door cell (+3.5 m, `DoorRules.pathCost`, in the region
  route and the cell search; [rooms.md](rooms.md)). Known danger goes into the cell cost and
  the region hop cost later (the raid tasks for danger). Terrain speed and slope slow the walk
  (S4) but do not shape the route yet; [movement.md](movement.md) says why.
- **Smoothing and local avoidance** are S4: [movement.md](movement.md). The pawn takes a path
  with `takePath`, then `smoothPath` and `Mover::walkTo` (`Sim.cpp` does it for the hauling test).
- **Stairs** (M2): stairs and ladders link regions on two levels, and the cell search steps over them at
  their climb cost; see [levels.md](levels.md).
- **Shared flow fields** for many raiders heading to one place come with the raid (F6).
- **Per-faction doors**: everyone walks through every door (see regions.md).
- **Speed per cell**: about 2,500 to 4,000 nodes per ms in the cloud. If a profile shows the
  budget is too small, a faster heap and cell loop come before a bigger budget.
