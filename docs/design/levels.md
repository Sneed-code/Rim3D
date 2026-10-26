# Levels and upper floors

Code: `ColonySim/Map.h` (levels, upper chunks, `setUpperFloor`, `floorAbove`, `LevelLink`), `ColonySim/UpperFloors.h`,
`Private/UpperFloors.cpp`; level handling in `Private/Regions.cpp`, `Private/Rooms.cpp` and
`Private/Temperature.cpp`; stairs and ladders in `Things.h` (`levelLinksOf`), `Private/Regions.cpp`
(`linkLevels`), `Private/Pathfinder.cpp` and `Private/MovementSystem.cpp`. Command `PlaceUpperFloorCommand`.
Tests: `Tests/LevelsTests.cpp` (M1), `Tests/StairsTests.cpp` (M2). Tasks M1 and M2, of M1 to M4 and U17. The whole thing together is checked by the [house scenario](house-scenario.md) (M4).
Builds on [regions](regions.md) and [rooms](rooms.md).

## Levels

The map has `Map::kLevels` = 4 levels: the ground (level 0) and three above it. Every grid, region and
path function already took a `CellPos{x, y, level}`; M1 fills the upper levels in.

- **Height.** Each level is `Map::kLevelHeightM` = 3 m above the one below (ours; RimWorld has no
  levels). An upper floor runs parallel to the ground under it, so `Map::groundHeight(c)` and
  `Map::heightAt(x, y, level)` are the ground's height plus 3 m per level. The snapshot's `z` for a
  thing upstairs is that height.
- **Storage.** The ground keeps its whole layers. An upper level is stored in 16 m chunks (the same
  size as a region block), made the first time something is laid or built in them, so empty sky costs
  nothing: 7 KB per chunk in use. `Map::hasUpperChunk` says whether one exists.
