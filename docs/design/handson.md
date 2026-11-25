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

Refused, with a plain-words reason shown on screen: the target is out of reach (`kHandsOnReachCells`, **two cells**
from the cell the colonist stands in to the nearest cell of the target, diagonals too: a 5 by 5 square round the feet;
sneed 2026-11-25, the player only), another colonist holds it, the hands are full (work other than
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
called away). The job ends by itself when the work is done.

**Holding on** (sneed 2026-11-25: "if I hold down F it goes to the next cell after finished, so you can rapidly cut
grass"). While F stays held and the job is over, `UHandsOnSubsystem::ContinueWork` asks the sim for the next target
(`JobSystem::nextHandsOn(pawn, kind, viewX, viewY)`): the same kind of work (mine, chop, cut, pick or build; a tree is
not grass), that the colonist can do now from where it stands, within the two cells. The one nearest the way the camera
looks wins (cost: one minus the cosine off the view, plus 0.15 a metre; ties go to the first cell in row order), and it
goes in as an ordinary `HandsOnCommand`, so it replays like any other. Nothing of the kind left in reach, or a fight
(locked on, or fighting): F does nothing more until it is pressed again. Picking up and putting materials in don't chain.

## Carrying and putting down

Picking up gives the colonist a `Carrying` like a hauler. **X** sends `HandsOnCommand{PutDown, cell}`: the
stack goes on the cell under the crosshair if that is within reach, free or holding the same item with room,
and not held by another colonist, else on the nearest free cell around the feet. Letting go of the colonist
(Tab, or it goes down) puts it down at the feet (`JobSystem::putDown`).

## The build screen

**B** in direct mode (sneed 2026-11-25: "it shows me my UI, like overseer mode, with build open by default; when I click
and drag walls I don't move; B just toggles the overseer interface without the view") brings up the overseer's panels
over the walking view: the bottom bar with the **Build** menu open, the order tools, the inspect panel. The camera stays
where it is (`UOverseerSubsystem::SetUiOverDirectView`: `IsOverseerActive` is true for the panels and tools, while
`IsOverseerCamera` stays false, so the levels cutaway and the roofs stay as walking has them). The mouse cursor comes up
in the middle of the screen, where the crosshair was. Clicks go where the overseer camera's would
(`UProtagonistSubsystem::UpdateBuildUi`): a tool in hand plans under the cursor, picked through this camera (click, or
drag for a wall line), the right button puts the tool away, R turns, a click with no tool selects. The body's keys are
off (`Rim3DKeys::SetGameKeysBlocked`): no walking, turning, punching or shooting, so dragging walls never moves the
colonist. **B** again or **Esc** goes back to walking (the tool is put away). Leaving direct mode, dying, a load or a
take-over close it too. Colonists build the plans as usual, or the player delivers and builds them by hand.

The old crosshair ghost (`UOrdersSubsystem::IsDirectBuilding`, the wheel, Q, R and click on a building in hand) is still
in the code but no key reaches it.

## Looking

`UHandsOnSubsystem::FindTarget` casts the camera ray against the sim, not Unreal collision, like the
overseer picking: rock and plants by marching the ray through cell columns (a rock is 4 m tall, a tree 6.5 m,
anything else 1.1 m), items and plans by their boxes (`SimPicking::ThingBox`), the ground by the height map.
Nothing behind the colonist's head counts (the third person camera hangs behind it). The nearest wins.

## Screen

Bottom middle (sneed 2026-11-25, moved from under the crosshair): `F  hold: Chop poplar tree` in green when it can be
done, or the reason in grey (`get closer`, `the wall needs materials: pick some up and bring them`), with a progress bar
under it while the job runs (`ThingView::jobProgress`: a swing of the pick, a plant, a frame). It stacks on what is
carried, which stacks on the weapon box (`UFightSubsystem::GetBottomBoxTop`; bare fists with nothing else to say have
no weapon box). The key is the one bound to Interact. **An item looked at says it on itself**: `F   Pick up Steel x35`
drawn over it in the world (projected from the top of its box), green when it can be taken, else why not (`Steel: get
closer`, `Steel: Your hands are full`). F on a weapon in reach takes it into the hands (`EquipCommand`), ahead of the
plain pick-up. Messages from the sim (a refused command) near the top of the crosshair.

## Save and replay

`HandsOnCommand` is logged and replays like every command (replay type `HandsOn`, mode `Work`,
`PutDown`, `Stop`). The job's `handsOn` field is in the save (format 19) and in the state hash.

## Not done here

- **No work animation or sound.** The `PawnAction` events (`Mine`, `Cut`, `Harvest`, `Build`, `PickUp`) are sent
  as for any colonist, but the pawn actors only play pick-up and put-down so far; the colonist stands still while
  it works.
- **Deconstruct, bills (cooking), sowing, hauling to a stockpile by hand.** Pick up and put down on the ground only.
- **Only one thing at a time in the hands**, and 75 units at most (the colonist's carrying capacity).
- **Game speed.** Direct mode holds the game at 1x (30 steps a second), so work takes real time: a granite block about
  1.7 minutes at Mining 8, an oak (2016 work) about a minute at Plants 12; the damage stays on a rock between visits.
- **No skill gain message**; XP is gained as for a colonist (Mining, Plants).
