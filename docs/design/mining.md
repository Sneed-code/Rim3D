# Mining, chopping and harvesting (task C3)

Colonists dig out rock and ore, fell trees and pick berries on the player's marks. The numbers are
the RimWorld wiki's; the ones it doesn't give are listed in `WorkRules.unverified` and `ours`
(`data/defs/Rules/Rules.json`).

## The rock layer

`sim.mineables()` (`Mineables.h`) holds, for every cell of `Terrain::Rock`, which rock or ore it
is and its hit points left. Flat arrays like the plant layer (a 1 km map has hundreds of thousands
of rock cells) with a running hash that `Sim::stateHash` includes.

- Defs are `Things` with category `Mineable` (`data/defs/Things/Rocks.json`), fields
  `mineable.{hitPoints, dropDef, dropCount, dropChance, isOre, commonality, lumpSize}`.
  Rocks: granite 900 hit points, limestone 700, sandstone 400, slate 500, marble 450, each with a
  25% chance to leave a chunk. Ore: steel 1500 (40 steel), silver 1500 (40 silver), components
  2000 (2 components), in lumps of 3 to 6.
- A new map gets 2 or 3 rock types (RimWorld: 2 to 3, chosen from the seed) in patches of 96 m
  value noise, and ore lumps grown inside the rock (6 per 10,000 rock cells, as the wiki says).
  Rock cells keep the terrain `Rock`; fertility under rock stays 0.
- **Mined out** (hit points 0): the layer forgets the cell and `Map::setTerrain(c, Terrain::RoughStone)`
  marks regions and cover dirty. Rough stone walks like soil at path cost 2. Regions rebuild on
  their next update, so a tunnel opens a route the same second.
- Whether a rock leaves a chunk is a hash of the map seed and the cell, not the random stream, so
  it doesn't depend on who mines what first.

## Designations

`sim.designations()` is one mark per cell: `Mine`, `CutPlant` or `HarvestPlant`. Commands:

- `DesignateCommand{area, kind}` marks every suitable cell in the rectangle (rock for Mine, any
  plant for CutPlant, a plant that gives something for HarvestPlant). It fails with a reason if
  nothing in the area suits.
- `CancelDesignationsCommand{area, kind}` takes marks off (`None` means every kind) and ends the
  jobs working on those cells.

Marks go when the work is done, and are pruned each hour if their rock or plant is gone. The event
`DesignationsChanged` tells Unreal to redraw the marks; the snapshot lists them (`designations`).

## The jobs

Three job types (`Mine`, `CutPlant`, `HarvestPlant`), two scanners (`MineGiver` in the Mining work
type; `CutOrHarvestPlantsGiver` in Plant cutting) and steps `GotoTouch` then `MineWork` or
`PlantWork`.

- The target is a cell, reserved by the pawn like a thing. Rock can't be walked into, so the pawn
  works from a walkable cell beside it (`touchCell`, the nearest cardinal neighbour it can reach);
  the scanner skips rock nobody can reach.
- A pawn with no working hands (Manipulation 0) is not offered the work.
- **Mining**: the pick falls every 144 game seconds at 100% speed (RimWorld's 100 ticks), 80 damage a
  hit. Speed is `0.04 + 0.12 x Mining level`, times Manipulation. Damage stays on the rock if the
  pawn leaves; the time towards the next hit does not. When the rock goes it drops its chunk or ore
  at the cell, scaled by the miner's yield (60% at level 0, 100% at 8, 113% at 20, always at least
  1) and the designation clears.
- **Plants**: work piles up at `0.08 + 0.115 x Plants level` (at least 0.1), times Manipulation,
  until the plant's `harvestWork`. Cutting a tree gives its wood, scaled by growth (50% of the yield
  at 65% grown, full at 100%; a younger tree gives nothing) and by the same yield curve; cutting
  anything else gives nothing. Picking from a bush sets it back to `harvestAfterGrowth` (30%); a harvest mark
  waits until the plant is 65% grown (`plant.harvestMinGrowth`, unverified).
- XP: Mining 0.0486 a second and Plants 0.0590 a second, scaled by passion; the daily cap and the decay
  of high skills are C12's ([skills.md](skills.md)).
- Needs, a downed pawn or direct control end a gather job like any other (C6's interruption
  covers it). A cancelled mark ends the job with "the order was cancelled".
- Events: `PawnAction` `Mine`/`Cut`/`Harvest` (for the animation), `CellMined`, `PlantRemoved`
  ("felled" or "cut"), `PlantHarvested`.

## Saves

Format 14: the rock layer (a record per cell: def and hit points) and the designations. Cells that
are `Terrain::Rock` in the generated ground but have no record were mined, so the load turns them
back into rough stone. Older saves load with the generated rock layer. The job's `progress` holds
the half-done pick. Commands 20 and 21. See `saves.md`.

## Not yet

- Mining away roofs and the collapse of unsupported ones (C5 adds roofs).
- Deep ore is in [deep-mining.md](deep-mining.md). Stone blocks are cut at the stonecutter's table from chunks (crafting.md).
- Pawns choosing a cheaper cell to mine from; the nearest reachable cell is used.
- Hauling the dropped chunks happens through the normal hauling job once a stockpile exists.
