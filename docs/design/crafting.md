# Work tables and crafting

Code: `ColonySim/Crafting.h`, `Private/Crafting.cpp` (quality, stuff, bench speed, where products go),
`Private/Bills.cpp` (bill details), `Private/BillJobs.cpp` (the `DoBill` job), the `DoBillsGiver` in
`Private/WorkGivers.cpp`. Defs: `data/defs/Rules/CraftingRules.json`, the benches in
`data/defs/Things/Buildings.json` and `Benches.json`, the recipes in `data/defs/Recipes/`.
Tests: `Tests/CraftingTests.cpp`. Task C14. Builds on [bills.md](bills.md) (the bill and the `DoBill` job
itself), [apparel.md](apparel.md) (stuff and quality on clothes) and [fuel.md](fuel.md).

Every work building takes bills now, not just the stove, and what a bill makes can be made *of* something
and have a *quality*.

## Benches and who works at them

A building def names the work giver that offers its bills with `workGiver`. The work types list the givers
(`WorkTypes.json`), so the colonist's work priorities decide who works where:

| Work giver | Work type | Benches | Makes |
| --- | --- | --- | --- |
| `DoBillsCookStove` | Cooking | fueled stove, electric stove | meals, kibble |
| `DoBillsButcher` | Cooking | butcher table | meat and leather from corpses ([hunting.md](hunting.md), N2) |
| `DoBillsSmithy` | Smithing | fueled smithy, electric smithy, machining table | knife, club, gladius, spear, mace, longsword, breach axe |
| `DoBillsTailor` | Tailoring | hand tailor bench, electric tailor bench | clothes |
| `DoBillsCraftingSpot` | Crafting | crafting spot | prosthetics (wood), knife, club, spear |
| `DoBillsStonecut` | Crafting | stonecutter's table | stone blocks from chunks |
| `DoBillsSmelt` | Crafting | electric smelter (Electricity, 170 steel, 2 components, 700 W) | smelt or destroy weapons and clothes |
| `DoBillsCremate` | Hauling | electric crematorium (Electricity, 150 stone, 20 steel, 2 components, 200 W) | cremate corpses, burn weapons and clothes |

A recipe lists the buildings that can make it (`recipeUsers`); the cooking recipes now include the electric stove.
A new bench needs only its `workGiver` and its recipes. The check `Tests/CraftingTests.cpp` fails when a
bench of a recipe names no giver or a giver no work type lists.

- **Research:** a bill can only be added when the recipe's `researchPrerequisite` is done (unchanged from C7).
- **Power:** a bench with a power part does nothing while unpowered (unchanged from E5), except one whose def has
  `building.unpoweredWorkSpeedFactor` (the electric tailor bench, 0.5): it is worked at that share of its speed.
- **Fuel:** the fueled smithy has a `fuel` field like the stove: 50 wood, one burnt per product (ours: RimWorld
  burns it by the time worked). A bench without fuel is never out of it.
- **Bench speed:** `stats.workTableWorkSpeedFactor` multiplies the work done each second (hand tailor bench
  0.5, the others 1). RimWorld's number.
- **Stuff speed:** a product made of a stuff takes the stuff's `stuffProps.statFactors.workToMake` times as
  long (wood 0.7 against steel 1).
- **Skill speed:** unchanged: `0.6 + 0.05 x level` of the recipe's skill (recalled, not from the wiki).

## Ingredients

An ingredient line takes items of `categories` (an item's `thingCategories` or, for a stuff, its
`stuffProps.categories`: Metallic, Woody, Fabric, Leathery, Stony), `stuffCategories` (the same, for the
clothes recipes) or named `items`. A recipe with `productsMadeOfIngredient` (clothes) or
`productStuffFromIngredient` (weapons) takes its stuff from **one item def**: 60 cloth, not 30 cloth and 30
leather. The cook looks at the defs in turn, the def with the nearest stack first.

## Stuff and quality of a product

When a bill makes something (`finishProduct`):

1. **Stuff:** the item def among the ingredients used that is a stuff of one of the product's
   `stuffCategories` and has the most units (the lower def id on a tie). Clothes get `MadeOf` through
   `ApparelSystem::setMadeOf`, which also sets their hit points; other things get a `MadeOf` component.
2. **Quality:** things with a quality (apparel, melee weapons, anything with `hasQuality: true`) roll one by
   the crafter's skill in the recipe's skill, from the wiki's table in `CraftingRules.qualityChances` (one row
   per skill level 0 to 20: awful, poor, normal, good, excellent, masterwork). Skill 0 makes an awful thing
   65% of the time, skill 8 a normal one 51%, skill 20 an excellent one 51% and a masterwork 10%. **Legendary
   never comes from the table**: in RimWorld it takes an inspiration, and here too: `rollQuality(skill, shift)` raises the result by `shift`
   levels, and Inspired Creativity gives a shift of 2 to the crafter's next thing with a quality ([inspirations.md](inspirations.md)). Normal quality
   is not stored (no `Quality` component). The roll uses the system's own random stream (seed xor `CRFT`,
   saved in the `crafting` section).
