# Map generation 2

Code: `Private/Map.cpp` (`Map::generate`, `carveRivers`, `addWetlands`), `Private/MapFeatures.cpp`
(`placeMapFeatures`). Tests: `Tests/MapGenTests.cpp`, one in `SaveTests.cpp`. Task S8.

Look at it with `simrun --seed 42 --days 0 --png map.png` (1,024 m by default now; `--size 512`
for a quicker look; `--biome AridShrubland` for another biome).

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
mountain. Natural lakes get a shallow rim (the lowest 11% of the water band).

## Marsh and mud

On low ground (under 34% of the top height) where a slow wetness noise (48 m and 16 m) is high:
marsh, with mud where it is higher. Along any shore the threshold is lower. The biome's
`mapGen.marsh` scales the noise. Plants grow on marsh (`MarshyTerrain` fertility), not on mud or water.

Biomes (`data/defs/Biomes`, `mapGen`): temperate forest river 1 / marsh 0.6, boreal forest 1 / 0.8,
arid shrubland 0.5 / 0.1. `SimSettings::biome` (saved in the `world` section; replays have an
optional `settings.biome`) names the biome that the map, plants, weather and animals follow.

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

## Numbers

A 1,024 m map is generated in about 0.18 s (Release, one core). On a 512 m map from seed 42: about
1,700 cells of deep water, tens of thousands of shallow, marsh and mud as the tests check.

## Saves

The ground is regenerated from the seed, so **every save made before S8 on a map of 192 m or more
no longer loads**: it is refused with "the map generator gives a different map for this seed ...
this save can no longer be loaded". Saves and replays on maps under 192 m still load. Saves now have a
`world` section (version 1): the biome name.
