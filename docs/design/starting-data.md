# Starting data

Hand-written defs in `data/defs/`, replacing task D3 (running the RimWorld importer on a local
install; see `docs/decisions/0003-hand-written-starting-data.md`). Written for C1, C2, C8 and F1.
Checked by `tools/check_defs.py` (also in CI) and by the "real data folder loads" test in
`Tests/DefsTests.cpp`.

## Where the numbers come from

- **RimWorld wiki** (rimworldwiki.com), numbers only. No descriptions, flavour text, textures or
  sounds are copied; short labels ("oak tree", "hard worker") are kept, as D2 would have.
- **`"unverified": [...]`** on a def lists fields whose numbers come from memory of the game, not
  from a page we could read. Check them against the wiki when a task leans on them; drop the field
  name from the list once checked.
- **`"source": "Rim3D"`** or **`"ours": [...]`** marks our own content and numbers: backstories,
  name lists, the scenario's colonist count, adult-only ages, passion counts. Backstories are
  original, written in RimWorld's style (gains of +1 to +7, a few disabled work tags).
- These are a base for a 3D game, not a copy to keep. Change any of them freely as we playtest,
  preferably through `data/tuning.json` multipliers for broad balance changes.

`source` is set on each file's abstract base def and inherited.

## Units (same rules as the D2 importer)

Our day is as long as RimWorld's in game time, so days, hours, per-day rates, percentages, counts
and metres carry over unchanged. Then:

| RimWorld | Here | Rule | Examples |
| --- | --- | --- | --- |
| Work and durations in ticks | game seconds | x 1.44 | `plant.harvestWork` (oak 1,400 ticks = 2,016), `plant.sowWork`, `stats.workToMake` |
| Combat timings in real seconds | game seconds | x 30 | Human fist `cooldownTime` 2 s = 60 |
| Speeds in cells per second | metres per game second | / 30 | `stats.moveSpeed` 4.6 c/s = 0.153333; trait move offsets |

Percentages are fractions (`fertility` 1.4 = 140%, `painPerSeverity` 0.0125 = 1.25% per HP).
Field and stat names follow the importer: RimWorld's field names, `stats` for `statBases`, stat
names in lowerCamel (`workSpeedGlobal`), keys that are def names keep their case.

## What is there

