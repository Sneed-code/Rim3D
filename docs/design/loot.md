# Loot

Code: `ColonySim/Loot.h`, `Private/Loot.cpp` (`sim.loot()`). Tests: `Tests/LootTests.cpp`. Data:
`data/defs/LootTables/LootTables.json`, `data/defs/Rules/LootRules.json`, `lootTable` on
`PawnKinds`. Raids ([raids.md](raids.md)), the narrator ([narrator.md](narrator.md)) and quests
read it.

Loot is **data plus one roll function**. A `LootTables` def says what a table can give; anything
that wants loot rolls the table by name and drops the result:

```cpp
Rng rng = sim.loot().rngFor(salt, sim.time().seconds);       // same dice on every run
auto items = sim.loot().roll("SupplyCache", points, rng);     // [{def, count}]
sim.loot().drop(items, cell);                                 // stacks on free cells nearby
```

## A table

| Part | Meaning |
| --- | --- |
| `items[]` | each rolled on its own: `thing`, `chance` (1), `count [min,max]`, `perHundredPoints` (extra count per 100 raid points) |
| `stacks` | `rolls [min,max]` draws, each picks one of `from[]` by `weight` and makes a stack worth `value [lo,hi]` silver (count = value / `stats.marketValue`, at least 1, at most the stack limit; a `count` on the entry overrides) |
| `stacks.pointsScaled` | the stack value grows with points / `referencePoints` (100), kept between 0.5 and 4 (`LootRules`) |

Rolling with 0 points means no scaling.

## Who uses which table

- **Raiders** (`PawnKinds.lootTable`): pirates `RaiderPackPirate`, the mercenary `RaiderPackMercenary`,
  clan warriors `RaiderPackTribal`, Concord troopers `RaiderPackTrooper`, Free Hold gunners the pirate
  pack. A raider that **dies** drops its pack where it lies (`LootSystem::step`, from `PawnDied`),
  scaled by its squad's points; the roll is seeded from the world seed, the pawn and the second, so
  it is the same on every run and nothing is saved. Raiders who leave the map keep theirs. The gun and
  clothes a raider wears stay on the corpse for a colonist to strip (`apparel.md`). Packs carry the
  **packaged survival meal** (30% pirates, 50% mercenaries, 60% troopers), pemmican for clans, medicine
  and silver.
- **Cargo pod** (`CargoPod`): the `ResourcePodCrash` incident has a worker now. It lands one stack
  (a single good worth 200 to 900 silver) 10 to 40 cells from the middle of the colony on open, unroofed
  ground, and sends a positive Letter ("A cargo pod has crashed nearby, with 120 steel.") with the cell.
- **Ancient danger** (`AncientDanger`, always 3 to 8 survival meals plus 3 to 5 valuable stacks),
  **ruin stash** (`RuinStash`) and **supply cache** (`SupplyCache`, for quests): not called by anything
  yet. Map generation's ancient rooms and the quest supply cache call
  `sim.loot().roll(name, 0, rng)` and `drop`.

## Numbers

RimWorld's wiki gives no tables for these, so every number here is marked `unverified` in the defs
(chances, counts, weights, value ranges); only the idea (raiders may carry survival meals, a pod is one
stack of a single good) is the wiki's. Tune the data, not the code. Survival meals as loot closes the
BACKLOG note from the long-lasting food work.

## Not yet

Ancient danger rooms and ruins do not exist in map generation; nothing rolls their tables. Pawns do
not carry the pack visibly (it is rolled when they die). A lone `RaidState` squad that is gone before a
slow bleed-out death drops with 0 points (scale 1).
