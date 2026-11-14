# Build menu and placing plans

Code: `ColonySim/BuildPlanner.h`, `Private/BuildPlanner.cpp`; `Construction::whyNotBlueprint`,
`whyNotFloorPlan` and `removeFloor`; the `RemoveFloor` designation, job and work giver. Defs:
`ConstructionRules` (`architectCategories`, `architectLabels`, `removeFloorWork`), the wall's
`building.dragShape`, `RemoveFloor` in the Construction work type's tasks. Tests:
`Tests/BuildPlannerTests.cpp`. Not a plan task (sneed, 2026-11-14: "I can't build anything yet").

The overseer's architect menu ([orders.md](orders.md)) and, later, placing things by hand in first
person ([handson.md](handson.md)) ask one piece of plain C++ what can be built and what a drag would
plan, so both views place things the same way and the cloud can test it. `BuildPlanner` only reads
the sim; what it plans goes in as ordinary `PlaceBlueprintCommand`s, which the sim checks again.

## The menu

`BuildPlanner::menu()` gives the categories in RimWorld's order from `ConstructionRules`
`architectCategories` (Structure, Production, Furniture, Power, Security, Misc, Floors, Joy, Ship,
Temperature; any other category after them by name), labelled by `architectLabels` (Joy says
*Recreation*), empty categories left out. Each entry is a Things building with a
`designationCategory` and `stats.workToBuild`, or a Terrain floor on the Floors menu that costs
something (smoothed stone is made by smoothing rock, so it isn't on it). An entry has its label,
footprint, whether it is made of a stuff, and **`locked`**: the research it still needs, in plain
words, so the menu can show it greyed out instead of hiding it.

`stuffChoices(def)` lists the stuffs that fit with how many lie on the map (spawned stacks), the most
first; `bestStuff` is the first. `onMap(item)` counts one item.

## Dragging

Every entry has a drag shape (the player may pick another):

| Shape | Plans | Default for |
| --- | --- | --- |
| `Single` | one, where the drag ends | buildings bigger than one cell |
| `Line` | a row along the longer side of the drag | one-cell buildings |
| `Outline` | the edge of the dragged rectangle | the wall (`building.dragShape`: a room in one drag) |
| `Filled` | every cell of the rectangle | floors |

`dragCells(from, to, shape, stepX, stepY)` gives the origins; a footprint bigger than a cell steps by
its own size (as placed, after turning), so beds dragged out never overlap, and one dragged the
negative way grows back over the cursor. `preview(def, stuff, from, to, rot, shape)` checks every spot
with the sim's own rule (`whyNotBlueprint` / `whyNotFloorPlan`: map edge, water, rock, a building, a
plan, a floor already laid), refuses the whole order for an unknown def, a stuff that doesn't fit or
missing research, and adds up the cost of the spots that can be planned against what the map holds
(`affordable()`; colonists wait for more, as in RimWorld). `commands(preview, ...)` hands back one
`PlaceBlueprintCommand` per plannable spot.

Everything snaps to the 1 m grid: the sim decides walking, seats, rooms and cover by cell (sneed
agreed, 2026-11-14). A finer grid for small decor and a cosmetic nudge are in `BACKLOG.md`.

## Removing a floor

`DesignateCommand{area, RemoveFloor}` marks every cell of the area with a floor (none: refused).
Construction workers (`RemoveFloor` task, after Deconstruct) walk beside the cell and work
`removeFloorWork` game seconds (RimWorld's 200 ticks, x 1.44) at construction speed times
`buildWorkPerSecond`; then the floor is gone and `deconstructRefundFactor` of the floor's fixed
`costs`, rounded at random, lies beside the cell (`Construction::removeFloor`). A floor of a stuff
(stone tiles, carpet) gives none of the stuff back: the floor layer doesn't keep what it was made of.
`CancelDesignations` takes the mark off and stops the work. The designation kind and job type are
new at the end of their lists: saves keep their numbers, no save section changes.

## Not yet

- Unreal: the bar reading `menu()`, greyed locked entries with their research, the shape key, the
  floor tool and a remove-floor order drawing the mark (U-side, needs the PC).
- Copy a building (RimWorld's copy gizmo), replace a building's stuff, reinstall (move furniture).
- Stuff kept on laid floors (for a full refund).
