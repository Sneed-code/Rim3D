# Allowed areas

Code: `ColonySim/Areas.h`, `Private/Areas.cpp`. Tests: `Tests/AreasTests.cpp`. Save section `areas` 1.

As in RimWorld, an **area** is a named set of cells (any level), and a pawn can be **limited** to one. A limited pawn:

- takes no work whose target is outside its area (checked in `JobSystem::thinkWorkPass` for every work type);
- hauls only to a cell inside its area (`makeHaulJob`).

A pawn with no area, which is every pawn until a command says otherwise, is not limited.

| Command | Does | Fails when |
| --- | --- | --- |
| `CreateArea{name}` | a new empty area; a blank name becomes "Area <id>"; ids count 1, 2, 3 and are never reused | never |
| `PaintArea{area, rect, add}` | adds the rectangle's cells or takes them off | no such area, or no cell changed |
| `DeleteArea{area}` | deletes it; pawns limited to it are free again | no such area |
| `AssignArea{pawn, area}` | limits the pawn; area 0 frees it | no such area or pawn |

The snapshot has `areas` (`AreaView`: id, name, cells from `areaCells`) and `ThingView::area` for each pawn.

## Not done (BACKLOG)

- A limited pawn that is outside its area is not walked back in, and idle wandering and recreation are not limited to it
  (RimWorld does both). Eating and sleeping ignore areas on purpose, as in RimWorld.
- Animal zones (`husbandry.md`) and the fire "home area" (`fire.md`) stay separate.
- No Unreal UI yet (an Areas tab and a paint tool are a U task).

## Parity audit (task "Check work priorities, schedules and zones")

Checked against RimWorld and found the same: work priorities 0 (off) and 1 to 4 with 1 first, work types in Work tab order,
emergency work types asked first; schedules of Anything, Work, Joy and Sleep per hour (Sleep only when tired enough,
Joy plays, Anything and Work do work); stockpile priorities Low to Critical, items hauled only to a higher priority than where they lie,
a per-def filter; bills "do X times", "do until you have X", "forever", skill range, ingredient radius, ingredient filter,
"store in this stockpile", pause and restart thresholds, reordering.
One deliberate difference stays: within one priority the nearest job wins whatever its work type, not the Work tab order
(game design doc, `jobs.md`).
Gaps left in BACKLOG: stockpile filters by quality and hit points, and "Drop on floor" versus "Best stockpile" for bill products.
