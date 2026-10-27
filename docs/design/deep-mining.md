# Deep resources: ground-penetrating scanner and deep drill

Code: `ColonySim/DeepResources.h`, `Private/DeepResources.cpp` (the layer), `Private/DeepJobs.cpp` (the job and its
checks), `DeepWorkGiver` in `WorkGivers.cpp`. Defs: `data/defs/Things/DeepMining.json` (the two machines),
`deep` objects on the ore items in `Items.json`, `data/defs/Rules/DeepRules.json`, research `DeepDrilling` and
`GroundPenetratingScanner`. Tests: `Tests/DeepMiningTests.cpp`. Numbers are the RimWorld wiki's (ticks x 1.44 =
game seconds).

What mining already had before this (see [mining.md](mining.md)): rocks and ore lumps with a chance of a chunk, the
miner's yield by skill (60% at level 0, 100% at 8, 113% at 20), stone blocks cut from chunks at the stonecutter's
table. This adds what lies below the pick.

## The layer

`sim.deepResources()` holds the cells with deep resources: which item lies under the cell and how many units are
left (300 a cell). **Nothing is there until a scanner finds it**: a find makes the deposit. Items whose def has a
`deep` object can be deposits:

| Item | Cells (lump) | Commonality | Units per drill portion |
| --- | --- | --- | --- |
| Steel | 20 to 40 | 4 | 45 |
| Silver | 7 to 20 | 0.5 | 40 |
| Plasteel | 2 to 10 | 1 | 10 |
| Uranium | 4 to 10 | 1 | 10 |
| Jade | 1 to 5 | 0.5 | 10 |
| Gold | 1 to 5 | 0.5 | 10 |

Plasteel, uranium, jade and gold are new items (stats unverified). A deposit grows from a random ground cell over
random neighbours, going round water and cells that already hold a deposit. The layer has its own random stream
(`Rng` seeded from the map seed, saved).

## Ground-penetrating scanner

A 3 x 3 building (150 steel, 4 components; the wiki's advanced component is left out until we have one), 700 W,
research `GroundPenetratingScanner` (1,000 points, after `DeepDrilling`). It does not work under a roof. A colonist
with the **Research** work type sits beside it (job `DeepWork`, task `ScanDeep`). Each second of work at speed `s`
(`0.2 + 0.1 x Intellectual level` times Consciousness and light, ours as the research bench) has a chance of
`s / 259,200` of a find (3 game days on average, exponential as RimWorld's), and work past 518,400 s (6 days)
always finds. Each scanner remembers its own work. A find resets it, ends the job and raises
`DeepDepositFound` (`def`, `cell`, `count` cells, `thing` the scanner).

## Deep drill

A 1 x 1 building (100 steel, 2 components), 200 W, research `DeepDrilling` (4,000, after Microelectronics). A
colonist with the **Mining** work type works it (task `DrillDeep`) when there is a deposit within 2.6 m
(21 cells). The work is at the pick's speed (`miningSpeed`); a cycle is 20,160 s of work at speed 1 (14,000
ticks); the work stays on the drill if the colonist leaves. When a cycle is done the nearest deposit cell gives
one portion (the cell loses the portion; the miner's yield curve scales what comes up, at least 1) as items at
the colonist's feet, and `DeepPortionDrilled` is raised. A drill with nothing under it brings up one rock chunk of
the map's rock types instead; the work giver does not offer a drill that has nothing in reach, so this only
happens when the deposit ran out during a cycle.

## Hooks for later

- `DeepResources::findDeposit(def, out)` makes a deposit anywhere on the map (an invalid `def` picks by
  commonality). The **long-range mineral scanner** (a world-map building in RimWorld) and incidents or quests
  that reveal ore should call it; it raises no event itself, the caller says what was found.
- `placeDeposit(def, cell, cells)` for scenarios and tests.

## Not yet

- Drawing deposits and the machines (the snapshot lists the found cells in `deepResources`, rewritten when
  `deepChanges` moves).
- Minifying the drill, the vanometric power cell, sea ice and orbit rules.

## Saves

Section `deep` 1: item names, one entry per cell (cell, item, units), the work of each machine, the random stream
and the counters. Older saves load with no deposits. New job type number `DeepWork` at the end.
