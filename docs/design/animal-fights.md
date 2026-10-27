# Animal fights (predators, prey, manhunters)

Code: `ColonySim/Animals.h`, `Private/AnimalFights.cpp` (the `AnimalSystem`'s fight half). Tests:
`Tests/AnimalFightTests.cpp`. Rules: `data/defs/Rules/AnimalRules.json` (all ours except
`meatNutrition`). Stage 2 task N5. It stands on animal life ([animal-life.md](animal-life.md)), melee
([melee.md](melee.md)) and the fighter AI ([fighters.md](fighters.md)).

## What changed in the rest of the sim

- **Animals have hitboxes** (`Hitboxes/AnimalHitboxes.json`, ours): five four-legged sizes (tiny to
  huge) and one bird, each with standing, crouching and lying shapes, picked by the animal def's
  `race.hitboxes`. Before this a bullet flew through an animal. The shapes are not scaled for babies.
- **An animal's teeth and claws** reach `animalReachBase` + `animalReachPerRoot` x the root of its body
  size from its centre (wolf 1.03 m, deer 1.09, bear 1.3; `MeleeRules`, ours). Pawns keep about a metre
  apart, so the human 0.85 m would never touch.
- **Temper.** `Animal::temper` is `Calm`, `Hunting` (a predator after a pawn), `Revenge` (fighting back
  at whoever hurt it) or `Manhunter`. Anything but Calm is an **enemy to the fighter AI** and to the
  colony alarm: drafted colonists and called-up defenders shoot it and the alarm sounds. Calm animals
  are still nobody's enemy. `AnimalSystem::hostile`, and the snapshot's `animalTemper` and `animalMode`.
- **Combat tells the animals.** `CombatSystem::setHurtHook` is called when a bullet or a blow hurts a
  pawn; `AnimalSystem::hurt(victim, attacker)` answers it at once. After each step `Sim` gives the
  step's events to `AnimalSystem::scare`.

## Hurt

`hurt` wakes the animal, and its herd mates within `herdAlert` (20 m) that are calm and not predators
run from the attacker. Then, by who hurt it:

- **An animal** (a predator's bite): prey runs. A predator bitten by another does not.
- **A tame animal** hurt by a pawn: nothing, it never turns on its people.
- **A pawn**, wild animal: it **turns manhunter** with a chance by kind: `manhunterChanceFlagged` 0.5
  for `race.manhunterOnDamage` (the boar), `manhunterChancePredator` 0.2, `manhunterChanceOther` 0.02.
  Otherwise a predator or a tusker takes **revenge**: it chases the one who hurt it for
  `revengeSteps` (300) and stops sooner if that pawn goes down or dies. Anything else **runs**.
- A manhunter stays one for `manhunterSteps` (7200, two game hours). It picks the nearest colonist it
  can see within `manhunterSight` (45 m), able ones before downed, and keeps at it until the pawn is
  dead (it does not stop at a downed pawn). With nobody in sight it goes about its day, looking again
  every few steps. It walks straight lines like every animal (N1), so a colonist behind walls is safe
  until pathing for animals arrives.
- `AnimalSystem::enrage(animal, temper, target)` sets any of this by hand. **T3 (manhunter pack, mad
  animal) uses it**, and a failed taming (N3) can too.

## Running from a fight

Calm animals within `scareRadius` (22 m) of a **shot, a blow that hit a pawn, or an explosion** run
(`stats.scares`): 20 m straight away, trying turns of 45 degrees up to the way back if a wall is in
the way, at full speed for at most `fleeSteps` (240). Predators, animals a handler holds, hauling
animals and animals already fighting or running are left out. Tame animals run too and walk home after.

## Predators

A wild predator (`race.predator`: wolf, bear, cougar) now gets hungry like a grazer. Under
`predatorHungryBelow` (40% of its food) and not before `huntAfter`, at its next decision, it looks
within `huntRadius` (35 m) for:

- **Prey animals**: not predators, wild or tame, up to `preyMaxRatio` (1.5, ours) x the predator's
  body size, with a straight way to them. A wolf (0.9) takes deer, boar, alpaca and hare; a bear (1.8)
  takes muffalo.
- **Lone colonists**: a player pawn of such a size with no other able colonist within `loneRadius`
  (10 m). Hunting a colonist sets the temper to `Hunting`, hunting an animal leaves it Calm (the
  colony does not care).

The nearest is chased at full speed. The prey runs from it (80% of its top speed, `preyFleeSpeed`)
and its herd with it. When the predator is in reach, **the prey freezes in the jaws** (`Seized`,
90 steps, renewed while the predator keeps at it): a fleeing target would otherwise always dodge the
strike. The predator bites through melee (F8: wind-up, strike line against the new hitboxes, armor,
health). A chase that has not caught its prey within `huntChaseSteps` (1200) or has lost it by 60 m is
dropped and the predator waits `huntCooldown` (900) before looking again, or `huntLook` (300) if there
was nothing to hunt.

A kill (`stats.kills`) is walked to and eaten for `gorgeSteps` (240): the corpse is removed and the
predator gains the meat as nutrition (`race.meatAmount` x 0.05, RimWorld's raw meat). Wild animals
still do not starve, so a hungry predator hunts again and again.

## Numbers

Ours, to tune in `AnimalRules.json`; every one except `meatNutrition` is unverified. On a 1 km map
with 280 animals (33 wolves from the biome's list) a game day has about 100 hunts, 16 kills and a few
scares per animal; a 512 m map for two days: 13 hunts, 4 kills. The fight code costs under 0.02
ms a step more (`simrun --animals`).

## Save and hash

Section `animals` version 2: the animal part gains `temper`, `enemy`, `temperUntil`, `huntAfter` and the
stats gain `manhunters`, `revenges`, `hunts`, `kills`, `meals`, `scares`. Version 1 saves load with the
defaults. The hash covers the new fields only when one is set, so games without fights hash as before.
New `AnimalMode`s at the end: `Flee`, `Chase`, `Fight`, `Feast`, `Gorge`, `Seized`.

## Not here

Packs (wolves hunt alone), animals breaking doors, pathing round walls for chasers, a manhunter
choosing a colony to walk to, manhunters from failed taming (N3 hook is `enrage`), sound of fights
reaching farther than 22 m, animals fleeing fire, shooting a hunting animal making the whole herd
manhunter, the Unreal side (U15 draws animals; U22 swings and bites). `simrun --animals` prints the
fight counters.
