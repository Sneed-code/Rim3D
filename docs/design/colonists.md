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
2. **Backstories.** One childhood and one adulthood, uniformly, from the ones that fit the
   `category` asked for ("Colony" for settlers, "Raider" for pirates; a backstory without a
   `categories` list fits all). There are 28 childhoods and 44 adulthoods, all our own writing
   (each has a `description`). Their `workDisables` start the disabled work tags; their
   `disallowedTraits` keep traits out (a soldier is never a Wimp, a doctor never a Psychopath);
   their `forcedTraits` (`trait`, `degree`, `chance`) are tried before the random ones (a pit
   champion is often a Brawler). A forced trait that conflicts, repeats or needs disabled work is
   skipped, and forced traits count towards the one to three.
3. **Traits.** `traitCount` (1 to 3), each picked by `commonality`, one degree each. A trait is
   refused if it repeats, conflicts with a chosen trait (either direction) or needs a work tag
   (`requiredWorkTags`) that is disabled. A trait's `disabledWorkTags` join the list.
4. **Skills.** For each skill: a random base from `skillLevelRandomCurve` (the curve is a density
   over levels), or 0 to 4 for the skills a backstory mostly defines (`backstoryDefinedSkills`),
   plus the backstories' and traits' `skillGains`, then times a random factor between 1 and
   `ageSkillMaxFactorCurve` at the colonist's age (0.7 at 10, 1 at 35, 1.6 at 60: the old know
   more, the young a little less), clamped to 0 to 20. The three rules numbers are unverified. A skill is
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

### Gear by kind

The scenario's `colonistPawnKind` (a `PawnKinds` def, role `colonist`) says what a colonist lands
wearing: its `apparel` entries (`thing`, `stuff`, `chance`) and, if it has `weapons`, one of them
with `weaponChance`, each rolled from the colonist's own seed. Nudists wear nothing. Its
`backstoryCategory` picks the backstories. Raider kinds (`PirateKindBase`) carry
`backstoryCategory: "Raider"`, and the raid system makes its pirates with that category. A
scenario without a kind falls back on its `colonistApparel` list.

### Rerolling the starting colonists

`RerollColonistCommand{slot}` (before game second 0 only; later it fails) throws away the starting
colonist in that slot (the order they were made; slot 0 is the protagonist, who stays the
protagonist) and makes another in the same place, with their gear, from a fresh draw of the main
stream. The same seed and the same rerolls give the same colonists, and the command goes through
saves and replays by name. `simrun --start Crashlanded --reroll 1,3` shows it. A start screen with a reroll button per colonist and "reroll all" is not planned yet.

Colonists have no jobs yet: C1 gives them work. The scenario uses its own random streams, so it
does not change the sim's main stream.
