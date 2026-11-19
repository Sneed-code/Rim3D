# Husbandry: taming, training, pens, feeding and slaughter (task N3)

How wild animals become the colony's, what a tame animal learns and eats, and where it stays. The wild
side (herds, grazing, sleep) is in [animal-life.md](animal-life.md). Code: `Husbandry.h`, `Husbandry.cpp`
(commands, zones, rules), `HusbandryJobs.cpp` (the Handling work and its jobs), `AnimalCare.cpp` (what a
tame animal does by itself). Tests: `Tests/HusbandryTests.cpp`. Numbers: `data/defs/Rules/HusbandryRules.json`
(each is marked `unverified` where it is RimWorld's number from memory or `ours` where we made it up).

## The player's side (commands)

| Command | What it does |
| --- | --- |
| `MarkAnimal` `Tame` | A wild animal is marked to be tamed. `on` false takes the mark off. |
| `MarkAnimal` `Slaughter` | A tame animal is marked to be killed. |
| `MarkAnimal` `Hunt` | A wild animal is marked to be shot by a colonist with a gun and the Hunting work ([hunting.md](hunting.md), N2). |
| `MarkAnimal` `Release` | A tame animal goes wild at once. |
| `MarkAnimalsInArea` | Marks every animal in a rectangle that can take the mark (`Tame` or `Hunt`: wild ones; `Slaughter`: tame ones): the "tame all nearby" button. Fails if none. |
| `SetAnimalName` | Names a tame animal (24 characters at most, empty takes it off). Saved on the animal (section `husbandry` 3) and in the snapshot as `animalName`. |
| `SetTraining` | A tame animal is to learn Obedience, Release, Rescue or Haul. Wanting one wants what it needs first. |
| `CreateAnimalZone` | Paints an animal zone over the walkable cells of a rectangle that are in no animal zone. |
| `RemoveAnimalZoneCells` | Takes cells out of their zones; an emptied zone is deleted and its animals roam free again. |
| `AssignAnimalZone` | A tame animal stays in a zone (0: anywhere near where it was tamed). |
| `SetBreeding` | Breeding on or off for one tame animal, or for every animal in a zone ([breeding.md](breeding.md)). |

A command that can't be done fails with a reason in plain words (a `CommandFailed` event).

## Taming

- A colonist with the **Handling** work type does it (the `Tame` task). The scanner lists marked wild
  animals, nearest first; the job fetches hay or other food the animal eats, walks to the animal, works
  `interactSeconds` (389 s) and then rolls.
- Chance of one attempt: `(0.5 + 0.05 x Animals level) x (1 - wildness)`. Wildness is the animal def's
  (deer 0.7, muffalo 0.5, cow 0.25). A failed attempt keeps the mark and the colonist tries again;
  `AnimalAttemptFailed` is sent, and some animals turn on the handler (Revenge on a failed taming, below).
- The animal is fed `0.15 x body size` nutrition by the attempt, from one stack of enough food.
- On success: the animal's side is the player's (`Faction` 1), `Animal::wild` is false, its home is
  where it stands, the mark is gone and `AnimalTamed` is sent. A tame animal that has wandered can't be
  tamed twice.
- Experience goes to the Animals skill while the colonist works.
- The animal stands still while a handler is within 6 m of it on the way (`hold`), so the walk ends.

## Training

- Four tricks, in this order in the rules: Obedience (Simple, 10 steps), Release (Intermediate, 5 steps,
  needs Obedience), Rescue (Intermediate, 20 steps, needs Obedience), Haul (Advanced, 30 steps, needs
  Obedience). An animal learns a trick only if its `race.trainability` is at least the trick's.
- The `Train` task picks a tame animal with a wanted trick it can practise now and whose wait is over.
  One attempt per 6 hours (`attemptIntervalSeconds`). Chance: `(0.5 + 0.05 x skill) x (1.5 - wildness)`.
- Unfinished training fades: a step is lost every 6 days (`decayDays`) after the last attempt. A
  learned trick stays.
- **Haul** makes the animal a worker of the job system with only the Hauling work type, so it carries
  things to stockpiles while it is awake, over 30% fed and unhurt (`AnimalSystem::mayWork`). Releasing it,
  or its death, takes the worker off again.
- **Rescue** makes the animal a worker with only the Doctor work type, and for an animal that means the
  Rescue task alone: it lifts a downed colonist no bed holds and carries it to a free bed, like a doctor
  does. It never tends (`TendPatientsGiver` skips animals). It works under the same conditions as a hauler
  (`AnimalSystem::mayWork`); an animal with both Haul and Rescue does both.
- **Obedience**: while any colonist is drafted, the animal keeps within 4 m of the nearest one (within 40 m
  of it; numbers ours), over its zone and food walks except when hungry. Undrafted, it returns to its own
  life. No master is stored, so there is no new save data.
- **Release** only counts steps: the `MarkAnimal` `Release` command (go wild) works on any tame animal
  already. "Come when called" is not done.

## Zones and pens

- An animal zone is a set of ground cells (level 0), no cell in two zones. Zones are saved.
- A tame animal with a zone stays inside it: when it would wander out, it goes back, walking round walls
  by asking the pathfinder for a path (it can't be away from the zone for long). With no zone it stays
  within 40 m of its home.
- A zone is a **pen** (`Husbandry::isPen`) when every cell just outside it that is next to it is not
  walkable, apart from doors, fences and fence gates: nothing joined to the zone gets out. Doors and fences count as walls for this test,
  and they are walls to animals: an animal's walk never crosses a door or a fence and a path through one is dropped (RimWorld's animals can't open doors).

## Fences

`Fence` and `FenceGate` (Structure, `Things/Husbandry.json`, wiki numbers: a fence is 1 stuff, 40 HP, path cost 80, cover 25%; a
gate 25 stuff, 20 HP) carry `building.fence`. `Map::isFence` marks their cells (kept with the building layer, from the def, so a loaded
game has them too) and `Map::blocksAnimals` is a door or a fence. People walk over a fence (slowly) and through a gate; no animal
crosses either, wild or tame, and a fleeing animal turns away from them. Only an animal charging its enemy (`Chase`) still goes
through, as RimWorld's manhunters break through fences. An animal zone never takes a fence cell, so a zone painted over a fence
ring keeps its inside, and a ring of fence with a gate makes a pen. A tame animal assigned to a pen that is outside it may walk in
through a gate (`Map::isFenceGate`); once inside the gate is shut to it again.

## Animal beds

`AnimalSleepingSpot`, `AnimalSleepingBox` (fits body size 0.55 and under; from memory) and `AnimalBed` (40 fabric or leather,
fits any animal; wiki) carry `building.animalBed`; people never sleep in them. At bedtime a tame animal takes the nearest
animal bed that fits it, stands in its zone (or within `homeRadius` of home) and no other living animal has
(`Husbandry::bedFor`, kept in `AnimalCare::bed` and saved), walks there and sleeps on it. A hurt tame animal with a bed goes
and rests on it by day as well. While lying on it the animal heals as in a bed (`Health::rest` is `Bed`: RimWorld's rest heal
bonus); on the ground it heals as before. No bed, or no way there: it sleeps where it is, as before. Saved in section
`husbandry` version 4 (`bed`).

## Revenge on a failed taming

`race.revengeOnTameFail` (the wiki's "manhunter chance (taming)": 30% for wolves, bears, the cougar, the rhinoceros and the
megasloth, 10% for the boomalope and the ostrich, 0 for farm animals; ours for the lion, raptor and tyrannosaurus) is rolled
after every failed attempt: the animal turns on the handler (`AnimalTemper::Revenge`) for a short time.
- Animals in a pen eat hay and kibble that lies in it (and hand-feeding reaches them there).

## Feeding

- A tame animal's food runs down like a wild one's. When it is hungry it looks (within 12 m of its
  zone or home) for something its diet eats: grazing plants (herbivores and omnivores), hay (`Plant`
  food type), kibble and raw meat (`Kibble`, `Meat`). It walks to the item and eats from the stack.
- Under 25% food and nothing found: a Handling colonist hand-feeds it (the `FeedAnimals` task; 20 s).
- With no food at all for 5 days (`starveDays`) the animal dies: `AnimalStarved`.
- Kibble is made at a stove from meat and plants: recipe `MakeKibble` (data in `Things/AnimalFeed.json`,
  `Recipes/AnimalFeed.json`).

## Slaughter

A Handling colonist walks to a tame animal marked `Slaughter`, works 259 s and kills it
(`AnimalSlaughtered`). The corpse is marked to be collected: haulers carry it to a stockpile and the butcher table cuts it up
for meat and leather ([hunting.md](hunting.md), N2).

## Saves

Section `husbandry` 1 (see [saves.md](saves.md)): next zone id, the zones with their cells, the counters,
and every animal's `AnimalCare` part (marks, zone, home, timers, training steps and wants, the walk it is
waiting on). The state hash only includes animal care for animals that have it, so a game that never
touches animals hashes as before. Replays carry the five commands.

## Bridge (for Unreal)

Snapshot `ThingView` has `markTame`, `markSlaughter`, `animalZone`, `learnedTricks` and `wantedTricks`
(a bit per trick: Obedience 1, Release 2, Rescue 4, Haul 8). Events: `AnimalTamed`, `AnimalReleased`,
`AnimalTrained` (`amount` steps now, `part` steps needed), `AnimalAttemptFailed`, `AnimalSlaughtered`,
`AnimalStarved`; `PawnAction` `Handle` and `Slaughter`.

## Limits

- A taming or feeding trip needs one stack with enough food.
- No handler walks an animal into its pen (RimWorld's roping): instead a tame animal outside its own zone may path through a
  fence gate (not a door) to get back in; from inside, the gate is shut to it.
- Mating, young, eggs, milk and wool are in [breeding.md](breeding.md) (N4); the animal zone a mother is in is the zone her young are born into.
