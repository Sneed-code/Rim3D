# Construction

Code: `ColonySim/Construction.h`, `Private/Construction.cpp`, the three scanners at the end of
`Private/WorkGivers.cpp`, the construction steps in `Private/JobSystem.cpp`. Defs:
`data/defs/Things/Buildings.json`, `ConstructionRules` in `data/defs/Rules/Rules.json`. Tests:
`Tests/ConstructionTests.cpp`, the replay `Tests/replays/colony-build.json`. Task C4.

Building works as in RimWorld: the player plans a **blueprint**, colonists carry the materials to it
(it becomes a **frame** with the first delivery), and once every material is there a colonist works
on the frame until the **building** stands. A building can be marked and taken down again.

## The player's commands

| Command | Does | Fails when |
| --- | --- | --- |
| `PlaceBlueprintCommand{def, stuff, cell, rot}` | a blueprint of a Things building made of `stuff` ("" for a building that takes none), its footprint starting at `cell` and facing `rot` | unknown def or stuff; `def` isn't something that can be built; the stuff doesn't fit (the wall wants steel, wood or stone; the stove wants none); a cell is off the map, water or rock, under a building or under another plan |
| `CancelBuildCommand{thing}` | a blueprint or frame goes away and **everything delivered is put back** on the ground; a building marked for deconstruction is unmarked | `thing` is none of those |
| `DeconstructCommand{thing}` | marks a finished building to be taken down | not a building; marked already |

A plan is a thing: an entity with `Thing` (its `def` is the building it will become, category
`Building`), a `Position` (the middle of its footprint) and a `ConstructionSite`. It is **not on the
map's building layer**: no `Footprint`, so it blocks nothing, regions and cover don't see it, and
`Map::buildingAt` is empty. `Construction::siteAt(cell)` finds the plan on a cell. Placing a plan
never needs pawns or materials to exist; it simply waits. The snapshot shows plans as ordinary
buildings with `stage` `Blueprint` or `Frame`, `buildProgress` (0 to 1), `materialsHave` of
`materialsNeeded` and `stuff`; a finished building has `stage` `Built` and `markedForDeconstruct`.

## What a building costs

Defs use RimWorld's fields:

- `costs`: `{"Steel": 80}`, a fixed list (the stove).
- `stuffCategories` and `costStuffCount`: the building is made of one *stuff* from a category, and
  needs that many of it (wall 5, door 25, bed 45). An item is a stuff if its `stuffProps.categories`
  names one of the building's categories (steel `Metallic`, wood `Woody`; stone arrives with C3).
