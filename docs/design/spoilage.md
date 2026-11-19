# Rot and storage

Code: `ColonySim/Spoilage.h`, `Private/Spoilage.cpp`; the `Spoilage` component and `Things::addToStack` in
`Things.h`; the cooking and eating hooks in `Private/BillJobs.cpp` and `Private/NeedJobs.cpp`. Tests:
`Tests/SpoilageTests.cpp`. Data: `SpoilageRules` (`data/defs/Rules/SpoilageRules.json`), the `Rottable`
comp and `stats.deteriorationRate` on item defs (D-tasks). Save section `spoilage` 1. Task C16. Builds on
[temperature](temperature.md) and [room stats](room-stats.md).

## The component

Every item whose def has a `Rottable` comp, a `stats.deteriorationRate` or is a meal gets a `Spoilage`:
`rot` (0 fresh to 1 gone), `decay` (the fraction of a hit point worn off), `poisoned`, and `ingredients`
(Things defs, sorted by id; meals only). Stacks keep one value for the whole stack. When a stack is
put onto another (`Things::addToStack`) the rot is averaged by count, ingredients are joined and one
poisoned part poisons the stack. `splitOff` copies it.

## Rot

Every 60 game seconds each *placed* item that rots gains `speed * 60 / (daysToRotStart * 86400)`. The speed
follows the temperature of its cell (`TemperatureSystem::temperatureAt`, so a cold room, a freezer or a
cave keeps food): 0 at or below 0 degrees, 1 at or above 10, linear between. At `rot >= 1` the item is
gone (`ItemSpoiled`, message "rotted"). Items being carried do not rot (they are not placed).

## Wear in the open

An item with `stats.deteriorationRate` (hit points a game day) that lies on a cell with no roof, and not
on a building with `building.preventDeteriorationOnTop` (no def has it yet), loses hit points at that
rate times `outdoorDeteriorationFactor` (1; E1's weather calls `setOutdoorFactor` every step: 5 while it rains). At 0 it
is gone (`ItemSpoiled`, message "weathered"). Quality's `deteriorationFactor` is not applied yet.

## Meals and food poisoning

A bill's products remember what was used (`ingredients`). A meal is poisoned when it is made by chance
`dirtyKitchenChance * dirt + unskilledChance * unskilled`: `dirt` is 0 at kitchen cleanliness 0.4 or
better and 1 at -1.1 or worse (the room's stat, or the cell's own outdoors); `unskilled` is 0 at Cooking
8 or better and 1 at 0. Eating a poisoned stack gives, with `onIngestChance` (0.35), a `PawnPoisoned`
event and a call to the handler set with `SpoilageSystem::setPoisonHandler`. **H2 must install that
handler to give the pawn the `FoodPoisoning` hediff** (diseases do not exist before H2).
The snapshot has each item's `rot`; whether a meal is poisoned is hidden from it.

All numbers except 0 and 10 degrees and 0.35 are ours (see the def's `ours` and `unverified` lists).
Cancelling a bill returns fresh ingredients, not the rot they had.

## Corpses

(sneed, 2026-11-14: flies and maggots on corpses.) Every dead pawn that is not a machine (`race.mechanical`)
gets a `CorpseRot` component on the first spoilage pass after it died. Its `days` are rot-days: every pass adds
the rot speed of the body's cell (the same 0 to 10 degree curve as food) times the pass length, so a freezer or
a winter night keeps a body. They stop at `corpse.daysToDessicated`. Save section `corpses` 1; the days are in
the state hash.

`SpoilageSystem::corpseLook(days, celsius)` gives the stage and what to draw, and the snapshot copies it onto the
body's `ThingView` (`corpseDays`, `corpseStage`, `flies`, `fliesFlying`, `maggots`):

| | from | numbers (`SpoilageRules` `corpse`) |
| --- | --- | --- |
| Fresh | death | |
| Rotting | 2.5 rot-days | RimWorld's corpse `daysToRotStart` (unverified) |
| Dessicated | 25 rot-days | RimWorld's `daysToDessicated` (unverified) |
| flies | 0 at 0.01 days (about 15 minutes), a full swarm at 0.25, thinning from 12 days, gone when dessicated | ours |
| flies on the wing | none at 10 degrees or less (they sit on the body), all at 20 or more | ours |
| maggots | from 1 day, the most at 2.5, gone by 18 | ours |

Flies and maggots are cosmetic: no mood, no disease, nothing else in the sim reads them (sneed, 2026-11-17: no
mood debuffs for corpses). Chitinid bodies rot and draw flies too; the PC side draws the swarm and the maggots
from these numbers and darkens a rotting body.
