# Hands-on work: doing the job yourself

Code: sim side `ColonySim/Private/HandsOnJobs.cpp` (`JobSystem::planHandsOn`, `handsOn`,
`handsOnPutDown`, `handsOnStop`, `jobProgress`), `HandsOnCommand` in `Commands.h`, the `handsOn`
field of `Job`; Unreal side `game/Source/Rim3D/HandsOnSubsystem.*`, the direct-mode ghost in
`OrdersSubsystem.*`. Tests: `Tests/HandsOnTests.cpp` and the smoke test `handson`
(`tools/smoke.ps1 -Test handson`). Task U10.

In direct mode ([protagonist.md](protagonist.md)) the player looks at something and **holds F** to do the
sim's own job on it, with the colonist's own skill and speed. The job is the same code a colonist
runs on its own ([jobs.md](jobs.md), [mining.md](mining.md), [construction.md](construction.md)); what is
different is that the player has already walked there, so the walking steps are skipped.

## What F does

The sim decides from what the crosshair is on (`JobSystem::planHandsOn(pawn, thing, cell)`; Unreal asks
it for the prompt and sends the same two numbers back in the command).

| Looking at | F does | Job |
| --- | --- | --- |
| a rock or ore cell | mines it, one swing every 144 s at 100% speed | `Mine` |
| a tree | chops it, wood drops at its cell | `CutPlant` |
| a ripe bush or other plant that gives something | picks it | `HarvestPlant` |
| an item | picks it up (as much as the colonist can carry), at once | none (instant) |
| a blueprint or frame, carrying a material it needs | puts the material in; what it doesn't need is put down | `DeliverResources`, started at its last step |
| a frame with all its materials | builds it | `BuildFrame` |

Refused, with a plain-words reason shown on screen: the target is out of reach (`kHandsOnReach`, 2 m
from the edge of the cell or footprint), another colonist holds it, the hands are full (work other than
delivering needs empty hands), a plan that still needs materials, a colonist who is down.

**Reach, not walking.** The job has `handsOn` set: its go-to steps end at once and every step checks that
the colonist is still within reach. Step away and the job ends as interrupted, "out of reach" (not a
failure: nothing goes in the avoid list). The sim's own colonists walk to a cell beside the target
instead.

**Marks.** The gather jobs need a designation. If the cell has none, the job makes one (`handsOn` = 2)
and takes it off again if the job ends without finishing, so a half-felled tree is not left for
colonists to finish. A mark that was already there stays.

**Holding.** F down sends `HandsOnCommand{pawn, Work, thing, cell}` once, F up sends `Stop`. Progress
persists where the job keeps it: the damage on a rock stays, a tree starts again (as when a colonist is
called away). The job ends by itself when the work is done; holding F on does not start another.

## Carrying and putting down

Picking up gives the colonist a `Carrying` like a hauler. **X** sends `HandsOnCommand{PutDown, cell}`: the
stack goes on the cell under the crosshair if that is within reach, free or holding the same item with room,
and not held by another colonist, else on the nearest free cell around the feet. Letting go of the colonist
(Tab, or it goes down) puts it down at the feet (`JobSystem::putDown`).

## The build ghost

**B** takes the architect menu's Build tool in hand, in direct mode. The ghost is the same grid-snapped
see-through mesh as in the overseer view, aimed by the crosshair (`UOrdersSubsystem::IsDirectBuilding`,
the cell comes from `UHandsOnSubsystem`), green where it can stand and red where it can't. Within 14 m of the
colonist only. Wheel picks the building, Q the stuff, R turns it, the left button plans it
(`PlaceBlueprintCommand`, as the overseer's click), B or the right button or Esc puts it away. While it is in
hand the gun is quiet (the left button and R are the ghost's). Colonists build the plan as usual, or the
player delivers and builds it by hand.

## Looking

`UHandsOnSubsystem::FindTarget` casts the camera ray against the sim, not Unreal collision, like the
overseer picking: rock and plants by marching the ray through cell columns (a rock is 4 m tall, a tree 6.5 m,
anything else 1.1 m), items and plans by their boxes (`SimPicking::ThingBox`), the ground by the height map.
Nothing behind the colonist's head counts (the third person camera hangs behind it). The nearest wins.

## Screen

Under the crosshair: `F  hold: chop poplar tree` in green when it can be done, or the reason in grey (`get
closer`, `the wall needs materials: pick some up and bring them`). A progress bar while the job runs
(`ThingView::jobProgress`: a swing of the pick, a plant, a frame). What is carried and the ghost's keys at
the bottom. Messages from the sim (a refused command) near the top of the crosshair.

## Save and replay

`HandsOnCommand` is logged and replays like every command (replay type `HandsOn`, mode `Work`,
`PutDown`, `Stop`). The job's `handsOn` field is in the save (format 19) and in the state hash.

## Not done here

- **No work animation or sound.** The `PawnAction` events (`Mine`, `Cut`, `Harvest`, `Build`, `PickUp`) are sent
  as for any colonist, but the pawn actors only play pick-up and put-down so far; the colonist stands still while
  it works.
- **Deconstruct, bills (cooking), sowing, hauling to a stockpile by hand.** Pick up and put down on the ground only.
- **Only one thing at a time in the hands**, and 75 units at most (the colonist's carrying capacity).
- **Game speed.** Direct mode holds the game at 1x, so a granite block takes about 1.7 minutes of real time at
  skill 8; the damage stays on the rock between visits.
- **No skill gain message**; XP is gained as for a colonist (Mining, Plants).
