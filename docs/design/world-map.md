# World map

The planet the colony sits on: a grid of tiles with biome, climate, hills, rivers, roads and faction settlements, and the choice of a start tile. Sim only (`World.h`, `World.cpp`, defs in `data/defs/WorldGen/`, tests `WorldTests`). Caravans, other maps, quests and the Unreal screen come later.

## Shape

- **A wrapping grid, not RimWorld's icosphere.** `width` by `height` tiles (default 120 by 60 = 7,200); east wraps to west, the top and bottom rows are the poles, a tile has 8 neighbours (`World::neighbour`, directions N, NE, E, SE, S, SW, W, NW). A grid is simple to save, draw, path over and test; the cost is that tiles near the poles are really smaller, which does not matter for a game that walks caravans abstractly.
- **Made, not stored.** `World::generate(defs, seed, width, height)` is a pure function of the seed, the size and the defs. Only floats `+ - * /` and `sqrt` are used (no pow, exp or trig), so it is identical on Linux and Windows. A save needs only the seed, size and start tile; `World::hash()` fingerprints all of it for tests.
- **Not in `Sim` yet.** The world is its own object. The sim does not own one, so no save section was added. When it does (the stage 3 map generation task), store seed, size, start tile and whatever settlement state changes (goodwill, destroyed bases) in a `world` save section.

## What a tile has (`WorldTile`)

Elevation (m, negative under water), yearly mean temperature (°C), rainfall (mm a year), biome (index into `World::biomeNames()`, which is every `Biomes` def; `kNoBiome` for open water), hilliness (Flat, SmallHills, LargeHills, Mountainous, Impassable), river size and the direction it flows to, best road on its edges, water flag, settlement index.

## How it is made (all numbers in `WorldGen/Default`, all ours)

1. **Elevation:** four to five octaves of wrapping value noise. Sea level is the quantile that leaves `landFraction` (40%) as land. Land metres rise to `highestPeak` (4,500), the sea floor falls to `seaFloorDepth`.
2. **Temperature:** `equator - poleDrop * |lat|^1.5`, minus 6.5 °C per km of height, plus a few degrees of noise; water is moderated (x 0.9).
3. **Rainfall:** a wet band at the equator and one in the mid latitudes, dry subtropics between, times noise, plus a bonus near the coast, less in the cold.
4. **Biome:** `biomeRules`, tried in order, first match wins, by temperature, rainfall and a swampiness noise; a biome that does not exist falls back to temperate forest. Frozen water gets `waterRules` (sea ice).
5. **Hilliness:** a score from height and a ruggedness noise, cut at the `quantiles` of land tiles (52% flat, 24% small hills, 14% large hills, 9% mountainous, 1% impassable).
6. **Rivers:** water runs from every land tile to its lowest neighbour (a tiny per-tile jitter makes the order strict); a tile's river size is how much passes through it (creek, river, large, huge). A pit that a river runs into becomes a lake.
7. **Settlements:** `per100Land` (3) per 100 land tiles, at least `minSpacing` (3) tiles apart, on land that is not impassable or colder than -10 °C. Every faction def that is `humanlike` (not the Remnant or chitinids) gets a home tile; a settlement belongs to the faction whose home is nearest (with jitter, and pirates count half), so factions have territory. Names are syllables, unique.
8. **Roads:** from each settlement a Dijkstra over land (hills and big rivers cost more, impassable and water are blocked) to its two nearest settlements within a cost limit; every edge counts how many routes use it: 1 dirt, 2 stone, 4 asphalt.

## Choosing a start tile

`canStartAt(tile)`: land, a biome, not impassable (`startTile.maxHilliness`), no settlement. `chooseStartTile(tile)` sets it; `suggestStartTile(StartCriteria)` picks one (by seed) that is a given biome, in a temperature range, beside a river or a road (-1 when none). `siteFor(tile)` gives the map generator (S8) what it needs: biome name, hilliness, river size, road, temperature, rainfall, elevation and a `mapSeed` made from the world seed and the tile, so a tile always makes the same 1 km map.

## Check it

`simrun --seed 42 --world world.png --check` (CTest `simrun_world`, `simrun_world_seed7` at 180 by 90) prints the numbers and draws it: biome colours (darker for hills), rivers blue, roads brown to black, settlements red, the suggested start yellow. `--world-size WxH` sets the size.

## Next

Show the world on screen and pick the start tile there (Unreal); the sim owning a `World` and the S8 map generator reading `siteFor`; tile-by-tile faction goodwill and events; caravans travelling the roads; other maps made when visited. Faction relations to tiles (who owns what) are static for now.