- **Terrain.** An upper cell is `Terrain::Air` (nothing to stand on, nothing grows) until an upper floor
  is laid; then it is `Terrain::UpperFloor`, walkable and buildable like ground, with no path cost
  (RimWorld's constructed floors have none). `Map::inBounds` takes levels 0 to 3;
  `Map::onGround(c)` is in bounds *and* on level 0.

## Upper floors

An upper floor is the floor of a cell on level 1 or above. It is laid with `UpperFloors::lay` (and
`PlaceUpperFloorCommand`, which does a rectangle and fails with the last reason if no cell changed).

- **Support.** It is held up like a roof: a building that holds roofs (a wall, a column, a door) or
  rock within the roof support radius (`RoomRules.roofSupportRadius`, 6.9 m, RimWorld's) **on the level
  below**, counting the cell right under it (`Rooms::roofSupported`). So a level 2 floor needs walls
  standing on the level 1 floor. Blueprints and frames don't hold anything up; only finished buildings.
- **Not over a mountain.** No upper floor over a cell with the mountain's thick roof.
- **Roof.** An upper floor is the roof of the cell under it: `Map::roof` says `Thin` there, so the
  cell below is indoors and lit like any roofed cell. Rooms never puts its own thin roof under a floor,
  so `Rooms::thinRoofCells` holds only the roofs Unreal should draw.
- **Buildings and items** go on an upper level only where a floor is laid (`Things::spawn` and
  building plans need walkable ground, which air is not). Nothing else is needed: the floor carries
  the support rule.
- **Taking one up** (`UpperFloors::remove`, the command with `remove`) is refused while a building, an
  item or a pawn is on it.
- **Built by colonists** (M2): the `UpperFloor` building def is planned one cell at a time like any
  building (`PlaceBlueprintCommand{"UpperFloor", stuff, cell}`), only where `UpperFloors::whyNotLay` allows,
  and colonists deliver its materials and build it; when the frame is done the floor is laid instead of a
  building spawned. A plan over open air is worked from the level below too, under it or beside that
  (`Construction::workCell`), so the first floor needs no stairs. Cost and work are the wood floor's: 4
  stuff (metal, wood or stone), 216 s (ours: RimWorld has no upper floors). `PlaceUpperFloorCommand`
  still lays floors at once, for tests and the dev tools. Floors fall when the walls under them go
  ([roofs](roofs.md)).

## Regions, rooms and temperature per level

- **Regions** are made per level from the same rules; a block on an upper level is flooded only if its
  chunk exists. Each level's region ids live in their own layer, made when the level is first used
  (`Regions::regionIdsOnLevel` is null before that). Only stairs and ladders link regions on two
  levels (next section); without them an upper floor is a reachability group of its own.
- **Rooms** are made per level. On an upper level a room with a cell beside open air (`Terrain::Air`)
  has `Room::openToAir` and is outdoors, like one that reaches the map edge; walls all round the edge
  of the floor make an enclosed room, which Rooms roofs with a thin roof as on the ground. A room under
  a floor is roofed by it.
- **Temperature.** Every enclosed room on any level has its own temperature. An upper floor passes
  heat like a thin roof (`TemperatureRules.roofThin`) between the room above and the room below,
  both ways; where there is no room on the other side (a wall) no heat goes that way.

## Stairs and ladders (M2)

Two buildings link a level with the one above (`building.climb` in their def; numbers ours, RimWorld has
no levels):

| Building | Size | Cost | Work | Speed on it | Climb |
| --- | --- | --- | --- | --- | --- |
| `Stairs` | 1 x 3 | 30 metal, wood or stone | 1,440 s | 60% of walking | 4 m along, 3 m up: 5 m |
| `Ladder` | 1 x 1 | 10 metal or wood | 432 s | 30% of walking | 2 m along, 3 m up: 3.6 m |

- **Where they go.** A climb goes towards the building's facing (North is +y). It starts at the cell in
  front of its foot, on the building's own level, and ends at the cell beyond its head one level up
  (`levelLinksOf`; a wider building gets one per cell of its width). Seen from above the two ends are
  side by side, so a staircase against a wall leads onto the floor that wall holds up. The building
  itself blocks walking: it is only climbed. Neither end needs to be walkable when it is built; the
  link counts once both are. Nothing is planned on the top level (there is nothing to climb to).
  Nothing stops a floor over the stairs for now; U17 may want a stairwell rule. An upper wall with open air on its outside cannot be reached once the walls on its two inner sides stand (see the house scenario).
- **The map** keeps the links (`Map::levelLinks`, `LevelLink{low, high, building, length, speedFactor}`,
  sorted by cell) in step with the buildings (`Things` adds them on spawn and load, removes them on
  despawn), and marks the regions at both ends dirty.
- **Regions** link the regions at the two ends (`Regions::linkLevels`), so they share a reachability
  group. The link's point is halfway between the two cells and it carries the climb's `extra` metres
  over the flat walk (cost `length / speedFactor` less the flat distance); both regions are marked
  `climbs`.
- **Paths.** The region route and its guide add a link's `extra`; the cell search keeps one array per
  level used and, in a region that `climbs`, steps from one end of a link to the other at its cost.
  A path's cells carry their level: the foot of the stairs is followed by their head. `smoothPath`
  straightens each level's part on its own and keeps both ends of every climb.
- **Climbing.** When a pawn's next waypoint is on another level it is at the foot (or head) of a climb:
  it stays in that cell, out of everyone's way, while `Mover::climbed` counts up at its walking speed
  times the link's `speedFactor` (dark, weather, injuries and load slow it as any walk); at the link's
  length it stands in the cell at the other end. If the stairs are gone it gives up the walk. A pawn
  downed or held on the way starts the climb again. Its `Position` is the cell at the foot throughout,
  so whatever asks where a climbing pawn is gets a walkable cell.
- **Unreal sees** a climbing pawn in the snapshot at `climb` (0 to 1) of the way along: its `x`, `y`
  and `z` are already moved that share towards the other end, its `level` is the one it started on.
  U17 draws stairs and ladders and the climb.

## Work across levels (M2)

- **Stockpiles** can be made on upper floors (`Zones` keeps a layer per level used).
- **Doors** work upstairs as on the ground (their own region, opening, rooms).
- These look across levels for their nearest target, counting each level between as `kLevelDetourM`
  = 10 m more (`levelDistSq`; ours): hauling to stockpiles, delivering materials, building frames,
  taking buildings down, food to eat, and a bed to sleep in. Every other work scanner still looks only
  on the pawn's own level (`pos.level != at.level`) until a task needs it.

## Systems that are still ground-only

These use `Map::onGround` and ignore cells on upper levels for now: plants, growing zones, mining designations, rock, floors (C17's finishes), filth, fire and the home
area, snow, lamps and darkness, solar panels' open sky, cover and line of sight, threat maps. Combat and fighters work on one level at a time but
take heights from the ground. Animals live on the ground. Each will take levels when a task needs it
(U17 draws it all).

## Unreal sees

`Map::upperFloorCells()` (by level, then row) and `Map::upperFloorChanges()` to know when to redraw;
buildings and items on upper levels come through the snapshot with their `level` and `z`. Nothing is
drawn yet: U17 does that, with the overseer cutting away levels above the chosen one. `simrun --png`
tints the ground pale blue under upper floors.

## Saves and hashes

Section `levels` (version 1) lists the upper floors; they are put back before the buildings, which
need them. `Sim::stateHash` adds `Map::upperFloorHash()`. Roofs, regions and rooms are worked out
again on load, and so are the stairs' links, from the buildings. Section `movement` 2 saves each
pawn's `climbed`; the state hash adds it only while a pawn climbs, so older hashes stay.

## Not yet

- Direct mode climbing stairs and ladders, and the orders bar's upper floor and stairs tools (U17).
- Raiders, fighters and animals using stairs: they path over them already, but combat, cover and the
  defence plans still look at one level at a time.
- Upper levels on screen (U17).
