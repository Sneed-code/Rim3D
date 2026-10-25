# Doors, roofs and rooms

Code: `ColonySim/Doors.h`, `Private/Doors.cpp` and the door part of `Private/MovementSystem.cpp`;
`ColonySim/Rooms.h`, `Private/Rooms.cpp`; `Map::roof`. Defs: `DoorRules` and `RoomRules` in
`data/defs/Rules/Rules.json`, `building.holdsRoof` and `building.isStove` in
`data/defs/Things/Buildings.json`. Tests: `Tests/RoomsTests.cpp`, the replay
`Tests/replays/colony-rooms.json`. Task C5. Builds on [regions](regions.md) and
[construction](construction.md).

## Doors

A door is a Things def with `thingClass` `Building_Door`. It never blocks the path (regions and the
pathfinder treat it as a walkable cell and a region of its own) but it is **physically closed until a
pawn opens it**, and that costs time, as in RimWorld.

Every door building has a `Door` component: `progress` (steps spent opening), `hold` (steps left
before it closes) and `open`. The movement system drives it, once per step, after the pawns moved:

- A pawn can't move its centre into a closed door's cell. It stops at the edge and **asks the door to
  open**. Each step a door is asked, it opens a step further; after `openSteps` it is open. While
  open, any pawn walks through.
- A pawn standing in a door's cell (even a downed one) holds it open.
- The pawn the player walks (`PlayerControl`) is never held up, but it asks for the door it is
  walking up to (8 steps ahead), so the door opens as it arrives.
- An open door that nobody asks for closes `closeDelaySteps` after the last ask. A door that was
  only part-way open closes at once.

| `DoorRules` | Value | Meaning |
| --- | --- | --- |
| `openTicks` | 45 | RimWorld's time to open (recalled, unverified) |
| `closeDelayTicks` | 110 | RimWorld's time open after the last pawn (recalled, unverified) |
| `stepsPerTick` | 0.5 | ours: opening is part of walking, which runs at real speed (a RimWorld tick is 1/60 s, a step 1/30 s), not at the 1.44 that work durations use |
| `pathCost` | 3.5 | ours: metres of walking a path counts a door as (45 ticks at 4.6 m/s) |

So a closed door costs a pawn **23 steps** (0.77 real seconds at 1x) and stays open for **55 steps**
(1.8 s) after use; a stream of pawns pays once.

**Path cost.** The pathfinder adds `pathCost` to entering a door cell, in the region route and in
the cell search (not in the guide, which is only a heuristic), so a path takes a gap in the wall
that is up to about 3.5 m longer rather than a door. Set from the rules by the sim
(`Pathfinder::setDoorCost`).

**Sight.** An open door stops blocking sight and cover: when it opens or closes the obstacle height
of its cell goes to 0 or back to its def's `fillPercent` (`setDoorSight`, `Map::setObstacle`), so the
cover map and line of sight follow ([cover.md](cover.md)). A door that was open in a save is open
for sight after loading.

**Not here:** per-faction doors (raiders can't open colony doors in RimWorld, forbidden doors),
locked doors, powered doors, doors that break or burn, and the door animation itself (the snapshot
has `ThingView::doorOpen`, 0 closed to 1 open, for Unreal).

## Roofs

`Map::roof(cell)` is `None`, `Thin` or `Thick`.

- **Thick** is the mountain: every rock cell the map was generated with. It stays when the cell is
  mined out (C3: a mined tunnel is roofed, so it is indoors).
- **Thin** is the built roof. There is no roof designation and no roofing job: **a roof appears
  by itself over an enclosed room** (task C5's "auto-roof"), at the end of the step the last wall
  stands, and goes the moment a wall is taken down (RimWorld's would collapse and hurt; that comes
  with combat damage). It is not saved: it follows from the walls and is rebuilt on loading.

A cell of an enclosed room gets a thin roof if a **roof holder** is within **6.9 m**
(`RoomRules.roofSupportRadius`, RimWorld's roof support span, recalled): rock terrain, or a building
whose def has `building.holdsRoof` (the wall and the door; ours, RimWorld has it on more). A room
wider than that leaves its middle open, and a room with an open cell counts as outdoors. Walls,
doors and furniture next to a roofed cell are under the roof too, so Unreal's roof has no holes.

## Rooms

A **room** is the regions of [regions.md](regions.md) joined across every border except doors. A
door is in no room; it is where two rooms meet (`roomsBesideDoor`). `Rooms::update()` runs at the
end of every step and does work only when a cell's passage or any building changed: a passage change
(wall, door, mined rock) rebuilds the rooms and the thin roof, a building that doesn't change
passage (a bed, a table) only recounts what is in each room. A rebuild walks the region graph and
looks at the cells of enclosed rooms only. Measured in the cloud: 40 walled rooms on a 1 km map,
including the region rebuild they cause, take 0.7 ms.

| | |
| --- | --- |
| `touchesEdge` | a cell on the map's edge: the outdoors, never roofed |
| `huge` | more than `RoomRules.hugeRoomCells` (2,000, ours) cells: the outdoors |
| `enclosed()` | neither of those; these get a roof, a cell list, contents and a role |
| `openRoofCount` | cells with no roof; `outdoors()` is edge, huge, or any open cell |
| `beds`, `tables`, `chairs`, `stoves` | buildings in the room, each counted once (`building.bed`, `isTable`, `isSittable`, `isStove`) |
| `role` | `None`, `Bedroom`, `Barracks`, `Kitchen`, `DiningRoom` |

**Roles** (`RoomRules.roleOrder` and `roles`, ours; RimWorld scores every role, we take the first
that fits): `Barracks` 2 or more beds, `Bedroom` 1 bed, `Kitchen` a stove, `DiningRoom` a table. So a
room with a bed and a stove is a bedroom, and one with a stove and a table is a kitchen. Only a room
that is not outdoors has a role. More roles (hospital, workshop, storeroom, recreation room,
prison) come with their buildings and need only a line in the rules and a count.

Asking: `sim.rooms().roomAt(cell)`, `roomOf`, `roleAt`, `indoors(cell)` (under a roof and not in an
outdoor room: what "slept outside" and "ate indoors" want), `cellsOf(room)`, `rooms()`. Room ids
last until the next change to the walls, like region ids; keep a cell and ask again.

## Unreal sees

`Snapshot::rooms` (enclosed rooms: id, role, size, bounds, outdoors, counts), `Snapshot::roofCells`
(the thin roof, in cell order) and `Snapshot::roofChanges`, which goes up when that set changes, so
the roof mesh is rebuilt only then. The thick roof is `Map::roof` over rock. `ThingView::doorOpen`.

## Saves and hashes

Save format 12: each door has `progress`, `hold`, `open`. Roofs and rooms are not saved; they are
rebuilt from the walls when a game loads (the thick roof from the seed's map). Door state is in
`Sim::stateHash`; rooms and thin roofs are not, because they are a function of what is.

## Not yet

- **Roof collapse**, roof and no-roof designations, building roofs as work.
- **Temperature, cleanliness, beauty and impressiveness** of a room (RimWorld's room stats); the
  `Room` struct is where they go.
- **Room ownership and bedroom assignment** (a bed's owner and the room it is in), prison cells.
- **Multi-storey**: rooms and roofs are for level 0 (floors above are roofs for the level below).
- **Mining and terrain edits**: the code that mines rock must call `Regions::markDirty` (it does
  already for the regions), and leaves `Roof::Thick` alone.
