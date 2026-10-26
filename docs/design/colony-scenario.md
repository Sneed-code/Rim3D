# Colony scenario

Code: `Tools/simrun/ColonyScenario.{h,cpp}` (in `game/Plugins/ColonySim/`), run with
`simrun --scenario colony [--seed N] [--size 512] [--days 5] [--verbose]`. CTest: `colony_scenario`
(seed 42) and `colony_scenario_seed7`, about 6 seconds each; runs in CI on Linux and Windows.
Task C10.

The one test that runs everything the colony does so far together, the way a player's first
days go. It is not a unit test of anything: it is the check that the systems still work side by
side (jobs, movement, construction, rooms, mining, plants, bills, needs, mood).

## The script

The Crashlanded start (protagonist and 5 colonists, 50 survival meals, steel and wood nearby) on a
512 m map. All commands go in at step 0 except two that need something built first:

| What | Commands |
| --- | --- |
| A room 9 x 7 cells, at the spot near the landing with the fewest trees | 27 steel walls, a wood door in the south wall, 6 steel beds, a wood table with 3 wood stools, a stove, a wood horseshoes pin (C18) |
| Mining, chopping, picking | `Designate` on the nearest 12 reachable rock cells, 12 mature trees and 24 mature berry bushes |
| Cooking | once the stove stands (checked every game hour): `AddBill` "simple meal, until 8 on the map" |
| Beds | once all six stand: `AssignBed`, one each |

C17 adds a wood floor over the room's inside (placed by command at step 0). Colonists then clean the
dirt they walk in. The summary prints `cleaned`, `filthLeft`, `roomBeauty` and `roomImpressiveness`;
the run fails if more than 12 layers of filth are left or the room has no stats.

C20 adds social life: the summary prints `talks`, `fights`, `friendPairs` and `rivalPairs`, and the run fails if the six never talked.

Everything else is built by the colonists themselves from the starting steel and wood. The script
never touches a pawn or an item.

## The checks

The run prints one JSON line (`{"colony":{...}}`: lowest food and mood, longest stuck time, marks
done, what was built, meals made and eaten, jobs failed) and exits 1 with a "check failed" line on
stderr for each of these that does not hold:

- Six colonists; nobody died; nobody starved (no malnutrition, food never 0).
- **Nobody stuck over a game hour.** Once a game minute each colonist with a job (not Wait, not
  asleep) gets a fingerprint of its cell, job, job step, job progress, the thing it works on and,
  for building, the frame's work done. The same fingerprint 60 samples in a row is stuck.
- **All marks done**: no mining, chopping or picking designation left.
- Everything planned is built (walls, door, 6 beds, table, 3 stools, stove) and the room is one
  enclosed, roofed room holding them.
- The stove made meals and colonists ate them; at least one colonist woke up in a bed.
- Colonists started recreation jobs, at least one at the horseshoes pin (C18: Joy hours 19:00 to 21:59).
  Standing or lying still while playing doesn't count as stuck.
- No command and no job failed (a failed job is a bug here: nothing in the script can legitimately
  fail one).

It also reports, without failing on it, the lowest recreation, comfort and outdoors needs, the recreation jobs and how many were at the pin, the longest idle spell (about 10 hours at night once the
work is done) and the mood minimum.

A typical seed 42 run: everything built by game hour 7 of day 1, marks done by hour 11, 22 meals
cooked and 54 eaten (29 at the table) in 5 days, lowest food 0.12, lowest mood 38, worst stuck
11 minutes.

## What it found

The first run failed three jobs and later runs on other seeds more. All fixed in C10:

1. **A wall finished under a walking pawn** moved the pawn and cleared its route, and the job took
   the empty route for arriving ("the item isn't where the pawn stands"). Now `gaveUp` is set
   (`construction.md`).
2. **Pawns were shoved out of their working cell** by others walking past, failing "the pawn left
   its place beside the frame". Now `Mover::hold` (`movement.md`).
3. **Two pawns slid along a wall for 65 game hours**, never giving up, because sliding sideways
   counted as not being held up. Starving followed. Now only progress along the wanted direction
   counts (`movement.md`).
4. **Steel lying where a wall went up was destroyed and made again**, so a pawn on its way to it
   failed with "the material is gone". Now the same stack is moved, and a pawn whose item moved
   walks to the new place (`construction.md`).

Left alone, on purpose: a pawn's route is not checked when a wall goes up across it; it walks into
the wall, gives up after 300 steps and asks for a new path (5 game minutes lost).
If that shows up in play, the fix is to mark routes stale when `Regions` changes.

## Adding to it

Later systems that colonists use (farming, hauling to stockpiles, research, a doctor) should get a
line in the script and a check, so this stays the "everything together" test. Keep it under
10 seconds. A failure that is not worth fixing in the thread goes in `BACKLOG.md` with the seed.
