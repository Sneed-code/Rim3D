# Level ground for structures

Code: `ColonySim/Map.h` (`levelGround`, `levelCells`, `levelBase`, `levelledCorners`, `LevelState`),
`Private/Map.cpp`; called from `Things::spawnAt`, `Construction::placeBlueprint`, `UpperFloors::lay`, the
`PlaceFloorCommand` handler in `Sim.cpp` and `Rooms::levelRooms`. Tests: `Tests/LevellingTests.cpp`.
Asked for by sneed on 2026-11-13: walls stood unevenly on sloped ground (each wall sat on the lowest
corner under it, so their tops went up and down the hill) and floors followed the slope.

## The rule

Anything built on the ground digs out or fills its ground to one flat height, its **base**. The terrain mesh
has a vertex per cell corner at `Map::groundHeight`, and Unreal already stands a building on the lowest corner
under it, so once the corners are level the ground is flat and every wall on that base has the same top.

- **Fixed steps.** A base is always a whole number of `Map::kLevelStepM` (0.25 m): the lowest corner under the
  footprint, rounded to the nearest step. On flat ground that moves a corner at most 12.5 cm.
- **Joining.** If a levelled cell next to the footprint (8 around it, or the footprint itself) has a base
  within `Map::kLevelJoinM` (1 m) of that, the new structure takes it (the nearest one, the lower on a tie). So
  a run of walls shares one base and lines up; a wall running up a hill keeps the base until the ground is a
  metre above it, then steps up. No wall is dug in more than about a metre.
- **Shared corners.** A corner is shared by the four cells round it. It is moved only if no levelled cell round
  it has another base; where two bases meet the corner stays and the ground ramps between them.
- **Rooms.** When Rooms rebuilds, an enclosed room on the ground whose cells are all roofed is levelled to the
  base most of its walls and doors share, with the furniture that stands in it (impassable furniture is in no
  region but its cells touch the room). So the floor inside is flat and an upper floor over it is flat too. A
  room walled only by rock (a cave) keeps its ground; a big walled yard that is not roofed is not levelled.
- **Plans level at once**, so the player sees the pad where a building will go. Cancelling the plan leaves the
  ground dug (ours; a later task may make levelling a step of the construction work).
- **Floors** (`PlaceFloorCommand`) level each cell they lay, joining as above, so a big floor on a slope becomes
  terraces a metre apart at most. **Upper floors** level the ground cell under them (the floor runs parallel to
  it, 3 m a level up).
- **Not levelled:** buildings on moving water (the watermill), anything on an upper level (it stands on an upper
  floor, which follows the levelled ground).

Numbers are ours: RimWorld's ground is flat.

## What it changes

Only ground heights: pawns walk on the new surface (`heightAt`), cover and sight are told about the four cells
round each moved corner. Regions, paths and terrain types do not change.

## Unreal

`Map::levelledCorners()` lists every corner moved, in order (a corner can come twice). The terrain, upper floors,
snow and fire layers redraw the cells round the ones they have not seen yet, like a mined cell; a load rebuilds
everything anyway. Not done yet (a PC task): until then the change shows after a load.

## Saves and hashes

Section `terrain` version 2 adds `levelCells`/`levelBases` (each levelled cell's base in cm) and
`levelCorners`/`levelHeights` (each moved corner's height in cm); bases are whole steps, so cm are exact. The
map is made again from the seed on loading and these are put back. `Sim::stateHash` adds `Map::levelHash()` once
anything was levelled, so hashes of games with nothing built stay as they were.