| Kind (folder) | Defs | For | Notes |
| --- | --- | --- | --- |
| `Terrain` | soil, rich soil, stony soil (`Gravel`), sand, marshy soil, mud, shallow and deep water, rough stone x5 | C2 | `fertility`, `pathCost` (walk speed = 13 / (13 + pathCost)), `affordances` |
| `Things` | `Items.json`: steel, wood, silver, component, medicine, berries, survival meal, simple meal, fine meal (berries carry `thingCategories` that recipes filter on). `Plants.json`: oak, poplar, grass, tall grass, bush, brambles, dandelions, berry bush. `Pawns.json`: Human | C1, C2, C8, F1 | Plants are `category: "Plant"` with a `plant` block. Steel and wood carry `stuffProps` for C4 |
| `Biomes` | temperate forest, boreal forest, arid shrubland | C2, D4 | `plantDensity` and `wildPlants` commonality (only the plants we have; newer expansion plants left out) |
| `Things` (buildings) | `Buildings.json`: wall, door, bed, stool, three tables, fueled stove | C4 | `costs`, `stuffCategories`, `costStuffCount`, `stats.workToBuild` (game seconds), `size`, `passability`, `building` flags C6 reads ([construction.md](construction.md)) |
| `Recipes` | simple meal, four simple meals, fine meal | C7 | `recipeUsers`, `workAmount` (game seconds), `ingredients` (item categories and nutrition or count), `products` ([bills.md](bills.md)) |
| `Weathers` | clear, fog, rain, foggy rain, dry and rainy thunderstorm, light and heavy snow | D4 (read by E1) | `durationHours` (range), `temperatureRange` (where it can happen), `skyGlow`, `moveSpeedFactor`, `accuracyFactor`, `sightFactor`, `rainRate`, `snowRate`, `extinguishesFire`; a biome's `weather` lists commonality per kind |
| `Rules` | `PlantRules`, `SkillRules`, `HealthRules`, `PawnGenRules`, `JobRules`, `NeedRules`, `ConstructionRules`, `MoodRules`, `CombatRules`, `DoorRules`, `RoomRules`, `BillRules` | C2, C8, F1, C1, C6, C4, C9, F2, C5, C7 | Numbers RimWorld keeps in its code rather than its defs (`JobRules`: [jobs.md](jobs.md); `NeedRules`: [needs.md](needs.md); `ConstructionRules`: [construction.md](construction.md); `CombatRules`: [combat.md](combat.md); `DoorRules`, `RoomRules`: [rooms.md](rooms.md)) |
| `Skills` | the 12 skills | C1, C8 | `disablingWorkTags` |
| `WorkTypes` | the 20 Core work types, in Work tab order | C1 | `relevantSkills`, `workTags`, `alwaysStartActive`, `inPrototype`, `tasks` (our job names, in order) |
| `Traits` | 27 traits, spectra as `degreeDatas` | C8 | `statOffsets`, `statFactors`, `skillGains`, `conflictingTraits`; mood numbers used by C9 ([mood.md](mood.md)) |
| `Backstories` | 14 childhood, 18 adulthood (ours) | C8 | `slot`, `skillGains`, `workDisables` (work tags) |
| `Names` | first names (two lists), last names, nicknames (ours) | C8 | `names` |
| `Scenarios` | Crashlanded | C8, F2 | RimWorld's starting items, with the rifle, revolver and flak gear since F2 (the knife waits for melee) |
| `Bodies` | Human | F1 | Part tree: `def`, `label`, `coverage`, `depth`, `groups`, `parts` |
| `BodyParts` | 32 part kinds | F1 | `hitPoints`, `bleedRate`, `solid`, `destroyableByDamage`, `lethalIfDestroyed`, scarring flags |
| `BodyPartGroups` | 12 groups | F1, apparel later | |
| `Capacities` | consciousness, moving, manipulation, sight, hearing, talking, eating, breathing, blood filtration, blood pumping, digestion | F1 | How parts and other capacities feed each one; downed and lethal thresholds |
| `Hediffs` | injuries (cut, stab, gunshot, scratch, bite, shredded, crush, bruise, crack, burn), missing part, blood loss | F1 | `injuryProps.painPerSeverity`, `bleedRate`, blood loss stages |
| `DamageTypes` | bullet, cut, stab, scratch, bite, blunt, crush, burn | F1, F2 | Which injury each damage makes (`hediff`, `hediffSolid` on bones); `reachesInside` (ours: bullet, cut, stab) lets a random hit go in to organs and bones; `armorCategory` (Sharp, Blunt, Heat) says which armor rating stops it |
| `Things` (F2) | `Weapons.json`: revolver, autopistol, bolt-action rifle, pump shotgun, assault rifle. `Apparel.json`: flak vest, flak pants, simple helmet (steel) | F2 | `verbs`, accuracy and cooldown stats, `recoilPerShot` (ours); armor ratings, `apparel.bodyPartGroups` and `layers`. See [combat.md](combat.md) |
| `Projectiles` | the five guns' bullets | F2 | damage, armor penetration, stopping power, speed (wiki m/s / 30; `tuning.json` multiplies it by 3) |
| `Hitboxes` | Human (ours) | F2 | Capsules and boxes per stance, by body part label; aim point and muzzle height |

## Rules worth knowing

