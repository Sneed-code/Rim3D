# Colonists from a seed

Code: `ColonySim/Colonist.h`, `Private/Colonist.cpp` (the generator), `Private/Scenario.cpp`
(`Sim::startScenario`). Tests: `Tests/ColonistTests.cpp`. Task C8. Data: `data/defs/Backstories`,
`Traits`, `Skills`, `Names`, `Rules` (`PawnGenRules`, `SkillRules`) and `Scenarios`
(`starting-data.md`).

## The colonist

`Colonist` is a component on a pawn thing: first name, nickname, last name, gender, age,
childhood and adulthood backstory, traits (def plus degree), skills (level, passion, experience,
whether disabled) and the sorted list of disabled work tags. Defs are stored by `defName`, so it
saves as it is (save format 4). `protagonist` marks the one colonist who gets 2x part hit points
(`HealthRules.protagonistPartHitPointsFactor`, used by F1) and the talent tree later.

## How one is made

`generateColonist(defs, seed, protagonist, out, error)` uses only its own random stream made from
`seed`, so a colonist depends on nothing else and changing one never changes another. In order:

1. **Gender, names, age.** Gender is a coin flip. Names are drawn from the male or female first
   names, last names and nicknames. Age is drawn from `ageGenerationCurve` between
   `adultAgeMin` (18) and the curve's end.
2. **Backstories.** One childhood and one adulthood, uniformly. Their `workDisables` start the
   disabled work tags.
3. **Traits.** `traitCount` (1 to 3), each picked by `commonality`, one degree each. A trait is
   refused if it repeats, conflicts with a chosen trait (either direction) or needs a work tag
   (`requiredWorkTags`) that is disabled. A trait's `disabledWorkTags` join the list.
4. **Skills.** For each skill: a random base from `skillLevelRandomCurve` (the curve is a density
   over levels), plus the backstories' and traits' `skillGains`, clamped to 0 to 20. A skill is
   `disabled` when a disabled tag is one of its `disablingWorkTags`; it keeps its level.
5. **Passions.** `passionCount` of them (0 to 4) go to the highest skills first, never to a
   disabled skill or one a trait forbids (`disallowedPassions`, e.g. Brawler and Shooting). Each
   is Major with chance `majorPassionChance`, else Minor.
6. **Experience.** Each skill starts with 10% to 90% of the XP needed for the next level
   (`SkillRules.xpToNextLevel`), nothing at level 20. This fraction is our own choice.

The draws use whole-number weights so Linux and Windows give the same colonists.

## The Crashlanded start

`SimSettings::scenario` names a `Scenarios` def; `Sim` runs it when it is made (`simrun --start
Crashlanded` also prints every colonist). Crashlanded makes the protagonist plus `colonists` (5)
more, standing 2 m apart on the walkable ground nearest the middle of the map, each made from the
map seed and their number. Then it drops `startingThings` right beside them,
`nearbyThings` a little further out and `scatteredThings` anywhere in about 45 m, as stacks (no
bigger than the item's `stackLimit`, one stack per cell). The guns and armour in `gearAddedByF2`
come with F2. A scenario or item def that does not exist is reported in `Sim::setupErrors()`.

Colonists have no jobs yet: C1 gives them work. The scenario uses its own random streams, so it
does not change the sim's main stream.
