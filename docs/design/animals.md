# Animals (data)

Stage 2 task D5. Data only: the sim reads none of it until N1 (animals on the map), N2 and the
taming and husbandry tasks. Files: `data/defs/Things/Animals.json`, `Things/AnimalProducts.json`,
two bodies in `Bodies.json` and the new `Wing` part in `BodyParts.json`.

## The 15 animals

Hare, squirrel, deer, wild boar, muffalo, alpaca, cow, chicken, husky, cat, timber wolf, grizzly
bear, cougar, boomalope, thrumbo. They keep RimWorld's plain names: the invented-animal renames
(muffalo, boomalope, thrumbo) are still only *proposed* in [naming.md](naming.md). When sneed picks
them, the rename is a data change (def names, labels, `Meat_` and `Wool` defs).

Every animal is a `Things` def (`category: "Pawn"`) inheriting `AnimalBase`.

## Fields on `race`

| Field | Meaning |
| --- | --- |
| `body` | `QuadrupedAnimal` (four legs, pelvis, spine, organs) or `BirdAnimal` (two legs, two wings, beak is the `Jaw` part). Same part defs as the human body, so the capacities in `Capacities.json` work unchanged |
| `baseBodySize`, `baseHealthScale`, `lifeExpectancy` (years) | as `Human` |
| `diet` | `Herbivore`, `Omnivore`, `Carnivore`; `foodPerDay` is nutrition a day (human rate x body size) |
| `meatDef`, `meatAmount`, `leatherDef`, `leatherAmount` | what the corpse gives. Chickens have no leather |
| `wildness`, `trainability` | `None`, `Simple`, `Intermediate`, `Advanced`; wildness makes taming harder |
| `herd` | `minSize`, `maxSize`; missing = lives alone |
| `gestationDays`, `litterSize` | live birth. Chickens lay eggs instead |
| `lifeStages` | baby, juvenile, adult: `minAgeYears`, `bodySizeFactor`, `healthScaleFactor`, `foodFactor` (ours) |
| `milk`, `wool`, `eggs` | `thing`, `intervalDays`, `amount`; eggs have `unfertilizedThing`, `fertilizedThing`, `intervalDays`, `count`; the fertilized egg's `hatches` says what it becomes and when |
| `wildBiomes` | biome def name to commonality (ours). N1 reads a biome's own `animals` list first (RimWorld's numbers) and uses this only for a biome with no list ([animal-life.md](animal-life.md)). Cow, chicken, husky and cat are domestic (`domestic: true`) and never live wild |
| `predator`, `manhunterOnDamage`, `rare` | behaviour flags N1 and the incident tasks read |

`tools` are the melee attacks (`power`, `cooldownTime` in game seconds, like the human's fists). The
boomalope has a `deathExplosion`.

## Items

`MeatBase` and one `Meat_<Animal>` each (category `MeatRaw`, so fine meals can use them),
leathers (`Leather_Light`, `Plain`, `Pig`, `Dog`, `Wolf`, `Bear`, `Panthera`, `Thrumbo`, with
`stuffProps` for apparel), `MilkRaw`, two eggs, and `WoolAlpaca`, `WoolMuffalo`.

## Where the numbers come from

The rimworldwiki.com pages weren't read for this task, so the race numbers are from memory of
RimWorld and every animal def lists `stats`, `race` and `tools` in `unverified`; check them against
the wiki when a task leans on them. Our own: `lifeStages`, `herd`, `wildBiomes` and the animal body
trees (`ours`).

## On the map

N1 puts these animals on the map, makes them wander, graze and sleep, and fills and empties the edges:
[animal-life.md](animal-life.md).
