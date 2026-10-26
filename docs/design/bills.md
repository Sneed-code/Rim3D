# Bills and cooking

Code: `ColonySim/Bills.h`, `ColonySim/BillMode.h`, `Private/Bills.cpp` (recipes, bills, cooking speed),
`Private/BillJobs.cpp` (the `DoBill` job), the `DoBillsGiver` scanners in `Private/WorkGivers.cpp` (one per bench kind, see [crafting.md](crafting.md)).
Defs: `data/defs/Recipes/Recipes.json`, the meals in `data/defs/Things/Items.json`, `BillRules` in
`data/defs/Rules/Rules.json`. Tests: `Tests/BillsTests.cpp`, the replay `Tests/replays/colony-cook.json`.
Task C7.

As in RimWorld, the player puts **bills** on a work building (so far the stove): "cook simple meal,
3 times". Colonists with the **Cooking** work type fetch the ingredients, stand beside the stove, work,
and a meal appears. Hungry pawns then eat it (needs.md prefers the tastiest food).

## Recipes

A `Recipes` def (RimWorld's RecipeDef):

| Field | Meaning |
| --- | --- |
| `recipeUsers` | the Things building defs that can hold a bill for it (`["FueledStove", "ElectricStove"]`) |
| `workAmount` | game seconds of work at speed 1 (RimWorld's ticks x 1.44) |
| `workSkill`, `minSkill`, `workSkillXp` | the Skills def that speeds it up; the least level to be given the job; the XP per run (kept for the day skills learn) |
| `ingredients` | a list; each has `categories` (item defs whose `thingCategories` include one) and/or `items` (defNames), plus `nutrition` (worth of the items' `stats.nutrition`) or `count` |
| `products` | `{ "ItemDef": count }` |

| Recipe | Work | Ingredients | Makes | Skill |
| --- | --- | --- | --- | --- |
| `CookMealSimple` | 300 ticks | 0.5 nutrition of `FoodRaw` (10 berries) | 1 simple meal | none |
| `CookMealSimpleBulk` | 1,200 ticks | 2.0 nutrition of `FoodRaw` | 4 simple meals | none |
| `CookMealFine` | 450 ticks | 0.25 of `PlantFoodRaw` and 0.25 of `MeatRaw` or `AnimalProductRaw` | 1 fine meal | 6 |

(Wiki numbers. There is no meat item yet, so fine meals can't be cooked until animals and hunting
arrive; the recipe is there for them.) Simple and fine meals are `MealBase` items: stack 10, 0.9
nutrition, 4 days to rot.

## Bills

A bill is `{id, recipe, mode, count, suspended}` in a `BillList` component on the building (created
by the first bill). Ids count 1, 2, 3 per building and are never reused. At most 15 bills a building.

| Mode (`BillMode`) | RimWorld's | Runs while |
| --- | --- | --- |
| `Count` | "Do X times" | `count` runs are left; each run takes one off and the bill **removes itself** at 0 |
| `UntilHave` | "Do until you have X" | fewer than `count` of the first product lie on the map (carried and in-job ingredients don't count) |
| `Forever` | "Do forever" | always |

| Command | Does | Fails when |
| --- | --- | --- |
| `AddBillCommand{building, recipe, mode, count}` | adds a bill at the end of the list | not a finished building; unknown recipe; the building can't make it; count not 1 to 9999 (not checked for `Forever`); 15 bills already |
| `RemoveBillCommand{building, bill}` | takes it off; a cook working on it stops and the ingredients come back | no such bill |
| `SetBillCommand{building, bill, mode, count, suspended}` | changes it; `suspended` is RimWorld's pause | no such bill; bad count |

The snapshot has `ThingView::firstBill` / `billCount` into `Snapshot::bills` (`BillView`).

## Who cooks, and the job

The Cooking work type's `DoBillsCookStove` scanner (a `DoBillsGiver`) offers every work building that has a bill which
isn't suspended (and, for `Count`, has runs left), nearest the pawn first like all work. A cook on the
stove **holds it** (one cook at a time, as RimWorld's single interaction cell). `makeJob` goes through
the bills **in list order** and takes the first that:

1. should be done now (`BillSystem::shouldDo`),
2. needs a skill the pawn has (`minSkill`; a pawn without a Colonist counts as `defaultSkillLevel` 8),
3. has ingredients available: for each ingredient line, the free stacks of accepted items that the
   pawn can reach, **nearest to the stove first**, until the nutrition (or count) is covered; the last
   stack gives only what is needed. At most 8 stacks (`maxIngredientStacks`). Held, avoided and
   unreachable stacks are skipped.

A `DoBill` job (`Job::site` the stove, `bill` the bill id, `thing`/`count` the stack being fetched,
`picks` the stacks after it, `used` what has been handed over, `progress` the work done) has the steps

`go to the stack` - `pick it up` - `carry it beside the stove` - `hand it over` (back to the first
step while `picks` has more) - `work` - `make the product`.

All the stacks are **reserved** when the job starts, so another pawn can't eat or haul them (the
reservation list is now up to 12 long). Handing over takes the ingredient out of the world into the
job; if the job ends in any way but done (the bill or stove is removed, the cook is downed or
ordered away, a stack went missing) `returnIngredients` puts them back on the ground near the pawn.
Work per game second is the **cooking speed** times the pawn's **Manipulation** capacity. The pawn
stands on a cell beside the stove (`Construction::workCell`).

When the work is done the products appear on the nearest free cell by the cook (RimWorld's "drop on
floor"; haulers move them to a stockpile and eaters find them anywhere), `iterationDone` counts the
bill down, and `PawnAction::Cook` / `RecipeDone` are sent. A job fails if the stove is gone, the bill
is gone or suspended, or an ingredient is gone; the pawn remembers the stove for 10 minutes.

## Cooking speed

`speed = skillSpeedBase 0.6 + skillSpeedPerLevel 0.05 x Cooking level` (1.0 at level 8, 1.6 at 20), in
`BillRules`. **Recalled, not read from the wiki** (the wiki gives no table), so `unverified`. A level 8
cook makes a simple meal in 432 game seconds, 7.2 game minutes, plus the walking.

## What Unreal sees

- **Commands:** the three above. **Snapshot:** bills per building; the cook's `job` is `DoBill` and
  `jobTarget` the ingredient being fetched (none while working).
- **Events:** `PawnAction::Cook` once per run when the work begins (`target` the stove), `RecipeDone`
  (`thing` the cook, `def` the Recipes def, `target` the stove, `amount` products made, `message`
  "bill finished" if a `Count` bill ended), `JobStarted`/`JobEnded` with `JobType::DoBill`, and the
  usual `ThingSpawned` for the meals.
- Save format 18: a building's bills (`recipe` by defName), a job's `bill`, `picks` and `used`
  (by defName). Format 6 to 17 saves still load.

## Not yet

- ~~Fuel~~ done (C11, [fuel.md](fuel.md)): the stove holds 50 wood, burns one per meal made and is
  stopped when it is out; the Refuel job under Hauling brings wood. A new stove starts empty.
- Skill learning (XP is in the recipe, no sim system gives it), food poisoning, meals remembering
  their ingredients (mood from human or insect meat), rot.
- ~~Bill details~~ done (C14, [crafting.md](crafting.md)): reordering, the ingredient radius, allowed skill
  range, "store products in", the ingredient filter, "pause when satisfied, unpause at".
- ~~Other stoves and work tables~~ done (C14): the electric stove and every bench have their own work giver.
  Butchering waits for N2. Still open: campfire (half speed), cooking indoors, temperature and room-role speed
  factors, the stove's heat.
- Pawns pick the cheapest food for the cook's kitchen first only by distance to the stove; no
  "prefer the older stack" rule.
