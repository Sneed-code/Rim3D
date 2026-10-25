# Orders: the architect menu, marks and fighting orders

Code: `game/Source/Rim3D/OrdersSubsystem.*`, small hooks in `OverseerPawn.*`, `OverseerSubsystem.cpp`,
`SimPicking.*` (`PickGroundFromRay`) and `BuildingSubsystem.*` (`GetWholeMesh`, `GetPlacedTransform`).
Smoke test: `orders` (`tools/smoke.ps1 -Test orders`). Task U7. Unreal side only; the sim is untouched,
every order is an existing command ([bridge.md](bridge.md), [construction.md](construction.md),
[mining.md](mining.md), [fighters.md](fighters.md)).

## What it is

`UOrdersSubsystem` (one per game world) is the player's hand in the overseer view ([overseer.md](overseer.md)).
It holds a **tool**, turns clicks and drags on the ground into commands, draws the sim's marks, and sends
the draft, move and attack orders. It works only while the overseer view is the one in use; in direct mode
([protagonist.md](protagonist.md)) a tool in hand is put down.

Orders given while the game is **paused** make the sim step once, so a plan or mark shows at once
(commands are applied by the next step, and a paused game has none). Running games are not touched.

## The bar at the bottom

Buttons for **Orders**, **Zones** and one for each architect category the defs name
(`designationCategory`: Structure, Furniture, Production...). A category opens its row of tools:

| Category | Tools | What the drag does |
| --- | --- | --- |
| Orders | Mine, Chop trees, Harvest | `DesignateCommand` for the dragged rectangle (rock, trees and plants, plants that give something) |
| Orders | Deconstruct | `DeconstructCommand` for every finished building touching the rectangle |
| Orders | Cancel | `CancelDesignationsCommand` for the rectangle, and `CancelBuildCommand` for every plan, frame and deconstruct mark touching it |
| Zones | Stockpile | `CreateStockpileCommand`: a zone of the free cells in the rectangle |
| Zones | Remove zone | `RemoveZoneCellsCommand` |
| a building category | one button for each building def with a `designationCategory` and `stats.workToBuild` | see below |

Click a tool to pick it, click it again, press Esc or right click to put it down. With a tool in hand the left
button does the tool's job instead of selecting. The bar also shows what the last order did ("Planned 6 x Wall").

**Building.** Picking a building shows a *Made of* list (the stuffs that fit, with how many the map holds; the
one with the most is chosen) and what it costs. Move the cursor: a **ghost** of the building follows it, snapped
to the cell grid, **green where it can be planned and red where it can't** (off the map, water, rock, a building or
another plan). R turns it (the same turn as `Rot4`: north, east, south, west). A click plans it; a 1 x 1 building
(wall, stool) can be **dragged along a line** (the longer way) to plan one on each cell, and the cells that are
blocked are skipped. A building of several cells is centred on the cursor's cell. The sim re-checks everything
(`PlaceBlueprintCommand`); the ghost's check is the same rule for the sim's walkable ground, buildings and plans.

The ghost is the same mesh the building will have, drawn with the see-through `M_BuildingGhost` material
([building-draw.md](building-draw.md)) in green or red (two dynamic instances of it). Without the generated
material there is no ghost, only the outline of the footprints.

## The marks

The sim's designations, stockpile cells and deconstruct marks are drawn from the snapshot, one instanced mesh
component for each kind (and each plant height), rebuilt only when a signature of the marks changes:

| Mark | Looks like |
| --- | --- |
| Mine | orange ring and post, on top of the rock block ([terrain.md](terrain.md)) |
| Cut (trees, plants) | green ring on the ground, a thin post and a plate above the plant's top, so a crown doesn't hide it |
| Harvest | the same in yellow |
| Deconstruct | red ring and plate on top of the building |
| Stockpile cell | blue see-through tile (a blue ring if the ghost material is missing) |

`Orders.Marks 0` hides them. While you drag, the rectangle is outlined in the tool's colour (lines on the ground
following the terrain). Marks, ghosts and outlines are all ordinary static meshes and debug lines: no custom
render passes, no dithering, no world-position offset, so DLSS stays possible
([terrain.md](terrain.md), [building-draw.md](building-draw.md)).

## Draft, move, attack

| Input | Does |
| --- | --- |
| R, no tool in hand, a colonist selected | `DraftCommand`: draft, or send back to work if drafted. (R with the Build tool turns the ghost instead.) The inspect panel has the same as a button |
| right click on ground, a drafted colonist selected | `MoveOrderCommand` to that cell (the colonist holds in the best cover within 2 m) |
| right click on a pawn of another side | `AttackOrderCommand` |
| Alarm button (top right) | drafts every colonist who isn't; when all are, the button says *Stand down* and sends all back |

Drafted colonists get a **red ring** at their feet, a yellow line to the cell they hold and a red line to the pawn
they shoot at.

**The alarm is a placeholder.** F6 brings defence plans (Defender, Shelter, Keep working roles) and a real
alarm command; until then the button drafts everyone. Swap it for F6's command then.

## Not here yet

- Floor and zone tools beyond the stockpile (growing zone, home area), zone filter and priority panels.
- Dragging a rectangle of walls or floors (lines only), and selecting several colonists at once (the selection is one thing).
- Right-click menus for jobs on a thing (prioritise, take, haul to a zone): only the commands above.
- Work-type and stuff preferences per bill (bills themselves are in the U8 panels, [panels.md](panels.md)).
- The alarm roles of F6, which also decides what a colonist does when the alarm sounds.

## Smoke test `orders` (about 20 s)

Driven by calls and by screen positions, as the mouse does. Phases:

1. `orders_setup`: the menu offers all eight buildings and nothing else (steel is refused); a wall gets a stuff.
2. `orders_architect`: a line of six walls shows six green ghosts and sends six commands; a bed turned east
   has a two by one blueprint; a ghost on a plan is red and sends nothing; a door, a table and the stove
   (no stuff) are planned; ten blueprints stand and the sim refused nothing. A bed ghost hovers for the picture.
3. `orders_marks`: four rock cells are marked by a real press and release at their screen positions; a tree
   and a bush are chopped and harvested; a stockpile is dragged out; the overlay has one instance for each mark
   and each zone cell.
4. `orders_cancel`: a wall is marked for deconstruction (a red mark shows), then Cancel takes off the wall and
   door plans, the deconstruct mark and the mine marks, and leaves the marks outside the dragged rectangles.
5. `orders_fight`: R drafts a colonist; a right click on the ground holds it near that cell; a right click on a
   pawn that was put on another side is an attack order; the alarm drafts the rest and, pressed again, sends all
   back; R drafts once more for the picture.

Not covered (needs a person): the buttons themselves, the keyboard binding of R and Esc, how the ghost feels
under a moving mouse.
