# Map generation 2

Code: `Private/Map.cpp` (`Map::generate`, `carveRivers`, `addWetlands`), `Private/MapFeatures.cpp`
(`placeMapFeatures`). Tests: `Tests/MapGenTests.cpp`, one in `SaveTests.cpp`. Task S8.

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

## Flat ground

sneed (2026-11-19): base building needs flat ground, so on a map of 192 m or more **most of the map is
open plain**, and the hills sit together in **a few big hill areas** instead of bumps everywhere
(`Map::generateFlatLand`; numbers are ours, at the top of `Map.cpp`).

- **The plain** lies 15 to 17 m up (1.8 to 3.8 m over the water table) and only rolls over hundreds of
  metres: under 6 degrees nearly everywhere, so walls and floors sit on it as they are. Measured on seeds
  42, 7 and 1 at 1 km: about 76% of 16 x 16 m plots of dry land are flat within 1 m (0.1% before), and
  about 15% of the map is hills.
- **Hill areas** come from a slow noise (260 m and 110 m). The share of the map they cover is
  `MapSettings::hills`, from the world tile's hilliness (`mapHillShare`): flat 15%, small hills 20%,
  large hills 25%, mountainous 30% (impassable 35%, though nobody starts there); with no world map, 15%.
  The share is ground standing over 2 m above the plain; the gentle foot slopes round it add about
  70% more (`kHillFootprint`). The cut is taken from the noise itself, so the share holds on every seed.
  A hill area rises from the plain over tens of metres to rolling hills up to 26 m high; the highest
  sixth of the hill share or more (about 4% of a flat tile, so every seed has stone and ore; only ground 8 m or
  more over the plain, always ground 14 m over it) is **bare rock** (a mountain with its own thick roof,
  mined for stone and ore; ore lumps follow the rock, as in RimWorld, so there is less than on the old
  maps, which were 15 to 30% rock).
  No sheer cliffs: under 1% of the walkable ground is steeper than 0.6 (31 degrees), the steepest about 1.2
  (a short lake or hill side); the old maps had walkable drops of 10 m in a metre.
- **Lakes**: about 2.5% of the map (times the biome's river, at most 1), a few round lakes sunk into
  the plain away from the hills, with sand up to 1.2 m over the water, shallows, then deep water.
- **The middle stays clear**: hills and lakes keep about 150 m away from the middle of the map,
  where colonists land (a river may still cross it).
- Marsh and mud form on the plain's lowest ground (under 15.5 m) where the wetness noise is high,
  and round lakes and rivers.

Tests: `MapGenTests.cpp` "mostly flat open ground with no cliffs" (at least 75% of walkable ground
under 0.1 slope, under 1% over 0.6, hills 10 to 19%, steepest walkable under 1.5; shores left out), "a few big patches" (at most 12 hill
areas of 2,000+ cells holding 90% of the hills), "the middle ... is flat and open" and "a hillier
world tile gets more hills". Maps under 192 m (the test maps) are unchanged.

## Rivers

One river on a map, two on a map of 768 m or more. A river runs from one edge to the other (along x
or y, from the seed; the second runs the same way, at least 30% of the width away). Its middle line
wanders (noise at 200 m and 50 m), and its width varies: the deep channel is 0 to 3.5 m each side,
with 2 to 4 m of shallow water beside it, times the biome's `mapGen.river`. Each river has a **ford**
(no deep water for about 30 m) at a place from the seed, so the land is never cut in two. The bed
is cut into the ground and the banks slope back up over 8 m (6 m before flat ground), wider where the ground stands higher (never
steeper than 0.3 on average, up to 60 m wide); a river cuts a valley through a hill or a mountain. Since water became a surface ([water.md](water.md)) the shallows' bed rises to just under the water table at
the shore, the banks stop 0.15 m above it, and every river cell has a current. Natural lakes get a shallow rim (the lowest 11% of the water band).

## Marsh and mud

On low ground (under 15.5 m, the plain's lowest third and lakesides; 34% of the top height before flat ground) where a slow wetness noise (48 m and 16 m) is high:
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

## Roads

A map made for a world tile with roads gets them, with old stone bridges where they cross water: see
[roads-bridges.md](roads-bridges.md). Laid after the ruins and geysers.

## Numbers

A 1,024 m map is generated in about 0.18 s (Release, one core). On a 512 m map from seed 42: about
1,700 cells of deep water, tens of thousands of shallow, marsh and mud as the tests check.

## Saves

Flat ground (2026-11-19) changed the generator again: saves on maps of 192 m or more made before it
are refused the same way.

The ground is regenerated from the seed, so **every save made before S8 on a map of 192 m or more
no longer loads**: it is refused with "the map generator gives a different map for this seed ...
this save can no longer be loaded". Saves and replays on maps under 192 m still load. 