3. **Where it goes:** with a bill's `storeZone`, the product gets a `StoreIn` mark and haulers carry it to
   that stockpile before any other (the best other stockpile if it has no room). The mark goes when the
   product lies in the zone, or the zone is gone (checked every game minute).

Melee weapons carry their stuff and quality, and the snapshot shows both (`ThingView::stuff`, `quality`), but
**their damage does not use them yet** (repair and upgrades are P4). Clothes already do (C15).

## Bill details

Each bill has (RimWorld's bill dialog; defaults in brackets):

| Field | Meaning |
| --- | --- |
| `skillMin`, `skillMax` [0, 20] | only a worker whose skill is in the range takes the bill (with the recipe's own `minSkill`) |
| `radius` [0 = anywhere] | ingredients are taken only from within this many metres of the building |
| `storeZone` [0 = none] | the stockpile the products go to |
| `countZone` [0 = whole map] | "until you have" bills count only the stock lying in this stockpile |
| `unpauseAt` [0] | "until you have" bills: once the stock reaches the count the bill waits until the stock is *under* this number. 0 starts again as soon as it is under the count |
| `disallowed` [none] | item defs the bill may not use as ingredients (the ingredient filter) |

`BillSystem::step` looks at the stock of "until you have" bills with an `unpauseAt` once a game minute and
sets or clears their `satisfied` flag; `shouldDo` is false while it is set.

| Command | Does | Fails when |
| --- | --- | --- |
| `SetBillOptionsCommand{building, bill, skillMin, skillMax, radius, storeZone, countZone, unpauseAt, disallowed}` | replaces all the details | no such bill; skills not 0 to 20 with min <= max; radius under 0 or over 2000; `unpauseAt` out of range; no such stockpile; a name that is not an item |
| `MoveBillCommand{building, bill, index}` | moves a bill to that place in the list (0 first, past the end last) | no such bill |

The snapshot's `BillView` shows all of them. Setting the details clears `satisfied`; so does `SetBill`.
Saves: section `bills` 3 has the details (3 added `countZone`; the `commands` section 2 saves it too) (old saves load with the defaults), section `crafting` 1 the random
stream and the `StoreIn` marks.

## Not yet

- Butchering was left to N2 and is done: [hunting.md](hunting.md).
- Skill experience from crafting (`workSkillXp` is read and unused, as for cooking).
- Art, sculpting, smelting, the machining table's other recipes.
- Damage and hit points of melee weapons from their stuff and quality (P4).
- "Take from this stockpile only" and a per-bill ingredient quality
  filter (the bill's `disallowed` list is by item def).
- The electric tailor bench's `unpoweredWorkSpeedFactor` (an unpowered bench does nothing).

## Fuel per run and long-lasting foods

A stove burns `fuel.perUse` wood per product, unless the recipe has `fuelUses`: then the run burns that many uses. `MakeKibble` (50 kibble), `MakePemmican` (16) and `CookMealSurvival` (1) have `fuelUses: 1`. Pemmican keeps 150 days, the packaged survival meal 60, kibble 50 (simple meal 4); rot follows `docs/design/spoilage.md`. `Pemmican` research unlocks its recipe.

## Smelter and crematorium

`SmeltWeapon` and `SmeltApparel` (2304 s of work, RimWorld's 1600 ticks) give back 25% of the metal in the recipe that makes the thing,
rounded down per metal, whatever its quality or hit points. A recipe with `smelt: 0.25` does this: when the ingredient goes into the
smelter `BillSystem::smeltYield` finds the recipe that makes it and keeps the metals (the thing's own stuff for a stuff ingredient, a
named metal as it is); components, cloth, leather and wood are lost. An interrupted run drops the metal, not the weapon. `DestroyWeapon`,
`DestroyApparel` (576 s) and the crematorium's `BurnWeapon`, `BurnApparel` (86 s) eat the thing and make nothing. `CremateCorpse`
(`cremate: true`, 259 s) is the butcher's job with no meat, leather or memory: the corpse just goes. The ingredient filter now knows the
pseudo-category `Apparel` (any item with an `apparel` block). Tests `SmeltingTests`, `HuntingTests`.

Not yet: steel slag chunks (nothing leaves slag in this game), "burn drugs" (waits for the drug items), the fabrication bench's component
recipes and gear (PR 170), the drug lab and brewery (PR 163), sculpting (PR 155).