**Mining and plant work (C3).** `WorkRules` has the speeds (`mining.speedBase` 0.04 +
`speedPerLevel` 0.12; `plants` 0.08 + 0.115), the pick (`hitDamage` 80, `hitSeconds` 144 =
RimWorld's 100 ticks), XP a second, the Mining/Plants yield curve by level (`yieldByLevel`) and
`growthYieldFloor`. Rock and ore hit points and drops are on the `Mineable` things in
`Things/Rocks.json`, chunks in `Items.json`. The ones the wiki doesn't give are in `unverified`.

**Plants (C2).** Plants grow only from 06:00 to 19:00 (`PlantRules.growHours`; the wiki's "with
rest" grow times are `growDays` x 24 / 13). Growth also needs light 0.51 or more. A plant's
lifespan is `growDays` x `lifespanDaysPerGrowDays` (9 for trees, 8 otherwise). Fertility
sensitivity: growth rate = 1 + (fertility - 1) x `fertilitySensitivity`, and nothing grows below
`fertilityMin`. After a berry harvest the bush goes back to 30% grown (`harvestAfterGrowth`).

**Skills (C8).** `SkillRules.xpToNextLevel[n]` is the XP from level n to n+1. Passions multiply
XP (0.35 / 1.0 / 1.5). Over 4,000 XP in one day per skill, gains count 20%. Levels 10 and up lose
`decayXpPerDay[level]`. A colonist's starting level per skill is a random base from
`PawnGenRules.skillLevelRandomCurve` (unverified) plus backstory and trait `skillGains`, clamped
to 0 to 20. A skill is disabled when a backstory or trait disables one of its `disablingWorkTags`.
Passions: `passionCount` and `majorPassionChance` are our own guesses (the wiki doesn't give
RimWorld's rule); give passions to the highest skills first.

**Capacities (F1).** Each capacity is a product of its `parts` factors, its `limbs` factor and its
`capacityFactors`:
- `average`: the mean efficiency (HP / max HP; 0 if missing or under a missing part) of all parts of
  that def. Both kidneys give `(L + R) / 2`.
- `importance` with `weight` w: `1 + (efficiency - 1) x w`. A missing stomach (w 0.5) halves
  digestion; a missing heart (w 1) zeroes blood pumping.
- `bestWeighted`: best part x `bestWeight` + the mean of the rest x (1 - `bestWeight`); one eye
  gives 75% sight.
- `limbs` (Moving, Manipulation): per leg or arm, the `core` part's efficiency x each of the
  `segments` x `(1 - digitWeight) + digitWeight x` the share of working `digit` parts under it (toes
  0.4, fingers 0.8); the capacity is the mean over the limbs. One lost leg is 50% moving. (F1
  replaced an earlier `digits` form, `perDigit` x digits + `base`, and the plain product over every
  limb part, which made one lost leg 6%.)
- `capacityFactors`: the other capacity's value (capped at `max` if given) through the same
  `importance` formula.
- Consciousness also multiplies by pain: `min(max, intercept + slope x pain)` = min(1, 1.05 -
  0.45 x pain).

Then hediff offsets (blood loss) are added and caps applied. Downed when consciousness is below
0.3, moving is 0.15 or less, or pain is over the pain shock threshold (0.8, from
`HealthRules`). Dead when a `lethalAtZero` capacity reaches 0, a `lethalIfDestroyed` part is
destroyed, or blood loss reaches 1.0.

**Pain and bleeding (F1).** Pain = sum over injuries of severity (HP lost) x `painPerSeverity`,
divided by the health scale. An injury bleeds severity x `injuryProps.bleedRate` x the part's
`bleedRate` per day; blood loss rises by the total per day and recovers 0.333 per day when the
total is under 0.1 (these three blood loss numbers are unverified). A lost part bleeds as if it
had lost 2 x its max HP. Natural healing is 8 HP per day plus the rest bonus (ground 4, bed 8),
plus 4 to 12 when tended, applied every 0.24 game hours as rate x 0.01 HP to one random wound.
The protagonist's parts have 2x hit points (`protagonistPartHitPointsFactor`). A lost part that
is not yet tended also hurts (its hit points x `painPerSeverity`, ours). An injury on an inside part
also injures the outside part over it (`innerInjuryAlsoHurtsOuter`, unverified). How a hit picks its
part, and the rest of the rules in code: [health.md](health.md).

## Adding data

Add or change a def in the right kind folder, using the conventions above. Take numbers from the
wiki where it has them; where it doesn't, write our own and list the field in `ours`, or list it
in `unverified` if it is recalled. If a new field names other defs, add it to `REFERENCES` in
`tools/check_defs.py`. Run `python3 tools/check_defs.py`.

**Farming, climate and weather (D4).** Crops are `Plant_Rice`, `Plant_Potato`, `Plant_Corn`,
`Plant_Strawberry`, `Plant_Haygrass`, `Plant_Cotton`, `Plant_Healroot`, `Plant_Devilstrand` in
`Things/Plants.json`, all from `CropBase` (`plant.isCrop`; growing temperatures: nothing grows below 0
or above 58 degrees, full speed between 6 and 42, `coldDamageBelow`, `heatDamageAbove`). Each has
`growDays`, `harvestYield`, `sowWork` and `harvestWork` (game seconds), `sowMinSkill`,
`fertilityMin` and `fertilitySensitivity` (C2's formula). What they yield is in
`Things/CropProducts.json` (rice, potatoes, corn, strawberries, hay, cloth, herbal medicine,
devilstrand). D6 and D7 add the stuff and tend numbers for cloth, devilstrand and herbal medicine;
whoever merges second drops a duplicate def. Each biome has `climate` (`seasonMeanTemperature` per
season of 15 days, `dailySwing` in degrees, `rainfallMmPerYear`, `growingSeasons`), `weather`
(commonality per `Weathers` def) and `animals` (commonality by RimWorld's animal names until D5
defines them). Only the temperate forest got wild healroot (0.06), so existing maps change a little.
Almost every crop and climate number is in `unverified`: the wiki was not read for this task, so
check the numbers a task leans on (C13 for crops, E1 for climate and weather).
