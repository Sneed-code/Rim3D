# Jobs, work and stockpiles

Code: `ColonySim/Jobs.h`, `ColonySim/JobType.h`, `ColonySim/Zones.h`, `Private/JobSystem.cpp`,
`Private/WorkGivers.cpp`, `Private/Zones.cpp`. Tests: `Tests/JobTests.cpp`, `Tests/HaulingTests.cpp`,
the job and zone commands in the save and replay sessions. Task C1.

Pawns decide what to do the way RimWorld's do: a **think list** picks a **job**, a job is a short list
of **steps** with **fail checks**, **work types** and **priorities** come from the data, **scanners**
offer targets, and **reservations** stop two pawns going for the same one. Hauling to **stockpile
zones** is the first work on it and replaces the Stage 0 placeholder.

## Who works

A pawn with a `Worker` (its job and a short memory) and a `WorkSettings` (its Work tab row) works.
`JobSystem::addWorker(pawn)` gives both, with priority 3 on every work type, which is RimWorld's
starting value for work a new colonist can do. The hauling test's pawns and every pawn the spawn
command makes get them (there are no factions yet, so every pawn is a colonist). A scenario's
colonists (C8) get `addWorker(pawn, colonist.disabledWorkTags)`: a work type with one of those tags
in its `workTags` is `disabled`, priority 0 for good (RimWorld's "incapable of"), and
`SetWorkPriority` refuses to turn it on.

Every step, `JobSystem::step` runs each worker on the map **in entity id order**, so a pawn with a
lower id gets first pick of a contested target. A pawn with no job thinks and starts the job it got,
and runs its first step in the same game second; a pawn with a job runs it.

A downed or dead pawn (F1, `HealthSystem::incapacitated`) doesn't think. If it had a job, the job
ends ("interrupted: downed" or "interrupted: died"), its reservations are let go and what it carried
is put down next to it, so other pawns can take over.

## The think list

The think list is RimWorld's think tree, flattened: an ordered list of nodes, each of which may give
a job; the first that does wins. It is `kThinkList` in `JobSystem::step`.

| Order | Node | From |
| --- | --- | --- |
| 1 | Emergencies: on fire, mental break (a downed pawn skips the list, above) | later |
| 2 | Defense plan (alarm roles) | F6 |
| 3 | **Urgent needs** (`thinkNeeds`: eat, sleep when worn out) | C6, [needs.md](needs.md) |
| 4 | **Schedule block** (`thinkSchedule`: sleep hours, recreation hours) | C6 |
| 5 | **Work** (`thinkWork`) | C1 |
| 6 | **Idle** (`thinkIdle`): wait 60 game seconds, then think again | C1 |

**Player orders** don't wait for a think: the command ends the pawn's job ("interrupted: ordered to
haul") and starts the ordered one at once, marked `forced`. The only order so far is
`OrderHaulCommand`, RimWorld's "prioritize hauling"; draft, move and attack orders come with F4. A
forced job ignores work priorities (it is done even with Hauling off), as in RimWorld.

**Hands-on work** (U10, [handson.md](handson.md)) is the other way a player starts a job: the controlled colonist (`PlayerControl`) is skipped by the think list, but a job with `handsOn` set that `HandsOnCommand` started is ticked for it, in place with no walking, ending when the player steps out of reach.

A pawn re-thinks only when its job ends, except that `interruptForNeeds` (C6) checks every step
whether a pawn on a hauling or waiting job (not a player order) has become hungry or tired enough
to put it down. Another node that needs to interrupt (an alarm) does the same: it ends the job with
`endJob(Interrupted)`.

## Work types and priorities

Work types are the `WorkTypes` defs (`data/defs/WorkTypes/`), in **Work tab order**: highest
`naturalPriority` first (Firefighter, Patient, Doctor... Research last), as RimWorld lays out its
Work tab. Each def's `tasks` lists our names for its scanners in order. A task with no scanner yet
gives no work; the tasks with scanners so far:

| Task | Work type | Scanner |
| --- | --- | --- |
| `HaulToStockpile` | Hauling | items outside storage, or in a stockpile worse than one that would take them |
| `ConstructDeliverResources` | Construction | blueprints and frames that need materials ([construction.md](construction.md)) |
| `ConstructFinishFrames` | Construction | frames with all their materials |
| `Deconstruct` | Construction | buildings marked for deconstruction |
| `DoBillsCookStove` | Cooking | work buildings with a bill to do ([bills.md](bills.md)) |
| `CleanFilth` | Cleaning | the nearest filthy cell indoors ([room-stats.md](room-stats.md)); job `CleanFilth`, steps GotoCell, CleanWork |

C3 adds `Mine` and `CutOrHarvestDesignatedPlants`
and so on: a class deriving from `WorkGiver` in `WorkGivers.cpp`, added in `makeWorkGivers`.

A pawn's priority per work type is 0 (doesn't do it) or 1 to 4 (1 first), as RimWorld's manual
priorities. `SetWorkPriorityCommand{pawn, workType, priority}` sets one; it fails for a pawn that
doesn't work, an unknown work type, a priority outside 0 to 4, or switching on work the pawn can
never do (`disabled`).

**Choosing work** (`thinkWork`): for priority 1, then 2, 3 and 4, every scanner of every work type at
that priority offers its targets, and they are tried **nearest first, whatever work type they
belong to**, ties going to the Work tab column, then the scanner, then the target's id. This is our
one change to RimWorld (game design doc: "within the same priority, pawns prefer closer jobs"), which
on a 1 km map saves a lot of walking. RimWorld instead finishes a column before the next one at the
same number; a player who wants that gives the columns different numbers. Distance is a straight
line from the pawn for now; walking distance can replace it when maps with long detours need it.

A target is skipped when another pawn holds it, when this pawn failed a job on it in the last 600
game seconds (its avoid memory, eight targets at most), or when it can't be reached (the regions'
reachability groups, two lookups, so unreachable targets never cost a path search). The scanner then
makes the job, or says there is none after all.

