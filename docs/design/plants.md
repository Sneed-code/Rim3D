# Plants

Code: `ColonySim/Plants.h`, `Private/Plants.cpp`, `Sim::plants()`. Tests: `Tests/PlantsTests.cpp` and
the `simrun_plants` ctest. Task C2. Defs: `data/defs/Things/Plants.json` (the plants),
`Biomes/` (what grows where), `Terrain/` (fertility), `Rules/Rules.json` (`PlantRules`).

Plants are a **grid layer**, not things: one flat array per field (kind, growth, age) with at most
one plant per cell, because a 1 km map holds hundreds of thousands. A 512 m map from seed 42 has
about 119,000 plants, and making them takes a few milliseconds.

```cpp
const Plants& p = sim.plants();
p.has(cell);  p.defAt(cell);  p.growth(cell);  p.mature(cell);  p.ageHours(cell);
p.fertility(cell);                       // 0 on water and rock
sim.plants().place(cell, def, 0.0f);     // sprouts one (jobs, later spreading)
sim.plants().remove(cell);               // felled, harvested whole, built over
sim.plants().setGrowth(cell, 0.3f);      // a picked berry bush goes back to harvestAfterGrowth
```

## Fertility

Each cell has a fertility, worked out when the map is made and not saved (it follows from the seed):
water and rock 0, sand from the `Sand` terrain def (0.1), and soil as one of `Gravel` (0.7), `Soil`
(1.0) or `SoilRich` (1.4) in 40 m patches (value noise from the seed). The terrain enum in `Map` is
still the placeholder (water, sand, soil, rock); when terrain defs replace it, `Plants` reads the
cell's terrain def instead and the patches become real terrain.

## Making the wild plants

For every cell with fertility above 0:

1. One draw decides whether the cell has a plant: chance `plantDensity` (the biome's 0.65) times
   `min(fertility, 1)`. Sand (0.1) is therefore sparse and rich soil is no denser than soil.
2. Among the biome's `wildPlants`, those whose `fertilityMin` the cell meets are weighted by
   commonality. Trees' weight swings between 0.2 and 1.8 times in 48 m patches, so there are forests
   and meadows (RimWorld's clumping is not copied).
3. Start growth: a third of the plants are fully grown and older, the rest are anywhere on the way up.

All of it comes from `Rng` streams made from the map seed, apart from the sim's own stream, so
adding a plant def never shifts anything else.

## Growth

`Sim::step` calls `Plants::growHour(hourOfDay)` at the end of every game hour. Each plant:

- gets an hour older, and **dies** at `growDays x lifespanDaysPerGrowDays` days (oak 270, grass 20);
- in the growing hours (`PlantRules.growHours`, 06:00 to 19:00) and when the cell's fertility is at
  least the plant's `fertilityMin`, grows by `1 / (growDays x 24)` of full growth times the rate
  `1 + (fertility - 1) x fertilitySensitivity` (never below 0). Thirteen growing hours a day make
  the wiki's "with rest" times: grass `2.5 x 24 / 13` = 4.6 days, an oak 55 days.

Tune it all at once with the `plantGrowth` multiplier in `data/tuning.json` (1 if absent).

Not in yet, each for a later task: the light threshold (`growMinGlow`, with day and night light),
temperature limits, plants spreading and sowing (C3 harvests, a farming task sows), blight, and
fire. Plants under buildings: **a building task (C4) must call `plants().remove` on its footprint**.
Plants also do not slow or block pawns yet: the path layer ignores `pathCost` from plant defs, so
`Plant_TreeOak`'s `PassThroughOnly` is not enforced until C3 or the next movement task decides how
trees stand in the way.

## Numbers and the state hash

Growth is in millionths of full growth (`uint32`), age in game hours, so the layer saves and hashes
identically on Linux and Windows. The layer keeps a running hash (XOR of one hash per plant, updated as
plants change) which `Sim::stateHash` includes; asking costs nothing.

## Saves

Save format 9: the plants as parallel lists (def names used, then per plant its cell index, def, growth
and age), after the zones. Older saves load with no plants. See `saves.md`.

## For Unreal (U3)

Read the layer through `sim.plants()` (`records()` lists every plant in cell order). It is not in the
snapshot yet: U3 decides whether to add a per-chunk list there, since a full copy of 119,000 plants
each step would be wasteful. Plants change only on the hour and when a job changes one.
