# Levels and upper floors

Code: `ColonySim/Map.h` (levels, upper chunks, `setUpperFloor`, `floorAbove`), `ColonySim/UpperFloors.h`,
`Private/UpperFloors.cpp`; level handling in `Private/Regions.cpp`, `Private/Rooms.cpp` and
`Private/Temperature.cpp`. Command `PlaceUpperFloorCommand`. Tests: `Tests/LevelsTests.cpp`. Task M1,
the first of M1 to M4 and U17. Builds on [regions](regions.md) and [rooms](rooms.md).

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
- **Instant for now**, like the floors of C17. Building one with work and materials comes with M2
  (pawns need stairs to get up there); floors that fall when the walls under them go come with M3.

## Regions, rooms and temperature per level

- **Regions** are made per level from the same rules; a block on an upper level is flooded only if its
  chunk exists. Each level's region ids live in their own layer, made when the level is first used
  (`Regions::regionIdsOnLevel` is null before that). Links stay within a level until M2's stairs link
  levels, so an upper floor is a reachability group of its own and the pathfinder finds paths on it
  but never from the ground to it.
- **Rooms** are made per level. On an upper level a room with a cell beside open air (`Terrain::Air`)
  has `Room::openToAir` and is outdoors, like one that reaches the map edge; walls all round the edge
  of the floor make an enclosed room, which Rooms roofs with a thin roof as on the ground. A room under
  a floor is roofed by it.
- **Temperature.** Every enclosed room on any level has its own temperature. An upper floor passes
  heat like a thin roof (`TemperatureRules.roofThin`) between the room above and the room below,
  both ways; where there is no room on the other side (a wall) no heat goes that way.

## Systems that are still ground-only

These use `Map::onGround` and ignore cells on upper levels for now: plants, zones and stockpiles
(M2), growing zones, mining designations, rock, floors (C17's finishes), filth, fire and the home
area, snow, lamps and darkness, solar panels' open sky, cover and line of sight, threat maps. Combat and fighters work on one level at a time but
take heights from the ground. Animals live on the ground. Each will take levels when a task needs it
(M2 does stockpiles and doors; U17 draws it all).

## Unreal sees

`Map::upperFloorCells()` (by level, then row) and `Map::upperFloorChanges()` to know when to redraw;
buildings and items on upper levels come through the snapshot with their `level` and `z`. Nothing is
drawn yet: U17 does that, with the overseer cutting away levels above the chosen one. `simrun --png`
tints the ground pale blue under upper floors.

## Saves and hashes

Section `levels` (version 1) lists the upper floors; they are put back before the buildings, which
need them. `Sim::stateHash` adds `Map::upperFloorHash()`. Roofs, regions and rooms are worked out
again on load.

## Not yet

- Stairs and ladders, region links across levels, climbing (M2).
- Laying an upper floor with work and materials (M2).
- Roof collapse and floors falling when their support goes (M3).
- Upper levels on screen (U17).
