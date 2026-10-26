# Fuel

Code: `ColonySim/Fuel.h`, `Private/Fuel.cpp` (burning, counting, refilling), `Private/FuelJobs.cpp` (the
`Refuel` job), the `Refuel` scanner in `Private/WorkGivers.cpp`. Tests: `Tests/FuelTests.cpp`, the
colony scenario (`woodIn`). Task C11.

A building whose Things def has a `fuel` field holds fuel, as RimWorld's `CompRefuelable`. The fueled
stove burns wood to cook, a campfire and a torch lamp burn it all the time, and the wood-fired
generator will (E5). Colonists with Hauling work bring the wood. **A building out of fuel is stopped.**

## The def field

```json
"fuel": { "item": "WoodLog", "capacity": 50, "perUse": 1.0, "autoRefuelPercent": 0.3 }
```

| Field | Meaning |
| --- | --- |
| `item` | the Things item def that burns |
| `capacity` | most it holds, in items |
| `perDay` | burnt all the time while it has any, items per game day (generator 63, torch lamp 7.5) |
| `burnHoursPerWood` | the same as `perDay` the other way round, used when `perDay` is 0 (campfire: a wood every 2 hours, 12 a day) |
| `perUse` | burnt per product of a bill finished on it (the stove: 1 wood a meal; a four-meal run burns 4) |
| `autoRefuelPercent` | colonists refuel at or below this fraction of `capacity` (default 0.3, RimWorld's) |
| `initialPercent` | fraction of capacity a new building starts with (default 0: **a new building is empty**, as RimWorld) |

Capacities, rates and the stove's 50 wood are RimWorld's (recalled, `unverified` in the defs). `perUse`
as "a wood per meal made" is ours (`ours`; BACKLOG asked for it).

## The component

`Fuel{amount, autoRefuel, fillOrdered}` is on every finished building with a `fuel` field (made by
`Things::make`; blueprints and frames have none). `amount` is a float of items: a campfire burns 12 a
day, not whole items. `FuelSystem` (owned by the job system, `sim.jobs().fuel()`) answers `lit`,
`canUse`, `fraction`, `roomFor`, `wantsFuel` and does `use` and `add`.

- **Burning.** `FuelSystem::step()` runs once a game second, before the jobs: a building with `perDay`
  loses `perDay / 86,400` and, when it reaches 0, sends `OutOfFuel` once. It stays dark until refuelled.
- **The stove.** Cooking needs `perUse x products` in the stove. The cook scanner doesn't offer a
  stove with less than one use of fuel, and a bill whose run needs more than is there is skipped. The
  fuel is burnt when the run finishes (with the product), so a cook interrupted part way loses
  nothing. If the fuel is gone in the middle (`DoBill` fail check) the job fails, "the building is out
  of fuel", and the ingredients are put back.
- **Light.** A lamp (`glower`) whose building has a `Fuel` part with nothing in it gives no light
  (`Lights::setActive` in `Sim` asks `fuel().lit`); the fuel system marks the light layer dirty when a
  building runs out or gets fuel again. So a new torch lamp or campfire is dark until someone fills it.
- **Power (E5).** `PowerGrid::setFuel` asks `fuel().lit`: a wood-fired generator with an empty
  `Fuel` part makes no power. It burns `perDay` only while it is switched on (`setBurning`: a building
  with a power part burns unless its `PowerState` is `Off`; campfires and torch lamps, with no power
  part, always burn). New generators start empty. Heat from a lit campfire is E2's (it reads `lit`).

## Refuelling

A `Refuel` scanner sits first in Hauling's tasks (`WorkTypes`: `["Refuel", "HaulToStockpile"]`).

| | |
| --- | --- |
| Offers | a finished building with `Fuel` that `wantsFuel`: auto refuel on and at or below `autoRefuelPercent` of capacity, or a fill order; with room for at least one item; not marked for deconstruction; no other pawn refuelling it |
| Job | `Refuel` (`Job::thing` the fuel stack, `site` the building, `cell` beside the building, `count`): go to the nearest free reachable stack of the fuel item, pick up as many as fit (at most the pawn's carrying capacity), carry it, stand beside the building for `kRefuelWorkSeconds` (345.6 game seconds, RimWorld's 240 ticks), put it in |
| Holds | the stack, and the building for delivery (the same hold as a construction delivery, so one refueller at a time but a cook can still use the stove) |
| Fails | the building is gone, full or has no fuel part; the stack is gone; the pawn can't use its hands |
| Left over | fuel that did not fit stays in the pawn's hands and is put down when the job ends |

One trip carries one stack. A building that is still low gets another job on the next think. After a
full tank (`roomFor` is 0) a fill order clears itself.

## Commands, snapshot, events

- `SetRefuelCommand{building, autoRefuel, fillNow}`: turns RimWorld's "allow auto refuel" on or off, and
  `fillNow` orders a fill to the top whatever the level (works with auto off). Fails for anything that
  is not a finished building with a fuel part.
- Snapshot `ThingView`: `fuel`, `fuelCapacity` (0 for a building with none), `autoRefuel`,
  `fillOrdered`. The panel (U8's) shows a bar and the two settings when the U task for it is done.
- Events: `OutOfFuel` (`thing`, `def`, `cell`), `Refueled` (the same, `amount` items in),
  `PawnAction::Refuel` (`thing` the pawn, `target` the building) when the pawn starts putting fuel in,
  and `JobStarted`/`JobEnded` with `JobType::Refuel`.

## Saves

Section `fuel` (version 1): the `Fuel` component per building. A save from before the section has none,
so **a building with a fuel part and no saved fuel loads full**: the stove of an old save keeps cooking
until it first runs low (`Tests/FuelTests.cpp` loads the format 19 colony-cook file). Fuel is in
`Sim::stateHash()`. Save checks: `Refuel` is a job type (`JobSystem::afterLoad` matches what the pawn
carries to the step, as for deliveries).

## Not yet

- Fuel items other than wood (chemfuel, components of a wood-fired generator's smoke), and fuel that
  can be taken out again.
- Refuel from a stockpile only; a storage-aware "bring it from the nearest stockpile first" rule.
- A refuel skill or speed: it is a flat 345.6 seconds, as RimWorld's.
- Heat from a lit campfire (E2), rain putting a campfire out (E1).
