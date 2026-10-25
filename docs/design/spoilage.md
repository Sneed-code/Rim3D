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
rate times `outdoorDeteriorationFactor` (1; E1 calls `setOutdoorFactor` to raise it in rain). At 0 it
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
