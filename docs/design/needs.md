# Food, rest, schedules and beds

Code: `ColonySim/Needs.h`, `Private/Needs.cpp` (the numbers and the per-second upkeep),
`Private/NeedJobs.cpp` (eating, sleeping and the think nodes, part of the job system). Tests:
`Tests/NeedsTests.cpp`, `simrun --start Crashlanded --days 3 --check` (`simrun_needs`). Task C6.

A colonist has two needs, **food** and **rest**, each 1 (full) down to 0. When food runs low the
pawn eats; when rest runs low, or the hourly **schedule** says so, it sleeps in its **bed**, else on
the ground. Eating and sleeping are jobs on the C1 job system ([jobs.md](jobs.md)). Mood effects
(ate without a table, slept on the ground) are C9 and read the events below ([mood.md](mood.md)).

Pawns get needs only when the data has a `NeedRules` def (`data/defs/Rules/Rules.json`). Without
one (the test fixture defs) pawns have none. The hauling test's pawns (`SimSettings::pawns`) never
have needs: they are a benchmark that hauls all day.

## Food

`Needs::food`, a full stomach is 1.0 (RimWorld's saturation). It falls by `race.foodPerDay`
(Human 1.6) a day, slower the hungrier the pawn is, as the wiki's table says:

| Band | Food | Falls at |
| --- | --- | --- |
| Fed | 0.25 and up | 1.6 a day (6.7% an hour) |
| Hungry | 0.125 to 0.25 | half that |
| Ravenous | above 0 to 0.125 | a quarter |
| Malnourished | 0 | doesn't fall; **malnutrition** builds |

A pawn looks for food when food is under **0.3** (`seekBelow`). At 0, `Needs::malnutrition` rises
0.48 a day (2% an hour) and the pawn dies of starvation (`DeathCause::Starvation`) at 1.0; it
falls at the same rate once the pawn has eaten. (Malnutrition's consciousness penalty is not built.)

**Choosing food** (`JobSystem::findFood`): every food item on the pawn's level that it can reach
and nobody holds. The **tastiest kind wins, the nearest of that kind** (ours: RimWorld weighs
distance against taste with a formula we haven't read). Kinds come from the item def's
`ingestible.preferability`, worst to best: `DesperateOnly` (only when ravenous), `RawBad`,
`RawTasty` (berries), `MealAwful`, `MealSimple` (survival meal), `MealFine`, `MealLavish`;
`NeverForNutrition` is not food. Food needs `stats.nutrition` and an `ingestible` block.

**The eat job** (`Ingest`): go to the food, pick up as many as fill the stomach (at most
`ingestible.maxNumToIngestAtOnce`, 1 for meals, 20 by default), go to the eating spot, eat. One
sitting takes `ingestible.baseIngestSeconds` (default 720 game seconds, RimWorld's 500 ticks) for
the whole handful, then the nutrition is added (the stomach caps at 1; there is no overeating
penalty yet) and the food is used up. What is left of a stack stays where it lay.

**The eating spot:** a **chair next to a table**: a building with `building.isSittable` that has a
building with `building.isTable` in one of its four neighbouring cells, within 40 m of the food
(`eatSpotRadius`), that the pawn can reach and nobody else holds (the chair's cell is reserved).
The nearest to the food wins. With none the pawn eats where the food lay, standing. The
`PawnAte` event says which (`message` "table" or "ground") for C9.

**Better food fills more** (sneed, 2026-11-16; ours, RimWorld has no such rule): a food's nutrition is its
def's `stats.nutrition` times NeedRules `food.nutritionFactor` for its preferability: simple meals 1.1, fine
meals 1.25, lavish meals 1.5, everything else 1. So a simple meal (0.9) fills 0.99, a fine meal 1.125 (a full
stomach from empty), a lavish meal 1.35. It counts for everyone who eats: colonists, prisoners, caravans.

**The protagonist eats from the bag** (sneed, 2026-11-16; `JobSystem::autoEatFromBag`): the main character
eats by itself from its own bag (`Gear::inventory`, [inventory.md](inventory.md)) as soon as food falls under
the seek level (0.3), before it is hungry (0.25), whether the player is steering or not. It picks the best kind
in the bag (the tiers above, tastiest first), the most filling of that kind next, eats as many as fill it (up
to `maxNumToIngestAtOnce`, so one meal) **at once, on the spot**, with no job and no time taken, and the rest
stays in the bag. Food for desperate times (chocolate) only when ravenous; drugs never; not while down. The
`PawnAte` event says `message` "bag". With nothing edible in the bag it goes looking for food as before (in
the overseer view; steered, the player feeds it). Other colonists don't eat from their bags yet.

**Starving** (sneed, 2026-11-16, "for now"; ours): a pawn whose stomach is empty (food 0, malnutrition
building) moves, attacks (aiming, cooldowns, reloads, swings) and works at **75%** (NeedRules
`food.starvingSpeedFactor` 0.75), on top of the weight slowdowns (`ApparelStats::hungerFactor`,
[armour.md](armour.md) "Weight"). It is gone the moment it eats. The snapshot has `starving` and
`moveFactor` (every walk slowdown together), and the player's own walk in direct mode follows `moveFactor`, so
a starving protagonist walks slower too (as do load and worn weight, which it ignored before).

## Rest

**sneed's rules (2026-11-16), replacing RimWorld's rest bands.** `Needs::rest`, 1 rested to 0.

- **Awake** it falls 0.6 a day whatever the level: a 16-hour waking day uses **40%**, so a pawn that woke at 100% has
  60% left at bedtime.
- **Lying down** it rises 1.5 a day times the bed's `stats.bedRestEffectiveness` (bed 1.0, the ground 0.8): a regular
  8-hour night in a bed gives back **50%** (40% on the ground), more than a day uses, so a pawn that lost sleep catches up
  over a few nights.
- **Nothing happens until 0.** The Drowsy and Tired bands are gone (`drowsyBelow`, `tiredBelow` and `exhaustedBelow` all
  0.001). At 0 the pawn is **Exhausted**: the Exhausted mood (-18) and **25% less performance**
  (`rest.exhaustedPerformance` 0.75, `NeedsSystem::performanceFactor`): work goes at 75% (through `JobSystem::workFactor`),
  aiming, cooldowns, reloads and melee swings take 1/0.75 times as long (`CombatSystem::attackTimeFactor`), and shooting
  accuracy is multiplied by 0.75. Walking speed is not touched. This leaves room for long fights and emergencies.
- **The default is still to sleep every night.** In a Sleep hour a pawn goes to bed below 0.85 (`fallAsleepBelow`; after
  any normal day it is well under) and gets up once full (`wakeAt` 1.0): from 60% that is about 4:30. At 6:00 a pawn under
  50% sleeps on until 50% (`wakeOutsideSleepAt` 0.5). `sleepThroughNight` true would keep everyone in bed until the Sleep
  hours end; tried and dropped, because the year colony lost too many work hours (20 alive of six runs instead of 37). Outside Sleep hours a pawn naps only below 10%
  (`napBelow`; in Work hours only at 0, `napBelowWork`) and gets up at 50%.

While lying, `Health::rest` is set to Bed or Ground for the healing bonus (F1, [health.md](health.md)) and goes back to
Ground when the pawn gets up. Every number above is ours (sneed's), in `NeedRules` `rest` with a `restNote`.

**The sleep job** (`LayDown`): walk to the bed's cell (or none: lie where the pawn stands), then lie. A fire beside the
bed wakes it, and a ravenous pawn (under 0.125) wakes to eat if there is anything to eat.

## The protagonist sleeps the night away

The protagonist has **no rest need** (sneed, 2026-11-16): rest is held at 1, never Exhausted, no sleep jobs from needs or
the schedule (a Sleep hour is a free hour), and the snapshot gives -1 for their rest. Sleeping only passes the night, as in
Palworld, so nights never force a stop and night missions are no chore.

`SleepCommand{pawn, bed, wake}`: the colonist under control lies down in `bed` (within reach, `kHandsOnReach`; its own or
one with a free slot, which it then owns) or with `kNoEntity` on the ground where it stands, as a hands-on `LayDown` job
(`JobSystem::handsOnSleep`), and the night is skipped to morning like `SkipNightCommand` ([skip-travel.md](skip-travel.md)):
the sim runs 1,200 game seconds a step with the narrator held, everyone else carrying on as normal
(`TravelSystem::startPlayerNight`). The colonist stays the player's. It may start from 18:00 (`rest.nightFromHour`) and
ends at 6:00 (`rest.nightToHour`); it is refused in daytime, for a pawn not under control or downed, with a bed out of
reach or full, and while anything threatens the colony (the skip's own threat check). A threat that turns up while asleep
ends the skip and the player gets up; so does a fire beside the bed. `wake` true gets up now and ends the skip. While asleep,
`DirectMoveCommand` is ignored. `planHandsOn` on a bed gives `HandsOnKind::Sleep` ("sleep till morning", or why not).

Unreal (next PR, waiting for a PC compile): in direct mode **F** on a bed (a press, not a hold) or **N** anywhere sleeps; the screen fades to dark with the clock
running; **W A S D, N or Esc** get up; Tab gets up first. Any other colonist under control can sleep the same way (and rests).

## Beds

A building def with `building.bed: true` gets a `Bed` component: its owners, at most
`building.sleepingSlots` (default 1); slot *i* is the *i*-th cell of the footprint. A pawn owns at
most one bed. A sleeping pawn goes to **its own bed** if it can reach it; with none it takes the
**nearest free bed slot** and owns it from then on (RimWorld's colonists claim a bed by sleeping
in it); with none free it sleeps on the ground. The bed's cell is reserved while the pawn uses it.
`AssignBedCommand{bed, pawn}` gives a pawn a bed (taking it from its old one); pawn `kNoEntity`
empties the bed. Destroying a bed ends the sleeper's job; it then looks again. Bed defs belong to
C4: `building: {bed, sleepingSlots}`, `stats.bedRestEffectiveness`, and for the table `building:
{isTable}`, for the chair `building: {isSittable}`.

## In the think list

Order in `JobSystem::step`: **needs** (eat when food is under 0.3, nap by
the table above), **schedule** (a Sleep hour sends a pawn under 0.85 to bed; a Joy hour idles
it), then work, then idle. A pawn on a job it may drop (hauling, waiting, not a player order) is
checked **every step**: if it has become hungry enough to eat, or tired enough to sleep this hour,
the job ends ("interrupted: hungry" / "interrupted: tired") and the need job starts. Eating and
sleeping jobs are never interrupted by anything but a downing or death. A pawn that finds no food
looks again a minute later (`Needs::foodSearchAfter`).

## What Unreal sees

- **Snapshot:** a pawn with needs has `food`, `rest` (else -1) and `asleep` (lying down).
- **Commands:** `SetScheduleCommand`, `AssignBedCommand` ([bridge.md](bridge.md)).
- **Events:** `PawnAction` `Eat` (target the food), `LieDown` (target the bed or none), `GetUp`;
  `PawnAte` (`def` the food, `amount` the nutrition gained, `message` "table" or "ground");
  `PawnWokeUp` (`target` the bed or none, `message` "bed" or "ground", `amount` the rest).
- `JobType::Ingest` and `JobType::LayDown` in `JobStarted`/`JobEnded` and the snapshot.

## Save format 8

`Needs`, `Schedule` and `Bed` are saved per thing; a job also saves its `chair`. Saves of format 6 and 7
still load (their pawns have no needs). Everything is in the state hash.

A pawn the player controls directly (U9) gets no jobs, so it does not eat or sleep by itself;
its food and rest still fall. Let go of it to have it look after itself.

## Trait effects (Traits defs, `degreeDatas`)

`NeedsSystem::step` reads two trait stats: `statFactors.hungerRateMultiplier` (Gourmand 1.5, food falls half as fast
again) and `statOffsets.restRateMultiplier` (Quick sleeper +0.5, rest rises half as fast again while lying down). Night
owl and greedy colonists have mood effects, see [mood.md](mood.md).

## Not yet

- Overeating, food poisoning,
  rotting, food policies, baby and animal food.
- Malnutrition's effect on consciousness; hunger slowing work.
- Plants to harvest (C3), medical beds, beds for
  guests and prisoners, sleeping spots, bedrolls, bed quality.
- Meditation. (Recreation for Joy hours is C18: [recreation.md](recreation.md); the standard schedule now has Joy from 19:00 to 21:59.)
- Food kept in a pawn's inventory; pawns feeding the downed.
