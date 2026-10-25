# Smooth movement

Code: `ColonySim/Movement.h`, `Private/Movement.cpp` (ground speed, straight lines),
`Private/MovementSystem.cpp` (the per-step walk and the steering). Tests: `Tests/MovementTests.cpp`.
Task S4. Builds on [pathfinding](pathfinding.md); the job system (C1) will give pawns their walks.

A pawn is a point that moves over the ground in metres, not a hop from cell to cell. The pathfinder
still thinks in cells; this system turns its answer into a walk that looks and costs right.

## From a path to a walk

1. **The pathfinder's cell path** is taken (`takePath`).
2. **Smoothing** (`smoothPath`) drops every cell the pawn can skip by walking straight to a later
   one. A line is allowed when a pawn 0.35 m wide fits along it (tested every 0.25 m, 0.05 m wider
   than the pawn's real 0.3 m radius, because a line can slip past a corner between two tests) and
   when it is no more effort than the cells it replaces (see below). Result: a subset of the
   path's cells, usually a handful for a 1 km walk (the test map: under a third of the cells). The
   look-ahead doubles until a cell is out of reach, then settles between; every cell kept was tested.
3. **`Mover::walkTo`** gives the pawn the waypoints and remembers where the first line starts.
4. **`MovementSystem::step`**, once per step after the jobs, moves every pawn.

`Mover::arrived()` is true when the waypoints are used up. If a pawn is held up for 300 steps in a row
it drops its walk and sets `Mover::gaveUp`; the job asks for a new path from where it stands (the
hauling placeholder does, and counts `SimStats::repaths`). `simrun --check` fails if more than about
1% of hauls needed that.

## How fast

A pawn's `moveSpeed` is RimWorld's `MoveSpeed` stat (4.6 cells a second for a human). RimWorld slows
that by the ground's path cost: `1 / (1 + cost * 0.077)`. Soil is cost 2 and sand cost 4, which
give the wiki's 87% and 76% (the wiki lists speed factors, so the costs are worked back from them).
So a human walks 4.0 m/s on soil, as the design notes say. They move into the Terrain defs when
terrain is loaded from data.

Slope is not in RimWorld (it has no height). It uses Tobler's hiking function, scaled to 1 on the
flat: a gentle downhill (5%) is 19% faster, 10% uphill is 70%, 20% uphill is 50%, and it levels out
at 17% from 50% either way. The values are a written-out table, because `exp()` can differ between
compilers and the sim must give the same game on every machine. The slope is the rise over the metre
around the pawn, along the way it is heading; `Map::heightAt` gives the ground height at any point
by interpolating the four corners around it (the same corners the terrain mesh is drawn from, so
pawns stand on what the player sees).

**Effort** (`walkEffort`) is the metres of a line divided by the speed factor along it. Smoothing
only takes a shortcut when its effort is no more than the cells it replaces (with 0.1% slack), so
a pawn does not cut across sand or up a hill to save a few metres.

## Steering

Each pawn decides from the positions and velocities everyone had at the start of the step, and
all the new ones are written together at the end. Nobody sees a half-updated neighbour, so the order
pawns are visited in can't change the game. Neighbours are always taken nearest first, ties to the
lower entity id; pawns are bucketed in 4 m squares to find them.

For one pawn:

1. **Wanted velocity.** It walks the line from where the last waypoint was reached to the next
   waypoint, aiming 1.5 m ahead of the closest point on that line. So a pawn pushed off the line
   by others comes back to it, and the line was checked for room. Speed is the ground and slope
   speed, lowered to brake into the final goal, and for a turn at a waypoint (35% of its speed
   through a right angle, more for a gentler one). A waypoint is reached 0.2 m away (a cell centre
   is 0.5 m from the walls and a pawn needs 0.3); the goal 0.25 m away, or sooner when the pawn
   is inside the goal cell and others have held it back for 10 steps (a crowd at one stockpile).
2. **Pushing apart.** Pawns closer than 0.7 m push each other away, so a pile (100 pawns spawned on
   one cell) spreads out. Two pawns on exactly the same spot go different ways, picked from their
   ids.
3. **Candidates.** The wanted velocity turned by up to 180° in 22.5° steps at full and half speed,
   its two axis parts (for sliding along a wall), and standing still: 29 in all. Each is limited by
   the pawn's acceleration (20% of its speed per step), so a pawn can't turn on the spot.
4. **Cost of a candidate.** How far it is from the wanted velocity, plus: a big penalty if the pawn
   would not fit in the ground cells next step *or where it would stop if it braked*, so it never
   builds up speed it can't lose before a wall; for each of the 8 nearest pawns, the chance of
   meeting them within 30 steps (looking at their current velocity) and the room left after the
   step (it likes 0.35 m to spare); and a small bias to turn right, so two pawns walking at each
   other both pass on the same side. The cheapest wins; ties go to the first tried.
5. **Held up.** If a pawn wants to walk but gets under 30% of its wanted speed *along the way it
   wants to go* for 45 steps, it walks *through* the other pawns for 30 steps (the way out for two
   pawns meeting in a one-cell corridor). Walls are never walked through. After 300 held-up steps in
   a row it gives up (above). Sliding sideways along a wall does not count as progress: two pawns
   pushing each other along a wall they could not pass once slid for 65 game hours (found by C10).
6. **Holding still.** The job system sets `Mover::hold` every step for a pawn at a job step it does
   standing still (building, deconstructing, mining, cutting, cooking, eating, sleeping). A held pawn
   stays exactly where it is, nobody pushes it, and others steer round it. Without it a pawn walking
   past shoved a builder out of its working cell and the job failed ("left its place"). `hold` is not
   saved: the job system sets it again before the movement system reads it.

A wall that is finished under a walking pawn moves the pawn out and clears its route; that now also
sets `gaveUp`, so its job asks for a new path instead of taking the empty route for arriving.

All of it is plain float arithmetic (`+ - * /`, `sqrt`, `floor`) and written-out tables: no `sin`,
`cos`, `exp` or `pow`, so Linux, Windows and the cloud agree to the bit. Unreal builds that
use fused multiply-add instructions (`/arch:AVX2`) may not; a replay made in the editor is checked
by the same build, which is what matters.

## What Unreal sees

`ThingView` has `z` (ground height under the thing), `vx`, `vy` (metres per step: times 30 for metres
per real second at 1x) and, for pawns, the route still ahead as `waypointCount` points from
`Snapshot::waypoints[firstWaypoint]`, nearest first. U5 feeds these to motion matching as the
trajectory.

## Cost, measured

100 pawns hauling on a 1 km map (the `simrun_paths` check, seed 42, in the cloud): the average step
went from 0.04 ms to 0.09 ms, 99% of steps under 1 ms, the slowest 3 to 6 ms (the same cloud
spikes as before, see pathfinding.md). Steering is a fixed 29 candidates against at most 8 neighbours
per pawn, so it grows with the number of pawns, not with how crowded they are; away from walls it
skips the wall tests. A day's hauling went from 1,637 to 1,110 items: pawns walk 13% slower on soil
and queue round each other at the stockpile.

## Not done here

- **Slopes and sand in the path search.** The pathfinder still treats every walkable cell as the same
  cost, so paths don't avoid hills or sand; pawns only walk them slower. It was tried (a cell costs
  `1 / speed factor`) and made the corridor search 15 times bigger on the test map, because the
  region route and its guide know nothing of slopes. It needs a per-region average cost in the
  region graph first; do that with the first task that wants it (a ridge or marsh in the way).
- **Stairs and levels**: pawns stay on their level (the movement ignores neighbours on other levels).
- **Running, sneaking, crawling**: one speed per pawn, from the def. Injury, load and
  apparel slow-downs come with the health and inventory tasks, as a multiplier on `metresPerStep`.
- **Animals and remnant machines** use the same system; bigger bodies need a radius per def (`kPawnRadius`
  is one number for now).
- **Doors** are in the movement system since C5: a pawn waits at the edge of a closed door's cell until
  it has opened (23 steps), and an open door is walked through like open ground ([rooms.md](rooms.md)).
