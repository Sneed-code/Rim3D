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
| `SetTraining` | A tame animal is to learn Obedience, Release, Rescue or Haul. Wanting one wants what it needs first. |
| `CreateAnimalZone` | Paints an animal zone over the walkable cells of a rectangle that are in no animal zone. |
| `RemoveAnimalZoneCells` | Takes cells out of their zones; an emptied zone is deleted and its animals roam free again. |
| `AssignAnimalZone` | A tame animal stays in a zone (0: anywhere near where it was tamed). |

A command that can't be done fails with a reason in plain words (a `CommandFailed` event).

## Taming

- A colonist with the **Handling** work type does it (the `Tame` task). The scanner lists marked wild
  animals, nearest first; the job fetches hay or other food the animal eats, walks to the animal, works
  `interactSeconds` (389 s) and then rolls.
- Chance of one attempt: `(0.5 + 0.05 x Animals level) x (1 - wildness)`. Wildness is the animal def's
  (deer 0.7, muffalo 0.5, cow 0.25). A failed attempt keeps the mark and the colonist tries again;
  `AnimalAttemptFailed` is sent. (No manhunter on failure yet: N5.)
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
- **Obedience, Release and Rescue** only count steps in N3. What they do (come when called, go wild
  on command, carry a downed colonist to a bed) comes with the drafted-animal and rescue work later
  (BACKLOG).

## Zones and pens

- An animal zone is a set of ground cells (level 0), no cell in two zones. Zones are saved.
- A tame animal with a zone stays inside it: when it would wander out, it goes back, walking round walls
  by asking the pathfinder for a path (it can't be away from the zone for long). With no zone it stays
  within 40 m of its home.
- A zone is a **pen** (`Husbandry::isPen`) when every cell just outside it that is next to it is not
  walkable, apart from doors: nothing joined to the zone gets out. Doors count as walls for this test
  (animals can still walk through a door in play; BACKLOG).
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

- No names for tamed animals yet.
- A taming or feeding trip needs one stack with enough food.
- Failed taming does not make animals manhunters (N5).
- Animals walk through doors; only the pen test treats them as barriers.
- Mating, young, eggs, milk and wool are in [breeding.md](breeding.md) (N4); the animal zone a mother is in is the zone her young are born into.
