# Breeding and animal products (task N4)

How tame animals mate, carry and bear young, how hens lay and eggs hatch, how young animals grow up, and how
colonists milk and shear. Taming, pens and feeding are in [husbandry.md](husbandry.md); the life stages and the
wild herds are in [animal-life.md](animal-life.md). Code: `Breeding.h`, `Breeding.cpp` (the pass, young, eggs,
products), `HusbandryJobs.cpp` (the milking and shearing jobs). Tests: `Tests/BreedingTests.cpp`. Numbers:
`data/defs/Rules/BreedingRules.json` (each marked `unverified` where it is RimWorld's from memory, `ours` where we
made it up) and the animal defs (`gestationDays`, `litterSize`, `eggs`, `milk`, `wool`, `hatches`).

## Only tame animals

Wild animals do none of this: the wild herd is kept at the biome's density by animals walking in and out at the
edges (N1), and wild breeding on top would run away. A released animal stops with it.

## Sex

Every animal is male or female for good, worked out from the game's seed and its id (`Breeding::sexOf`, nothing
saved). About half and half. A colonist who wants calves needs a bull and a cow, as in RimWorld; with two animals
there is a one in two chance they are the same sex. The snapshot has it (`ThingView::sex`, 0 male, 1 female).

## The pass

One pass every `passSeconds` (60) game seconds goes over the tame animals in id order (nothing else on the map
costs anything). Per animal that is grown up it:

1. makes its `AnimalBreed` part (saved; made on the first pass after the animal is grown up and tame),
2. gives birth if its time has come (a pregnant female gives birth even if she is hungry),
3. starts or advances its egg, milk and wool timers,
4. is listed as a possible mate if it is fed (30% of its food), awake, unhurt, not held by a handler and past its
   mating wait.

Then each listed male rolls `passSeconds / (mateMeanHours x 3600)`. A male that comes up picks the nearest listed
female of his own kind **in his own animal zone** (or both outside any zone) within `mateSearchRadius` (40 m). Within
`mateRadius` (12 m) the two mate at once; further off he sets out for her (`AnimalSystem::goTo`, round walls if need be),
is `courting` her for `courtSeconds` (15 min), and mates at the first pass that finds him within 12 m of her. If she
is taken, pregnant, hungry or gone he drops it. Last, fertilized eggs that have reached their hatch time hatch. A mother's birth can therefore be up to a minute late; that is fine at this scale.

**Cost** (`simrun --seed 42 --size 128 --days 2 --farm 500`, release build): the breeding pass adds about 0.005 ms to the average step; a whole step with 500 chickens costs about 0.8 ms, most of it movement (N1 measured 80% in `MovementSystem::movePawns`, which S9 is to fix). CI runs the 100-chicken version as `simrun_farm`; `Tests/BreedingTests.cpp` has a 500-chicken test of the egg bookkeeping.

Random rolls come from a stream made from the seed, the animal and the second, so the order animals are visited in
changes nothing.

## Mating, pregnancy and litters

- A mating stands the two still for `mateHoldSeconds` (6, a handler's hold, so Unreal can play something) and sends
  `AnimalMated`. Both wait `mateCooldownHours` (12) before the next.
- Kinds with `gestationDays` (everything but hens): `mateChance` (50%) of the female being pregnant. She is
  born-ready at `now + gestationDays` and carries a litter drawn from the def's `litterSize` (min to max). She
  does not mate while pregnant.
- Hens (`eggs`, no gestation): a mating fertilizes her next egg (`AnimalBreed::fertilized`).
- A male courting a female whose zone changes meanwhile drops it at the next pass.

## Birth

`AnimalBorn` per young animal. The young are placed round the mother (1.5 m), are **tame** (side 1), belong in
her zone and share her home, and are babies: `bornAt` is now, so N1's life stages apply (body size, food use). The
mother waits `birthCooldownDays` (2) before mating again. If she dies while pregnant, the young are lost.

## Growing up

Nothing new is needed: a young animal's stage follows its age (baby, juvenile, adult; a game year is 60 days, so a
cow grows up in 36 days, a chicken in 6, a boar in 24). While not adult it does not mate, gives no milk, wool or
eggs, and cannot be a hauler (`AnimalSystem::mayWork`); it eats less (food factor). `isAdult` is the test.
Hit points grow with the stages too: every part has the stage's `healthScaleFactor` share of its adult hit points (baby 0.3, juvenile 0.6), set by `AnimalSystem` when the animal looks round (`Health::stageHp`, not saved).

## Switching breeding off

`SetBreeding` (an animal, or with no animal a zone id, and `allow`) stops mating: a switched-off animal, or one in a switched-off zone, is never listed as a mate. Pregnant females still give birth and hens still lay (unfertilized). The animal's flag is `AnimalBreed::breedingOff`, the zone's `AnimalZone::breedingOff`; save sections `breeding` and `husbandry` are version 2 for them.

## Eggs

