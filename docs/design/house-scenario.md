# Two-storey house scenario

Code: `Tools/simrun/HouseScenario.{h,cpp}` (in `game/Plugins/ColonySim/`), run with
`simrun --scenario house [--seed N] [--size 512] [--days 3] [--png house.png] [--verbose]`. CTest:
`house_scenario` (seed 42) and `house_scenario_seed7`, about 12 seconds each; runs in CI on Linux and
Windows. Task M4. Builds on [levels](levels.md), [roofs](roofs.md) and the [colony scenario](colony-scenario.md).

The check that two storeys work with everything else the colony does: building, sleeping, eating and
hauling across a staircase, for three game days.

## The script

The Crashlanded start (six colonists, 50 survival meals) on a 512 m map. The house goes where the ground
near the landing is flat and has the fewest trees. The script never touches a pawn. It adds supplies
beside the house (Crashlanded's steel and logs do not cover two storeys: 600 more steel, 300 logs and 40
survival meals), then plans each part once the one before it stands (checked every game minute):

| When | What is planned |
| --- | --- |
| Step 0 | Ground ring of 27 steel walls and a wood door (9 x 7 cells, door in the south wall), stairs of steel inside at x = 1 climbing north |
| Ring and door stand | `UpperFloor` (wood) on all 63 cells of the footprint, walls included |
| Floor all laid | The 4 corner walls upstairs, 6 steel beds upstairs, a stockpile on each level (ground: only stone blocks; upstairs: only cloth), set in the same step so nothing is hauled to them by mistake |
| Corners stand | The 24 edge walls upstairs (a corner wall is built from the two cells beside it, so the edges wait: the outside of the upper ring is open air, and a corner shut in by walls cannot be reached) |
| Beds stand | `AssignBed`, one each |
| House done | Every 4 game hours until 10 hours before the end: 20 cloth on the ground outside the door (to go up) and 20 stone blocks on the upper floor (to come down) |

## The checks

The run prints one JSON line (`{"house":{...}}`: what stands, when each part was done, who slept where,
how many steps hauling spent climbing each way, where the cloth and blocks ended up) and exits 1 with a
"check failed" line for each of these that does not hold:

- six colonists; nobody died, starved (malnutrition) or stood stuck for a game hour;
- everything planned is built, no site left, and no command or job failed;
- the upstairs is an enclosed, roofed room (not outdoors) with six beds, and so is the ground floor;
- every colonist woke from a bed upstairs, and nobody woke in a bed downstairs;
- hauling climbed both ways: steps spent on the stairs on a haul job, up and down, both above zero; colonists also climbed to build;
- every dropped cloth is in the upstairs stockpile and every dropped block in the ground one.

## Pictures

`--png house.png` writes `house-level0.png` and `house-level1.png` at the end of the run: the house seen
from above on each level, 14 pixels a cell, north up. Walls dark grey, the door brown, stairs orange,
beds blue, items yellow, colonists red, plans paler, stockpiles a pale-blue dot in the cell, upper floor
tan, open air dark. (`simrun --png map.png` for the whole map still draws the ground only.)

## What it found

The first run found that an upper corner wall is unreachable once its two neighbouring walls stand:
colonists can only stand on a floor, and outside the upper ring there is none. The script now builds the
corners first. A player's colonists do the same only if told to, so U17's tools may want to plan corners
first, or a corner should be buildable from a diagonal.

Update: `Construction::workCell` now builds an upper wall with no standable neighbour on its own level from the level below, so corners can be planned in any order; the script keeps building them first.
