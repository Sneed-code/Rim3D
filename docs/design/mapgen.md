# Map generation 2

Code: `Private/Map.cpp` (`Map::generate`, `carveRivers`, `addWetlands`), `Private/MapFeatures.cpp`
(`placeMapFeatures`), `Private/MapProps.cpp` (`placeMapProps`). Tests: `Tests/MapGenTests.cpp`, one in `SaveTests.cpp`. Task S8.

Look at it with `simrun --seed 42 --days 0 --png map.png` (1,024 m by default now; `--size 512`
for a quicker look).

## Ground

`Terrain` has three new values after the old five: `ShallowWater`, `Marsh`, `Mud`. `Water` is
**deep water**: nobody crosses it. The others can be walked, slowly: the path costs are the
`Terrain` defs' (`terrainPathCost`: shallow water 30, marsh and mud 14, sand 4, soil 2), and a step
costs 7.7% of its time per point, so a pawn wades at about 30% speed and trudges through marsh at
about 48%. The pathfinder does not know these costs yet (a route can wade where it could walk
round); only walking speed does. Giving the region route a terrain cost is a later task.

Maps **under 192 m** (the test maps) are made exactly as before S8: no shallow shores, rivers, marsh
or mud, so the old tests, replays and `Tests/saves/` stay valid.

## Rivers

One river on a map, two on a map of 768 m or more. A river runs from one edge to the other (along x
or y, from the seed; the second runs the same way, at least 30% of the width away). Its middle line
wanders (noise at 200 m and 50 m), and its width varies: the deep channel is 0 to 3.5 m each side,
with 2 to 4 m of shallow water beside it, times the biome's `mapGen.river`. Each river has a **ford**
(no deep water for about 30 m) at a place from the seed, so the land is never cut in two. The bed
is cut into the ground and the banks slope back up over 6 m; a river cuts a valley through a
mountain. Since water became a surface ([water.md](water.md)) the shallows' bed rises to just under the water table at
the shore, the banks stop 0.15 m above it, and every river cell has a current. Natural lakes get a shallow rim (the lowest 11% of the water band).

## Marsh and mud

On low ground (under 34% of the top height) where a slow wetness noise (48 m and 16 m) is high:
marsh, with mud where it is higher. Along any shore the threshold is lower. The biome's
`mapGen.marsh` scales the noise. Plants grow on marsh (`MarshyTerrain` fertility), not on mud or water.

Biomes (`data/defs/Biomes`, `mapGen`: `river` and `marsh` multiples): temperate forest 1 / 0.6, boreal
forest 1 / 0.8, tundra 0.8 / 0.5, cold bog 1 / 1.5, temperate swamp 1 / 1.6, tropical rainforest 1.2 / 1,
tropical swamp 1.2 / 1.8, arid shrubland 0.5 / 0.1, desert 0.2 / 0, ice sheet 0.2 / 0, extreme desert
and sea ice none. The biome is the sim's (`Sim::biome()`: the world tile's, else temperate forest).

## Ruins and geysers

`placeMapFeatures` runs when a new game's map is made (not on a flat map, not under 256 m, not when
loading: the things are in the save). It uses its own random stream. One ruin for every 130,000
cells (2 on 512 m, 8 on 1 km) and 1 + one per 260,000 cells geysers. A ruin is the outline of a room (7 to 13 by 6 to 10
cells), sometimes with an inner wall, each wall cell standing 68% of the time, with two doorways
cut out: `AncientWall` buildings (200 hit points, hold a roof, so a player who closes the gaps
gets a roof-able room). A geyser is a 2x2 `SteamGeyser` building that cannot be walked over, on flat open
ground, preferably within 10 m of rock. Both keep 45 m clear of the map's middle and 25 m from each other, and sit
on flat soil or sand. Steam power and eruptions come with the events later; for now a geyser is a landmark.
Neither def has a `designationCategory`, so nobody can build them.

## Ground props, wreckage, ancient danger and roads

`placeMapProps` (`Private/MapProps.cpp`, tests `Tests/MapPropTests.cpp`) runs right after
`placeMapFeatures`, on the same maps (new games, not flat, 256 m or more), with its own random stream.
Its numbers are the `MapPropRules` def (all ours: RimWorld scatters chunks and puts an ancient danger
on most maps but gives no numbers). Everything it puts down is a normal thing in the save. In order:

- **Road**: when the world tile has a road (`Sim::site()->road`), a strip of road floor runs from one
  map edge to the other, along x or y, its middle wandering on two slow waves. Dirt road: 3 cells of
  `PackedDirt`; stone road: 4 of `Flagstone`; asphalt: 5 of `BrokenAsphalt` (`roadWidth`). It stops at
  water and rock and clears the plants on it. Road floors (`mapRoad` in the Terrain def) can't be laid
  from a floor plan. Unreal draws ground floors as tinted gravel (their `visual.color`; laid floors
  without one are grey-brown), which floors never had before.
- **Ancient danger**: one sealed room per 600,000 cells (at least one: 1 on 512 m, 2 on 1 km), 9 to 15
  by 7 to 11 cells, on flat open ground at least 45 m from the middle and 6 cells from any building.
  A full outline of `AncientWall` with no gap; Rooms roofs it like any closed room. Inside: 2 to 4
  `AncientCryptosleepCasket` buildings in a row along a long wall, and the `AncientDanger` loot table
  (survival meals and valuable stacks) on the floor of the far half. Getting in means taking a wall
  down. **Not yet**: the sleepers in the caskets (hostile or friendly) and opening them.
- **Wreckage**: one `ShipChunk` (2x2) per 350,000 cells (at least one), 45 m from the middle, sometimes
  (35%) a second one a few metres off, with 2 to 5 `ChunkSlagSteel` items around. Taking a ship chunk
  down gives the wiki's 20 steel and 5 or 6 components (its `costs`, half back); an electric smelter
  turns a slag chunk into 15 steel (`SmeltSlag`).
- **Boulders**: a group of 1 to 3 per 14,000 cells (about 75 groups on 1 km), 60% of them near rock,
  of the rock next to them (else one of the map's rock types): `Boulder<Rock>` buildings, impassable,
  75% cover. Taken down, one chunk of that rock.
- **Fallen logs**: one per 9,000 cells on soil with a tree within 5 m: `FallenLog`, 3 by 1, along x or
  y. Pawns climb over (path cost 42), 40% cover, burns; taken down, 25 wood.
- **Stone chunks**: a cluster of 2 to 6 per 16,000 cells within 4 cells, 60% near rock; the chunk of the
  rock beside them, else one of the map's rock types.

Small props keep 20 m off the middle, big ones 45 m; nothing goes on a road, a building, an item, water,
marsh or rock. Boulders, logs, wrecks and caskets remove the plants under them.

## Numbers

A 1,024 m map is generated in about 0.18 s (Release, one core). On a 512 m map from seed 42: about
1,700 cells of deep water, tens of thousands of shallow, marsh and mud as the tests check.

## Saves

The ground is regenerated from the seed, so **every save made before S8 on a map of 192 m or more
no longer loads**: it is refused with "the map generator gives a different map for this seed ...
this save can no longer be loaded". Saves and replays on maps under 192 m still load. 
