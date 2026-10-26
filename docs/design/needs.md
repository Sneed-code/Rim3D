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

## Rest

`Needs::rest`, 1 rested to 0. Awake it falls by band, per the wiki:

| Band | Rest | Falls at (a day) |
| --- | --- | --- |
| Rested | 0.28 and up | 0.95 |
| Drowsy | 0.14 to 0.28 | 0.66 |
| Tired | 0.01 to 0.14 | 0.28 |
| Exhausted | under 0.01 | 0.57 |

Lying down it **rises** 2.2857 a day (0.5714% per 150 ticks) times the **rest effectiveness**:
the bed def's `stats.bedRestEffectiveness` (bed 1.0), 0.8 on the ground (`groundEffectiveness`).
So 0 to 1 takes 10.5 hours in a bed. While lying, `Health::rest` is set to Bed or Ground for the
healing bonus (F1, [health.md](health.md)) and goes back to Ground when the pawn gets up.

**The sleep job** (`LayDown`): walk to the bed's cell (or none: lie where the pawn stands), then
lie. It wakes when rest is back to 1.0 (`wakeAt`); in an hour that is not a Sleep hour it wakes at
0.7 (`wakeOutsideSleepAt`, ours); and a ravenous pawn (under 0.125) wakes to eat if there is
anything to eat. RimWorld's own wake-up rule is not on the wiki, so the three levels above are ours
(`unverified` in the def).

## The schedule

`Schedule`: one `ScheduleKind` for each hour of the day. `Anything` and `Work` are for working,
`Joy` keeps the pawn from working (there is no recreation yet, so it idles), `Sleep` sends it to
bed. The standard schedule is Sleep from 22:00 through 05:59 and Anything the rest (RimWorld's
default is recalled, not read; `schedule` in NeedRules). `SetScheduleCommand{pawn, hour, kind}`.

When a pawn goes to bed (ours, all in `NeedRules`):

| Hour is | The pawn sleeps when rest is under |
| --- | --- |
| Sleep | 0.75 (`fallAsleepBelow`) |
| Anything or Joy | 0.28 (Drowsy) |
| Work | 0.14 (Tired) |

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

Order in `JobSystem::step`: **needs** (eat when food is under 0.3, sleep when drowsy or tired by
the table above), **schedule** (a Sleep hour sends a pawn under 0.75 to bed; a Joy hour idles
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

## Not yet

- Overeating, food poisoning,
  rotting, food policies, baby and animal food.
- Malnutrition's effect on consciousness; hunger and tiredness slowing work.
- Plants to harvest (C3), medical beds, beds for
  guests and prisoners, sleeping spots, bedrolls, bed quality.
- Meditation. (Recreation for Joy hours is C18: [recreation.md](recreation.md); the standard schedule now has Joy from 19:00 to 21:59.)
- Food kept in a pawn's inventory; pawns feeding the downed.
