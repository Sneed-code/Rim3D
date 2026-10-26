# Regions and reachability

Code: `ColonySim/Regions.h`, `Private/Regions.cpp`, the passage layer in `ColonySim/Map.h`.
Tests: `Tests/RegionsTests.cpp`. Picture: `simrun --regions FILE`. Task S2.

## What they are

As in RimWorld, walkable cells are grouped into **regions**:

- A region is a 4-connected patch of walkable cells inside one **16 m block** (`Regions::kBlockSize`).
  It never crosses a block boundary, so a 1 km map has at most 4,096 blocks and a changed cell
  rebuilds only the regions of its own block.
- A **door** cell is a region of its own (`RegionKind::Door`), one cell each. Doors are where rooms
  stop ([rooms.md](rooms.md)).
- Cells that can't be walked (water, rock, walls) have no region.

Regions that share at least one cell side are **linked**. A `RegionLink` holds the other region,
how many cell sides they share, and the middle of that shared border in metres, which the
region-graph search ([pathfinding.md](pathfinding.md)) uses as the crossing point. Both regions hold the link, with the same
numbers, sorted by the other region's id. Regions that only touch at a corner are not linked: the
pathfinder never cuts corners, so a diagonal step between them always passes a linked cell.

Linked regions form **reachability groups**: every region a pawn can walk to from another. "Can a
pawn walk from A to B?" is `canReach(a, b)`, which is two lookups. Work scanners use it so they
never pick a target nobody can reach, and S3's pathfinder uses it to reject impossible requests
before searching. `groupCellCount(group)` tells the size of an area (an island, a sealed room).

```
Regions& regions = map.regions();          // or sim.regions()
RegionId r = regions.regionAt(cell);       // kNoRegion if the cell can't be walked
const Region& info = regions.region(r);    // kind, cellCount, bounds, anchor, group, links
bool ok = regions.canReach(from, to);
```

Everything takes a `CellPos` with a level. Only level 0 exists so far; other levels have no
regions. Stairs (vanilla core) will be links between regions on different levels, so the graph and
the groups need no change.

## When they are rebuilt

`Map::setBuilding` tells the regions when a cell's **passage** (`Open`, `Blocked`, `Door`)
changes; a bed or table, which doesn't change passage, costs nothing. The regions mark the cell's
block dirty and rebuild nothing yet. Every query (`regionAt`, `groupAt`, `canReach`,
`regionCount`...) first rebuilds the dirty blocks, so queries always see the map as it is now and a
pawn building twenty walls in one step causes one rebuild per block, at the next query.
`update()` does it on purpose.

A rebuild:

1. Removes the old regions of every dirty block, and their links from the regions next door.
2. Floods each dirty block again in row order: a door cell becomes a one-cell door region, any
   other walkable cell starts a 4-connected flood over the walkable, non-door cells of the block.
3. Links every cell side between two different regions that has a rebuilt block on at least one
   side. Links between two untouched blocks are still in place.
4. Relabels every reachability group with a flood over the region graph in region id order.

The first query builds every block (a `Sim` does that when it is made). Measured in the cloud: a whole 1 km map takes about 30 ms
(about 3,400 to 3,700 regions), a 512 m map about 7 ms. Rebuilding one block, groups included,
takes about 40 microseconds on a 1 km map. Step 4 is the only part that grows with the map; if a
profile ever shows it, relabel only the groups the dirty blocks touched.

## Ids and determinism

- Dirty blocks rebuild in block order, and new regions take the **lowest free id**, so the same
  map and the same changes always give the same ids, whenever the queries happen. Regions in
  untouched blocks keep their ids; an unchanged block that is rebuilt gets its ids back.
- Region ids and group ids are only good until the map changes. Don't keep them across steps;
  keep a cell and look its region up again. Group ids are renumbered on every rebuild.
- `region(id)` doesn't rebuild, so its reference stays valid until the map changes and something
  queries again.
- Link border middles are sums of whole and half metres, exact in floats, so they are the same on
  every compiler.

## The picture

`simrun --seed 42 --size 512 --days 0 --regions regions.png` writes one colour per region, so the
16 m blocks and the cuts inside them show. Regions outside the biggest group are drawn dark, doors
white, and cells with no region in a dim version of their terrain (dark grey rock, dark blue
water, dark red for a wall). It also prints the region count and group count. The sim builds every
region when it is made (since S3), so the build time is part of `mapGenMs`. CI draws a 128 m one on
every run (`simrun_regions` test).

## Not done here

- **Levels** (M1, M2): every level has its own regions; stairs and ladders link the regions at their two
  ends with the climb's extra cost (`RegionLink::extra`, `Region::climbs`); see [levels.md](levels.md).

- **Who may pass a door.** Every pawn can pass every door for now (they open and cost time, [rooms.md](rooms.md)). Raiders can't open colony doors
  in RimWorld (they bash them), and forbidden doors stop colonists; that becomes per-faction
  reachability when the raid comes (F6).
- **Rooms** (C5) are built on regions and live in [rooms.md](rooms.md): regions joined across every border except doors, with a roof and a role.
- **Cover and danger** don't live here; the cover map (F3) has its own dirty tracking.
- Terrain that changes (mining, building floors over water) must call `Regions::markDirty`; the
  map does it for buildings now and will for terrain when terrain can change.
