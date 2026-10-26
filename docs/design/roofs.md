# Roof support and collapse

Code: `ColonySim/RoofCollapse.h`, `Private/RoofCollapse.cpp`; the lost-roof list in `Rooms`
(`takeUnsupportedRoofs`), the mountain's roof in `Map` (`setThickRoof`, `hollowRoofCells`,
`rockChanges`). Numbers: `RoofRules` in `data/defs/Rules/Rules.json`. Event `RoofCollapsed`. Tests:
`Tests/RoofTests.cpp`. Task M3. Builds on [rooms](rooms.md) and [levels](levels.md).

## The rule

A roof is held up by a roof holder (rock, or a building with `holdsRoof`: wall, column, door) within the
roof support radius, 6.9 m (`RoomRules.roofSupportRadius`, RimWorld's), on the roof's own level
(`Rooms::roofSupported`). When the last holder in reach goes, the roof falls and crushes what is under
it. Three kinds of roof follow the rule:

| Roof | When it falls |
| --- | --- |
| **Built (thin) roof** | Rooms puts it over an enclosed room's cells that a holder reaches, and it then stays while one does, even if the room is opened (RimWorld's). When the last holder in reach goes, Rooms reports the cell (`takeUnsupportedRoofs`) and it falls |
| **Mountain (thick) roof** | The roof over rock stays over a cell that is mined out (`Map::roof` stays `Thick`). A mined-out cell is checked whenever a building or rock changes; with no rock or wall within 6.9 m its roof falls. So a tunnel three wide never falls, a cave about 14 m across does, and walls inside a cave hold the middle up |
| **Upper floor** (M1) | A floor on level L needs a holder within reach on level L-1, as when it was laid. When walls under it go, it falls |

## What a fall does (`RoofCollapse::step`, after `Rooms::update`, every step)

- **Thin and thick roofs** crush everything in the cell. Buildings and items lose hit points (thin 100,
  thick effectively all, so the mountain destroys what is under it); a pawn takes several `Crush` hits
  (thin 2 x 25, thick 4 x 50, random parts) and its job ends. The roof is gone afterwards: the cell is
  open sky, and a thick roof that fell does not come back.
- **A floor** drops the pawns and items on it to the nearest walkable cell on the level below (within
  3 m of the spot under it); pawns take 2 hits of 20 from the fall. A building on it is lost. The floor is
  taken up. Floors are visited lowest level first, so walls that stood on a fallen floor go with it and
  the floors they held up fall in the same step.
- It all repeats (up to 16 passes) until nothing moves, so a chain of falls finishes in one step.
- Not done: rock chunks as rubble, a warning when a mine order would bring the roof down, and
  RimWorld's "roof collapsed" letter. The numbers are recalled, not verified (`RoofRules.unverified`).

## Unreal sees

Event `RoofCollapsed` for every roof cell and floor cell that falls: `cell`, `message` ("roof",
"mountain" or "floor"), `count` things and pawns hit. `Snapshot::roofCollapses` goes up with each;
redraw the mountain's roof (`Map::roof`) and the upper floors (`Map::upperFloorCells`) when it does.
The built roof is still `Snapshot::roofCells` / `roofChanges`.

## Saves and hashes

Section `roofs` (version 1): the built roofs (they stay up on their own now, so they can't be worked out from the walls), the counter, the mined-out cells whose thick roof fell (they cannot be
worked out: the generated map has the roof everywhere), and the thin roofs Rooms had reported but that had
not fallen yet. `Sim::stateHash` adds all three. Older saves load with nothing fallen.