- A tame adult hen (female, def `eggs`) lays `count` eggs every `intervalDays` (chicken: 1 in 1.5 days) where she
  stands, from the first pass that sees her grown up. Unfertilized eggs of one def join the stack in the cell
  (up to the stack limit), so a farm of hundreds adds few things; hauling brings them to the stockpile like any
  item.
- If she carries `fertilized`, the egg is the fertilized def (`EggChickenFertilized`, stack limit 1) and gets a
  `Hatching` part with its hatch time (`hatches.days`, 3.8). `EggLaid` has `message` "fertilized".
- A fertilized egg lying on the map hatches at that time into the egg's `hatches.into` (a tame chick in the zone it
  lies in, or with its home at the egg). One that is being carried waits until it is put down. `EggHatched`.
- A fertilized egg lying where the temperature is outside `hatchMinTemp`..`hatchMaxTemp` (0 to 50, ours) makes no progress: every pass pushes its hatch time on by the pass length. Nothing new is saved.

## Young animals follow their mother

A young animal born to a tame mother carries `Young { mother }` (saved, section `breeding` 3; hatched chicks have none). At each pass, while it is not grown up and its mother is on the map and on its level, it walks back to her (`AnimalSystem::goTo`) when it is further than `followRadius` (4 m) away. Once grown up it is left alone. Only one pass a minute, so it trails her loosely.

## Warm gestation and the hatchery

- A pregnant female whose cell is outside `gestationMinTemp`..`gestationMaxTemp` (-10 to 45 degrees, ours) makes no progress: each pass moves the birth on by the pass length.
- The **hatchery** (a building, 30 wood, def field `hatchery.warm`) keeps every fertilized egg within `hatcheryRadius` (2.5 m) on its level warm, so it hatches however cold or hot the room is. It needs no power. There is no job that carries eggs to it yet: eggs hatch where they lie, so a stockpile or the hens' pen has to be beside it.

## Milk and wool

- Milk (cows, muffalo): adult females only. Wool (muffalo, alpaca): adult of both sexes. `intervalDays` after the pass first sees the
  animal (or after the last gathering) the product is full (`fullness` 0 to 1 over the interval, exposed as
  `ThingView::milkFullness` and `woolFullness`, -1 for an animal that gives none).
- The **Milk** and **Shear** tasks of the Handling work type give a `GatherAnimal` job to a colonist when an animal
  is full (fullness 1): walk up to it (it stands still like for taming), work `milkWorkSeconds` (6.7) or
  `woolWorkSeconds` (28.3), and the product (`amount` of the def, 14 milk for a cow, 40 wool for a muffalo) is
  put down at the animal. Animals experience grows by `gatherXp`. `AnimalGathered`.
- A full animal stays full: time spent full is lost, as in RimWorld.
- Milk and wool are items like the others: they rot (milk), are food (milk) and are cloth (wool; the tailoring
  recipes come with the clothing work).

## Saves

Section `breeding` 1 (see [saves.md](saves.md)): the counters, then per animal its `AnimalBreed` part, and per
fertilized egg its `Hatching` part. Sex is not saved. The state hash covers both parts, and the counters only once
something has happened, so a game without tame animals hashes as before. The save check for jobs now accepts the
animal jobs (it used to refuse a save made while a colonist was taming).

## Bridge (for Unreal)

Snapshot `ThingView`: `sex`, `pregnant`, `milkFullness`, `woolFullness` (and the `lifeStage` and `bodySize` of N1, so a
calf draws small). Events: `AnimalMated` (`message` "pregnant" or "fertilized" or empty), `AnimalBorn` (`target` the
mother), `EggLaid`, `EggHatched`, `AnimalGathered` (`message` "milk" or "wool"). `PawnAction` `Handle` for the work.

## Limits

- Mating needs the two in the same zone (or both outside any); a pen or zone per kind is how to keep males and females apart.
- No separate "allow breeding" switch, no limit on the number of animals, no overcrowding. A pen only stays
  small because the player slaughters.
- An unfertilized or fertilized egg rots like any food (about five days outdoors), so a fertilized egg left unhauled still hatches at 3.8 days, but one hauled into a freezer-less stockpile is fine too.
- Animal products are not marked "do not sell" or ranked by quality (RimWorld has none).

## Products checked against the wiki (2026-10-26)

Milk: cow 14/day, yak 11/day, goat 12 per 3 days, caribou 10 per 2 days, dromedary 18 per 2 days (muffalo and muskox give none). Wool: alpaca and sheep 45 per 10 days, muffalo and muskox 120 per 15, with the wiki's insulation offsets. Eggs: chicken and duck daily, goose every 2 days (all three also lay unfertilized), turkey 1.333 days, ostrich 3.33 days; a species with no `unfertilizedThing` (turkey, ostrich) holds its timer at half until a male has mated her, then lays a fertilized egg. Egg nutrition 0.25 (chicken, duck), 0.5 (goose, turkey), 0.6 (ostrich); unfertilized eggs rot after 15 days. Hatch days other than the chicken's are ours.
