# Upper levels on screen

Code: `game/Source/Rim3D/LevelsSubsystem.h/.cpp` (`ULevelsSubsystem`), the level cut in `BuildingSubsystem.cpp`,
`PawnSubsystem.cpp` and `PawnActor.cpp` (`SetCutAway`), climbing drawn in `SimSubsystem.cpp` (`GetViewLocation`),
picking in `SimPicking.cpp`, Page Up and Page Down in `OverseerPawn.cpp`, the level on the orders tools in
`OrdersSubsystem.cpp`, climbing in direct mode in `ProtagonistPawn.cpp` and `ProtagonistSubsystem.cpp`. Sim side:
`ClimbCommand` ([levels](levels.md)). Smoke test `levels`. Task U17.

## What is drawn

- **Upper floors** (`Map::upperFloorCells`) are slabs 20 cm thick, one procedural mesh per level and 32 m chunk. The
  top is the floor a pawn stands on (the ground's height plus 3 m a level, following the ground's slope as the sim's
  floor does), lifted 2 cm so it covers the tops of the 3 m walls under it. The underside is a pale ceiling, and the
  cut edge shows where the floor ends. Boards alternate two browns by cell, so the cells can be counted from above.
  A chunk is rebuilt only when its floors or roofs change (`Map::upperFloorChanges`, the snapshot's `roofChanges` and
  `roofCollapses`), or stairs come or go.
- **The stairwell**: no floor is drawn over stairs and ladders (the sim may still have one there: the house scenario
  lays its floor over the whole house, so the upper room stays closed), and the floor's edge shows round the hole.
- **Roofs** of enclosed rooms upstairs (the snapshot's `roofCells` on levels 1 to 3) are thin roof slabs 3 m over the
  room's floor, drawn with the floors and shown or hidden with the ground's roofs (`Terrain.Roofs`: hidden in the
  overseer view). The terrain draws only the ground's roofs.
- **Buildings, items and pawns** upstairs come through the snapshot with their `level`; the building subsystem keeps
  chunks per level, and a building upstairs stands on the floor (3 m a level over the lowest ground under it).
- **Stairs** are twelve 25 cm steps rising 3 m over their three cells; a **ladder** is a steep one leaning from the
  near edge of its cell on the ground to the far edge 3 m up (both in `data/defs/Things/Buildings.json`, `visual`).
- **A climbing pawn** (the snapshot's `climb` above 0) is drawn on the slope: flat for the half metre in each end
  cell, rising evenly over the building between them (`USimSubsystem::GetViewLocation`). Its walk animation and
  facing come from how far it moved along the climb that step, since the sim's velocity is nought on stairs.

## The level looked at

In the overseer view **Page Up** and **Page Down** (or the buttons by the date) choose the level looked at, from the
ground to level 3; the top bar names it ("Ground", "Level 1"). Everything above it is cut away: its floors, roofs,
buildings, items and pawns are hidden, so the rooms of the chosen level are seen from above. The camera's focus
rises 3 m a level with it. `ULevelsSubsystem::GetTopShownLevel` is the one place that says what is shown; direct mode
and any other view show every level. Console: `Levels.View N`, `Levels.Enable 0`.

- **Clicks** (`SimPicking::PickFromRay`) hit things on the shown levels only, and land on the highest floor shown
  over the point, else the ground; a picked cell carries its level, so the inspect panel and move orders work upstairs.
- **The architect's tools** work on the level looked at (`PickGroundFromRay` at that level's height): stockpiles,
  cancel and take-down marks and buildings go on that level, and the upper floor tool lays a floor over open air
  where `UpperFloors::whyNotLay` allows. Stairs and ladders can't be planned on the top level.

## Climbing in direct mode

Walking into stairs or a ladder from the cell in front of their foot (or back towards them from the cell beyond
their head, into the stairwell, floored or not), within about 45 degrees of their way, sends `ClimbCommand{pawn, dx, dy}`. The sim climbs the colonist
as it climbs any pawn (at the link's share of its speed) and ignores `DirectMove` until it stands at the other end;
the body stands still meanwhile, then takes the sim's place and level. The camera follows the drawn body up the slope.

## Not yet

- Plants under an upper floor stay (the sim clears plants only under a building's own footprint), so a tree taller
  than 3 m pokes through the floor above it. The sim should clear or stunt them when a floor is laid over them.
- Zones, designation marks and growing zones are drawn on the ground only; the fire, snow, light and cover overlays
  stay ground-only, as their systems do ([levels](levels.md)).
- No climbing animation: the body walks up a ladder as up stairs.
- Upper walls built by colonists in a ring need their corners first (M4, [house-scenario](house-scenario.md)); the
  architect's wall drag does not order them yet.