- `stats.workToBuild` in game seconds at speed 1 (RimWorld's ticks x 1.44), times the stuff's
  `stuffProps.statFactors.workToBuild` (wood 0.7). `stats.maxHitPoints` times the stuff's
  `maxHitPoints` factor (a wood wall has 300 x 0.65 = 195).
- `size` `[x, z]`, `passability` (`Impassable` walls block, `Standable` things like beds and stools
  can be walked onto and store items), `fillPercent` (cover), `thingClass` `Building_Door`.
- `building`: `bed` / `sleepingSlots`, `isTable`, `isSittable`, which C6 (needs and schedules) reads;
  `stats.bedRestEffectiveness`, `stats.comfort`.

| Building | Size | Cost | Work (ticks) | Hit points |
| --- | --- | --- | --- | --- |
| `Wall` | 1x1 | 5 stuff | 135 | 300 |
| `Door` | 1x1 | 25 stuff | 850 | 160 |
| `Bed` | 1x2 | 45 stuff | 800 | 140 |
| `Stool` | 1x1 | 25 stuff | 450 | 75 |
| `Table1x2`, `Table2x2`, `Table2x4` | 1x2, 2x2, 2x4 | 28, 50, 95 stuff | 750, 1,500, 3,000 | 75, 100, 150 |
| `FueledStove` | 3x1 | 80 steel | 2,000 | 180 |

(From the RimWorld wiki; the numbers in the data are in game seconds. Fill percent of the door,
tables and stove is recalled, listed under `unverified`. The dining chair's wiki page gave
implausible numbers, so the stool stands in for it. Sandbags are not written yet.)

Stairs, a ladder and the **upper floor** (M2) are built the same way; an upper floor's frame lays a
floor instead of becoming a building, and a plan over open air is also worked from the level below.
Their numbers and rules are in [levels.md](levels.md). Delivering, building and taking down look
across levels for their target.

## The jobs

All three go to the **Construction** work type, whose tasks name the scanners
(`ConstructFinishFrames`, `ConstructDeliverResources`, `Deconstruct`). Pawns that can't do
`Constructing` work (a backstory) can't do any of this. Within a priority the nearest target wins,
whichever work type it belongs to, as for every job (jobs.md).

**Where a pawn stands.** Pawns work from a cell **beside** the footprint, sharing a side, never on it
and never on a cell that another plan covers (`Construction::workCell`). The scanner finds the one
nearest the pawn that it can reach, and the job system uses it for the reachability check
(`WorkCandidate::reach`). A plan walled in so that no side cell can be reached is left alone.

**`DeliverResources`** (steps: walk to the item, pick up, walk to the standing cell, deliver). The
scanner offers every blueprint and frame that still needs something; the job takes the first
missing material for which there is a **free, reachable stack** (the nearest, not held by another
pawn), and carries `min(missing, carrying capacity 75, stack)` of it. The pawn **holds the site for
delivery** while it goes, so two pawns never bring materials to one plan at once, and the stack by
the usual item reservation. At the end `Construction::deliver` adds to the site; the first
delivery turns a blueprint into a frame and sends `FrameStarted`. What the site didn't need stays in
the pawn's hands and is put down when the job ends. A stove (80 steel, capacity 75) takes two trips.

**`BuildFrame`** (walk to the standing cell, work). Only for a frame with all its materials. One
builder per frame: the frame is reserved. Each game second adds
`construction speed x buildWorkPerSecond (1.7)` to the frame's `workDone`; at `workTotal` the frame
is replaced by the building (`Construction::complete`) with full hit points, and the pawn's
`PawnAction::Build` was sent when it began. `workDone` stays with the frame, so another pawn carries
on where one stopped.

**`Deconstruct`** (walk, work) takes `clamp(workToBuild, 28.8, 4320)` seconds of work at speed x 1
(`deconstructWorkPerSecond`), kept in the job (`progress`; an interrupted job starts over). Then
`Construction::deconstruct` gives back **half** the costs, each rounded at random (5 steel give 2 or
3; it uses the sim's random stream), lying on the nearest free ground, and the building is gone.
A building made through `SpawnThingCommand` has no stuff (`BuiltOf`), so only the fixed part of
its costs comes back.

A job fails (and the pawn remembers the plan for 10 minutes, like any failed target) when its target
is gone, cancelled or unmarked, the material is taken, the pawn is moved off its standing cell, or
the building can no longer stand where it is (then the delivered materials are put back).

## Construction speed

`construction speed = skillSpeedBase 0.3 + skillSpeedPerLevel 0.0875 x Construction level`
(1.0 at level 8, 2.05 at 20), times the pawn's **Manipulation** capacity (a hurt hand builds
slower). A pawn with no skills (a test pawn) counts as level 8 (`defaultSkillLevel`, ours). All of
these are in `ConstructionRules`. A level 8 colonist builds a steel wall in 194.4 / 1.7 = 114 game
seconds, 1.9 game minutes, a bit under 4 real seconds at 1x.

## When a building appears

`Construction::complete` first moves out of the footprint what shouldn't be inside: **pawns** standing
in the cells of a building that blocks movement are put on the nearest free ground (their path is
cleared and `Mover::gaveUp` is set if they were walking, so the job asks for a new path from there),
and **items** lying under a building that isn't standable are moved, as the same stack (same entity, so
a pawn on its way to it, or a job holding it, still has it), to the nearest empty cell outside; with no
empty cell near they are made again by `dropNear`. A pawn walking to a stack that moved walks to where
it is now (the pick-up step repeats the walk, up to the job's repath limit). Then the frame is destroyed and the building spawned, which tells the
map layer, the regions (one rebuild for the touched blocks) and the cover. If the cells are no longer
free (a building appeared there by command) the materials are given back and the job fails.

**Plants** under the footprint are removed from the plant layer when the building appears
(`sim.plants().remove`, [plants.md](plants.md)). A blueprint or a frame leaves them alone, and
trees don't stop a plan yet (they don't block pawns either until C3).

## Saves and hashes

Save format 10. A plan is a thing with an optional `site` (stage, stuff, origin, rotation, size,
materials with `needed` and `have` by item `defName`, `workTotal`, `workDone`); a building made of
something has `builtOf`; a marked building has `marked`; a job has its `site` and `progress`. The cell
index (`siteAt`) is rebuilt on loading and the delivery reservations follow from the jobs like the
others. All of it is in `Sim::stateHash`. Saves of formats 6 and 7 still load.

## Not yet

- Failed construction. RimWorld lets low skill ruin a build (`ConstructSuccessChance`); we have no
  failures, and builders don't gain Construction XP yet (no skill learning in the sim at all).
- Repair, replacing a building with a better stuff, minifying and moving furniture. Roofs and rooms are not
  built: they follow from the walls ([rooms.md](rooms.md)).
- Walls over water, terrain affordances and floors; blueprint rules like "doors only between walls".
- Materials come from any free stack on the map, as RimWorld's do; forbidden items, "build from
  stockpile only" and small-volume stuffs (x10 counts) are not here.
- Frames and blueprints have no hit points and can't burn or be shot.
- A cancelled plan with a pawn mid-delivery makes that job fail once (the pawn puts the material
  down); nothing is lost.
