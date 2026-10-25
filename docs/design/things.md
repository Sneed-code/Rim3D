# Things and the world layout

Code: `ColonySim/Things.h`, `Private/Things.cpp`, `ColonySim/Grid.h`, `ColonySim/Entity.h`, the
building layer in `ColonySim/Map.h`. Tests: `Tests/ThingsTests.cpp`, `Tests/HaulingTests.cpp`,
`Tests/MapTests.cpp`. Task S1.

## Cells

`CellPos{x, y, level}` names one 1 m cell. Level 0 is the ground; levels stack about 3 m apart.
Every grid and path function takes a `CellPos` (`Map::inBounds`, `walkable`, `terrain`,
`groundHeight`, `buildingAt`, `Pathfinder::findPath`). Only level 0 is stored so far: any other
level is out of bounds and not walkable, and a path to another level fails. Upper floors and stairs
(vanilla core) fill this in without changing the interfaces.

`Map::groundHeight(x, y)` and `terrain(x, y)` stay as ground-surface shorthands for the Unreal
terrain mesh. Everything else uses `CellPos`.

`MapSettings::flat` (and `SimSettings::flatMap`) makes an all-soil map at height 0 for tests.

## Things are entities

Pawns, items and buildings are entities in an [EnTT](https://github.com/skypjack/entt) registry
(v3.16.0, vendored in `ThirdParty/entt/`). `Things` owns the registry and keeps the map's building
layer in step with it; `Sim::things()` gives access. Each thing is made from a def of kind
`Things` (RimWorld's ThingDef, from the import) and its `category` decides what it is:

| Category | Components | From the def |
| --- | --- | --- |
| every thing | `Thing` (def id, category) | `category`: `Item`, `Building` or `Pawn` |
| `Item` | `StackCount`, `HitPoints` if the def has them | `stackLimit` (1 if absent), `stats.maxHitPoints` |
| `Building` | `Footprint`, `HitPoints` | `size` `[x, z]` (1x1 if absent), `passability` (`Impassable` blocks movement), `thingClass` (`Building_Door` makes a door), `stats.maxHitPoints` |
| `Pawn` | `Mover` (speed, path) | `stats.moveSpeed`, metres per game second (the importer converts it); 4 m/s at 1x if absent |

Plants are not things: they are a grid layer (task C2), because a 1 km map holds hundreds of
thousands of them. Projectiles and corpses come with combat (F2) and health (F1). Any other
category, or a def of another kind, makes nothing.

**Spawned or not.** A thing on the map has a `Position` (metres on a level: the centre of its
cell for items, of its footprint for buildings, anywhere for pawns, which move continuously).
Height comes from the map. A thing without a `Position` exists but is off the map, such as an item
being carried.

- `make(def, stackCount)` creates a thing off the map with its components.
- `spawnAt(thing, cell, rot)` puts it on the map. Items and pawns need walkable ground. A
  building's footprint starts at `cell` (its lowest x and y corner), East and West swap its width
  and depth, and every cell under it must be on the map, walkable ground (not water or rock) and
  free of other buildings.
- `spawn(def, cell, stackCount, rot)` does both.
- `despawn(thing)` takes it off the map; `destroy(thing)` despawns and deletes it.

Anything that fails returns `kNoEntity` (or `false`) and changes nothing.

**Building layer.** `Map::buildingAt(cell)` is the building covering a cell, or `kNoEntity`.
`Map::passage(cell)` says how that building affects movement: `Open`, `Blocked` (the def's
`passability` is `Impassable`, so walls block paths and destroying one opens them again) or `Door`
(the def's `thingClass` is `Building_Door`, as in RimWorld; walkable, but a region of its own, see
[`regions.md`](regions.md)). Only `Things` calls the setters.

## Rules for code that uses things

- **Determinism.** Entity ids come out of EnTT in a fixed order, so the same commands give the same
  ids. Don't depend on the order a view iterates: break ties by entity id (as the hauling
  placeholder does), or keep your own ordered list (as `Sim::haulers()`). Saves (S6) must restore
  the same ids.
- **Don't add or remove components on other entities while iterating a view of those
  components.** Collect first, then change.
- **Unreal doesn't include `Things.h`.** `Sim.h` only forward-declares `Things`, and Unreal will
  read things through the snapshot and events (`docs/design/bridge.md`). Grid headers include only EnTT's small `entity.hpp`, so
  `ColonySim.Build.cs` exposes `ThirdParty/` as a public system include path.
- Def ids inside `Thing` are valid only for the `DefDatabase` the sim was made with. The sim
  holds that database by `shared_ptr`; `Sim(settings)` without defs has an empty one.

## Stage 0 hauling placeholder

The Stage 0 speed test still runs on things: `SimSettings::pawns` pawns of `pawnDef` (default
`Human`) haul `items` items of `haulItemDef` (default `Steel`) to one stockpile cell. Carried items
are off the map until put down. Its components (`HaulJob` and friends) live in `Sim.cpp` and go when
the job system (C1) arrives. It gives the same hauls, path searches and cells expanded as the
Stage 0 version. Missing defs are listed in `Sim::setupErrors()`.

`simrun` loads defs from `--data` (default: the repo's `data/` folder). Runs with `--pawns` or
`--items` need `Human` and `Steel` in `data/defs/Things/`, which the RimWorld import (D3) provides.

## Not done here

- A thing list per cell (what items lie here). The job system's haul lists (C1) decide its shape.
- Rotation-aware interaction cells, multi-level buildings, roofs (C5).
