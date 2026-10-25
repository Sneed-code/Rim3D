# Animal life (animals on the map)

Code: `ColonySim/Animals.h`, `Private/Animals.cpp`. Tests: `Tests/AnimalTests.cpp`, the ctest
`simrun_animals`. Rules: `data/defs/Rules/AnimalRules.json` (all ours). Stage 2 task N1. The animal
data is [animals.md](animals.md) (D5); hunting, taming, breeding and fighting are N2 to N5.

## What an animal is

A pawn whose def says `race.intelligence: "Animal"`. `Things::make` gives it the usual pawn parts
(`Mover`, `Posture`, and `Health` from `race.body`, so the animal bodies and capacities of D5 work
unchanged) and an `Animal` component. It has **no `Worker`**: the job system never sees it, and
`SpawnThingCommand` doesn't make a colonist of it. Its side is `kWildFaction` (100), set the first
time the animal system looks at it.

`Animal` holds: `wild`, `herd` (0 alone), `bornAt` (game second, negative for an animal older than
the game), `food`, the mode it is in (`Idle`, `Wander`, `GoToFood`, `Eat`, `Sleep`, `Leave`) with
its `until`, `nextThink` and `goal`, `diedAt`, `staysUntil`, `sleepShift`, `feeding`. Everything is
saved and hashed.

**Life stages** come from the def's `race.lifeStages` and the animal's age: `lifeStage()` is 0 baby,
1 juvenile, 2 adult and `bodySize()` is the def's size times the stage's factor. A game year is 60
days. Nothing ages into a new stage yet that the sim cares about except the size and food numbers;
births are N4. Animals placed at map generation are adults up to half their life expectancy.

## Putting animals on the map

`SimSettings::wildAnimals` is the switch: 0 for none (the default, so no old test or save changes),
else the density in percent (100 = the biome says). The map-making constructor calls
`AnimalSystem::populate` after the colonists are placed. It is part of a replay's settings and the
`animals` save section, so a replay or a save of a game with animals is the same game.

- The biome is `TemperateForest`, the same as the plants (both move to the settings together).
- Target weight = biome `animalDensity` x map area / 10,000 x percent / 100, counted in **body
  sizes** (a 512 m map in temperate forest wants 97; a 1 km map 388, about 300 animals). RimWorld's
  own formula is not copied; this is ours and marked unverified.
- Which animals: the biome's `animals` list gives each species' commonality (RimWorld's numbers).
  A biome without such a list falls back to each animal's own `race.wildBiomes` (ours). Domestic
  animals (`race.domestic`) never live wild. Names in the Biomes file now match the animal defs
  (`WildBoar`, `TimberWolf`, `GrizzlyBear`), and `tools/check_defs.py` checks the lists.
- A group is picked by commonality, its size from `race.herd` (min to max, 1 if none), and placed
  round a random walkable cell at least 25 m from any colonist. Groups are placed until the weight
  is reached. Members share a herd id; the **leader is the living, able member with the lowest
  entity id** (nothing saved, found again each step it is needed).

## What an animal does

Each animal decides when its `nextThink` comes, about every 8 to 45 seconds when standing and every
5 seconds while walking, staggered by a random first look. A decision has its own random stream from
the seed, the animal and the second (`animalRngFor`), so the order animals are visited in changes
nothing. Animals are visited in entity id order anyway, because two may want the same plant.

1. **The dead** lie where they fell. The system sees the death within the animal's next look, and the
   corpse is removed `corpseDays` (15) game days later (`stats.cleared`). Hunting (N2) will carry
   corpses off and rot (E-tasks) will take over this timer.
2. **Downed** animals do nothing.
3. **Sleep.** Diurnal animals sleep 21:00 to 05:00, nocturnal ones (the wolf and the cougar, listed
   in the rules) 07:00 to 17:00, each animal's hours shifted 0 to 2 hours later so a herd doesn't
   drop off in the same second. A sleeper stands still (`Mover::hold`, set every step for sleeping
   and eating animals) and the snapshot says `asleep`. An injured animal doesn't sleep and wakes up;
   `AnimalSystem::wake` is for what scares it (N5).
4. **Eat.** Grazers (herbivores and omnivores) start eating below half their food capacity and eat on
   to 90%. Food is in nutrition, capacity is body size, and it falls at `race.foodPerDay` (RimWorld's
   1.6 a day x body size). The nearest plant within 10 m that is not a tree, has `stats.nutrition`,
   has no `plant.harvestedThingDef` (bushes and crops are the colony's) and is at least 10% grown is
   walked to (straight line only, so never across water or rock) and eaten in 8 s: the plant is
   removed (`PlantRemoved` event with message `grazed`), the animal gains nutrition x growth.
   Hungry animals with no plant near wander further. Wild predators get hungry and hunt since N5
   ([animal-fights.md](animal-fights.md)); other meat eaters never do; wild animals don't starve either, because plants don't spread yet, so grass would run
   out in 20 days and the herd with it. N3 starves tamed animals that aren't fed.
5. **Follow.** A herd member further than 4.5 m from the leader (or from where the leader is going),
   or 9 m from the leader at all, walks back to its side; near it, it shuffles a little.
6. **Wander.** A leader or a loner walks to a spot 4 to 14 m away (60% of decisions) or stands. Walks
   are at 60% of the def's speed.
7. **Edges.** The wild herd is counted every game hour. Under 90% of the target weight, with a 35%
   chance, a new group walks in at a random edge cell (`stats.arrived`) and won't leave for 2 hours.
   A wild animal within 8 m of an edge has a small chance at each decision (0.1%, four times that
   while the herd is over 115%) to walk off it (`stats.left`).

## Not here

Hunting and the corpse as an item (N2), taming, training, pens (N3), breeding, young growing up,
milk, wool, eggs (N4), animals drawn on screen
(U15; until then Unreal does not turn `wildAnimals` on, because the pawn drawing would show every
animal as a person). Animal temperature comfort, the weather and seasons (E tasks) and old age.

## Fighters and animals

Fighters never take a **calm** animal for an enemy on their own (`PawnInfo::animal`; an attack order on
one still works). One that hunts a pawn, fights back or is a manhunter (`Animal::temper`) is an enemy:
[animal-fights.md](animal-fights.md) (N5). The side number 100 is "wild". Code that counts "pawns of
another faction" as raiders must skip a calm `Animal` (check `registry.all_of<Animal>` and `temper`).

## Save and hash

Section `animals` version 1: the setting, whether the map was populated, the percent, the system's
random stream, the next herd id, whether the herd was over its density at the last count, the
counters, and the `Animal` part of every animal. The state hash covers the same (the system's part
only after `populate`, so games without animals hash as before).

## Cost

`simrun --seed 42 --size 1024 --days 1 --animals` (about 300 animals, a 1 km map): 0.36 ms per
step on the build machine for everything, against 0.004 ms with no animals. The animal system itself
is under 2% of that; **80% is `MovementSystem::movePawns`**, which sorts and buckets every pawn each
step even when it stands still. S9 (sim cost at Stage 2 size) should look there: skip sleeping and
idle pawns when no one is near. At 3x that is about 3% of one core.
`simrun --animals --animal-percent 300 --check` gives a bigger herd; `--check` fails if no animal was
placed, one stands on ground it can't walk, or the herd is under half its density.

## Tame animals

Taming, training, zones, pens, feeding and slaughter (N3) are in [husbandry.md](husbandry.md). A tame animal is still an `Animal`; its modes `WaitPath`, `GoToItem` and `EatItem` are used only by tame animals, and its walking and eating are in `AnimalCare.cpp`.