## Jobs and their steps

A `Job` is plain data: its type, a thing and a cell, a count, which step it is on and the walking
state inside a go-to step. Each job type has a fixed list of steps (RimWorld's toils):

| Job | Steps |
| --- | --- |
| `Wait` | wait until `until` |
| `HaulToCell` | go to the item, pick it up, carry it to the cell, put it down |
| `Ingest` | go to the food, pick it up, walk to the eating spot (none if it is already there), eat |
| `LayDown` | walk to the bed (none if the pawn lies on the ground where it is), lie until rested |
| `DeliverResources` | go to the materials, pick them up, carry them next to the site, hand them over |
| `BuildFrame` | walk next to the frame, build until the work is done |
| `Deconstruct` | walk next to the building, take it down |
| `DoBill` | go to an ingredient, pick it up, carry it beside the stove, hand it over (and again for the next ingredient), work, make the product ([bills.md](bills.md)) |

A step that ends at once (picking up, putting down) lets the next start in the same game second. A
go-to step asks the pathfinder for a path (an answer the next step at the earliest), walks it, and if
the pawn gives up its walk asks for a new path from where it stands, up to 5 times.

**Fail checks** run before every step. A hauling job fails when the item is gone or the stockpile
cell can no longer take it (the zone was removed, its filter changed, something else was put there).
Once the item is in its hands, a cell that can't take it any more makes the pawn look for another
stockpile cell and carry on there; only if there is none does the job fail. When a job ends it
releases its reservations, cancels a path it was waiting for, stops the pawn if the job didn't
finish, and puts down whatever the pawn carries on the nearest free cell (or a stack of the same
item with room) within 6 m. A failed job counts in `SimStats::failedJobs` and goes in the pawn's
avoid memory.

Destroying a thing (`DestroyThingCommand`) is allowed while a job uses it: `JobSystem::forget` ends
the job of the pawn carrying it, and a destroyed pawn's job ends with what it carried put down. Jobs
heading for a destroyed item fail their next check.

## Reservations

A reservation is a thing or a cell plus a *purpose*: `Deliver` for the hold a delivery job has on
its blueprint or frame, `Use` for everything else. A builder holds the frame for `Use`, a deliverer
holds it for `Deliver`, so a frame being built can still be seen by a deliverer (it needs nothing)
and two deliveries to one plan never overlap. Which targets each job type holds is
`JobSystem::reservationsOf`, used for reserving, releasing and rebuilding on load.

A hauling job holds its item and its stockpile cell; no other pawn's scanner or storage search takes
them. An eating job holds the food and, if it eats in a chair, the chair's cell; a sleeping job holds
its bed's cell (`JobSystem::heldTargets`). Reservations follow from the jobs (each job type knows what it holds), so they are not saved:
loading rebuilds them from the jobs, and refuses a save where two jobs hold the same target.

## Stockpiles and hauling

`Sim::zones()` holds the stockpile zones (`Zones.h`). A zone is a set of cells with a priority
(RimWorld's: Low, Normal, Preferred, Important, Critical) and a filter that allows every item def
except the ones listed. Each cell stores one stack, as RimWorld: items of one def up to its
`stackLimit`. A cell can't store while a building that isn't `Standable` stands on it (a door, a
stove) or it can't be walked.

| Command | Does | Fails when |
| --- | --- | --- |
| `CreateStockpileCommand{area, priority}` | a new zone of the cells in `area` that are walkable and in no zone; ids 1, 2, 3... in order | no cell qualifies; priority Unstored |
| `RemoveZoneCellsCommand{area}` | takes the cells out of their zones; empty zones go | no cell there was in a zone |
| `SetStockpilePriorityCommand{zone, priority}` | | unknown zone; Unstored |
| `SetStockpileAllowedCommand{zone, def, allowed}` | the filter, by Things defName | unknown zone or def |

An item is **stored** at its zone's priority if it lies in a zone that allows it, else it is
`Unstored`. The hauling scanner offers every item stored below the best zone's priority; for one it
picks, the best cell is in the **highest-priority zone that takes it, then the nearest** to the item,
with room, not held by another hauler and reachable. RimWorld's rule is the same: haul only to better
storage. The pawn carries as much as the cell has room for and it can carry: its
`carryingCapacity` (RimWorld's 75 kg for a human) over the item's `stats.mass` (1 if none), which can be
more than one stack (C24). Right after picking up, stacks of the same def in the 3x3 cells around it
that would go to the same zone anyway join the load, up to that limit (several stacks in one trip). The
rest stays where it was. Putting it down on a stack of the same def adds to that stack; a load
bigger than the cell takes fills it and goes on to the next cell. A load slows walking by up to
`JobRules.carrySlowdown` (ours, 0.25) at full capacity, in proportion to the mass carried
(`JobSystem::loadSpeedFactor`, used by the movement system). Nothing new is saved.

`Things::itemsAt(cell)` is the items-per-cell index this needs; `Things::splitOff` takes part of a
stack.

## What Unreal sees

- **Events:** `JobStarted` and `JobEnded` (`thing` the pawn, `job`, `target`, `cell`; `JobEnded`'s
  `message` is "done", "failed: why" or "interrupted: why") and U5's `PawnAction` `PickUp` and
  `PutDown` from the hauling steps. `PutDown` onto an existing stack names that stack.
- **Snapshot:** each pawn's `job` and `jobTarget` and what it carries (`carriedDef`, `carriedCount`);
  every zone (`zones`, with its cells in `zoneCells`).

## The hauling test

`SimSettings::pawns` and `items` still make the Stage 0 test, now on the job system: a stockpile zone
square around the walkable cell nearest the middle (`stockpileCell()`), big enough for a cell per
pawn to carry to at once plus the full stacks, items scattered over the reachable map outside it,
and pawns with the default priorities at its middle. It needs the WorkTypes def `Hauling` as well as
its Things defs; the test fixture defs (`Tests/data/defs`, `TestDefs.h`) have one. The 1 km check
(`simrun_paths`: 100 pawns, 2,000 items, one day) hauls slightly more than the placeholder did
(1,148 against 1,110) at the same speed, with no failed job; its items now end up in stacks of 75.

## Numbers

`JobRules` in `data/defs/Rules/Rules.json`: `defaultWorkPriority` 3 (RimWorld), and ours:
`idleSeconds` 60 (two real seconds at 1x), `avoidFailedTargetSeconds` 600, `maxRepaths` 5. The code
falls back to the same values when the def is missing.

## Gathering jobs (C3)

`Mine`, `CutPlant` and `HarvestPlant` target a cell (a rock or a plant) instead of a thing. They
reserve the cell like a thing, a scanner skips cells nobody can reach, and a failed gather job makes
the pawn skip that cell for the avoid time. `Job::cell` is the rock or plant, not where the pawn
stands; `touchCell` finds the walkable neighbour it works from (`WorkCandidate::hasReach` is for
the construction jobs, whose standing cell the scanner works out). `Job::progress` is the work
towards the next pick hit or on the plant. Details and numbers: [mining.md](mining.md).

## Not yet

- Interrupting a job for something more urgent (C6 needs, F6 alarm) and the emergency nodes.
- Scanners keep no ready-made lists: hauling looks through every item at each think. That is cheap
  at a few thousand items; per-chunk haul lists come if a profile asks.
- Opportunistic hauling (picking up more of the same item on the way) and hauling to a stockpile
  next to the item first.
- Forbidden items, zones on upper levels, merging touching zones, splitting a zone cut in two.
- A building put on a stockpile cell leaves the cell in the zone; the cell just can't store.
