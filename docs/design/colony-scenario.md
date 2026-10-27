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

C14 adds a crafting spot in the room (a wood club bill, "make club once", added when the spot stands): the run fails unless exactly one club is made, and the summary prints `clubsMade` (`mealsCooked` counts food only).

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
- The work taught something: the total experience of skills under level 10 went up (`skillXpGained`).
- Colonists started recreation jobs, at least one at the horseshoes pin (C18: Joy hours 19:00 to 21:59).
  Standing or lying still while playing doesn't count as stuck.
- No command and no job failed (a failed job is a bug here: nothing in the script can legitimately
  fail one).

It also reports, without failing on it, the lowest recreation, comfort and outdoors needs, the recreation jobs and how many were at the pin, the longest idle spell (about 10 hours at night once the
work is done) and the mood minimum.
It also reports, without failing on it, the longest idle spell (about 10 hours at night once the
work is done), the mood minimum and `mentalBreaks`, the number of mental breaks (C19; a break that
makes a colonist leave fails the "six colonists" check).

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

## The year run (`--year`)

`simrun --scenario colony --year --seed N --size 512 --days 60` (60 game days is one game year; about 10 minutes
on one core) is the same script plus things the long game needs: wild animals at 100% density, a stockpile with
32 stacks of survival meals (there is no hunting yet, so food has to come from somewhere), a rice field of
14 x 10 cells on the most fertile soil within 40 cells of the landing, laid out on day 13 (sown earlier it dies
in the early spring frosts), and a meal bill of 30. It also prints the slowest step (`worstStepMs`), crop and
farming counters, deaths of everything that is not a colonist by cause, and counts filth only within 15 cells of
the room. In verbose mode it prints every injury a colonist takes. Colonists lying in `BedRest` are no longer
counted as stuck. Without `--year` nothing changes.

The year run also arms the colonist with the best Shooting skill with the landing's rifle and revolver (colonists do not
pick loose weapons up themselves), builds a butcher table outside the west wall with a forever bill for corpses within
40 m, and from day 13 keeps three deer, hares, muffalo or alpacas within 45 m of the landing marked for the hunt (a mark
is taken off an animal that runs 80 m away). In verbose mode it prints a food line every six game hours, and for each
injury what the colonist and the animals round it were doing.

**First runs (seeds 1, 7, 42, 60 days, before the fixes):** no crash, no stuck job, no determinism break, but all six
colonists starved between day 37 and day 51 once the autumn frosts killed the field; colonists sowed into the frost
(1,137 sown, 274 harvested on seed 1); wolves killed colonists (nobody fought back); animal blood piled up to 2,500 to
8,600 layers.

**After the fixes** (sow only what ripens before the frost, armed colonists stand and shoot, unarmed ones run for shelter,
outdoor filth fades, hunting; `farming.md`, `raids.md`, `fighters.md`, `room-stats.md`):

| Seed | Colonists alive on day 60 | Frost-killed crops | Hunted | What killed the rest |
| --- | --- | --- | --- | --- |
| 1 | 1 of 6 | 0 (137 sown, 137 harvested) | 8 | predators and manhunters, day 43 to 48 |
| 7 | 1 of 6 | 0 (260 sown, 260 harvested) | 6 | predator day 24, starvation day 31, predators and a bleed-out later |
| 42 | 1 of 6 | 0 (128 sown, 128 harvested) | 0 | a predator day 9, then hypothermia on days 50 to 52 (four) |

Frost no longer kills a single crop, and no seed is dead by day 51 any more (the first runs lost everyone), but the colony
still does not last the year: see `BACKLOG.md`. The run is not in CTest: it takes about ten minutes.

## The greenhouse (year run)

From day 15 the year run also builds a heated greenhouse on the nearest free 10 x 6 patch (outside the room and the
field): wood walls and a door, a wood-fired generator, a heater, two standing lamps, six hydroponics basins joined by a
conduit row, and a rice growing zone over the basins once all six stand. The script unlocks `Electricity` and
`Hydroponics` at the start (a player would research them). While the year run goes on it keeps ten of the nearest trees
marked for felling whenever the map holds under 400 wood, since the stove, the walls and the generator (63 wood a day)
otherwise empty it. Verbose mode prints the greenhouse's temperature, wood on the map and the generator's fuel every
12 game hours; the output has a `greenhouse` line (temperature range, most crops at once, crop-hours and ripe-hours).

**Seed 42, 100 days:** the room stays at 21 C through the cold (outside down to -5 C), the six basins carry rice
(6,337 crop-hours, 158 ripe-hours). The room only went cold again after the colonists were dead and nobody refuelled the
generator. The colonists still die for the reasons above (predators, starvation, hypothermia outdoors), so the greenhouse
alone does not yet keep the colony alive; only six basins feed few people.
