# Nutrient paste

The `NutrientPaste` research (RimWorld wiki numbers, checked 2026-10-28) unlocks the **nutrient paste dispenser** and the **hopper**.

## Defs

| Def | Numbers |
|---|---|
| `NutrientPasteDispenser` | 3 x 4, impassable, 125 steel + 3 components, 2200 ticks (3168 s), 350 HP, 50% flammable, 200 W. `building.nutritionPerMeal` 0.3, `building.mealDef` `MealNutrientPaste`. |
| `Hopper` | 1 x 1, standable, 15 steel, 300 ticks (432 s), 100 HP, 50% flammable. `building.hopper`. |
| `MealNutrientPaste` | 0.9 nutrition, 0.44 kg, 10 silver, rots after 0.75 days, preferability `MealAwful`, stack 10. |
| `AteNutrientPasteMeal` (thought) | -4 for 1 day. Not for an Ascetic. |

Fill-percent and the visual parts are ours (the wiki gives cover only as 75%).

## How it works (`Private/NutrientPaste.cpp`, `NeedJobs.cpp` `findFood`)

- Hoppers go on a cell touching a side of the dispenser (not a corner). The player paints a stockpile over the hoppers
  and disallows everything but raw food, with a higher priority than other stockpiles (RimWorld's advice); colonists
  then haul food in. A hopper cell holds one stack, as every stockpile cell does.
- The dispenser's stock is the nutrition of the **raw food** (vegetables, fruit, meat, eggs and milk; not hay, not
  human flesh) lying on its hoppers.
- A hungry colonist counts a **powered** dispenser with at least 0.3 nutrition in its hoppers as a meal of the
  `MealAwful` tier, standing on the nearest free cell touching the dispenser. If it is the best food (a simple meal or
  better wins; raw food loses), the dispenser takes 0.3 nutrition from its hoppers in row order, whole items (rounded
  up, so a little is wasted: RimWorld's), puts a `MealNutrientPaste` on that cell and the colonist eats it like any
  meal, at a table if one is near. Nothing is taken when the check only asks "is there food" (a sleeper waking to eat).
- No power, no meal. Too little food, no meal and nothing is taken.

## Not done (BACKLOG)

- The hopper does not filter by itself: the stockpile over it does. RimWorld's hoppers accept only raw food, one kind each.
- Human flesh in the hoppers is left out (RimWorld makes paste of it and the eaters get the human-flesh thought).
- Prisoner feeding (`findFoodForPrisoner`) does not use the dispenser yet; wardens carry meals.
- Dispensers have no mesh yet (the visual is boxes).
