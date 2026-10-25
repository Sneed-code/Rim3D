# Terrain, roofs and plants on screen

Code: `game/Source/Rim3D/TerrainSubsystem.*`, `TerrainDraw.h`. Smoke test: `world`
(`tools/smoke.ps1 -Test world`). Task U3. Unreal side only; the sim is untouched except that
plant defs got a `visual` field and `USimSubsystem` counts how often the sim is replaced.

## What it is

`UTerrainSubsystem` (one per game world) draws what the sim says about the ground. It starts by
itself whenever a sim exists and rebuilds everything when the sim is replaced (a new start or a
load, `USimSubsystem::GetSimGeneration`). One actor holds, for each 32 m chunk of the map:

- a **procedural mesh** with two sections: the ground and rock (section 0) and the roofs (section 1);
- **instanced plant meshes**, one component per plant kind that grows in the chunk.

A 1 km map is 32 x 32 chunks. Chunks are separate components so that mining a cell rebuilds one
chunk and not the whole map.

## The ground

A vertex per cell corner at the sim's ground height (`Map::groundHeight`), so pawns, items and
plants standing at `Map::heightAt` stand on the mesh. Colours are flat vertex colours from the
terrain type until the Megascans materials (A2): water, sand, soil, rough stone. Soil is tinted by
the fertility the plants use (gravel, soil, rich soil). The shared engine material
`VertexColorMaterial` draws them (it shows both sides of a face, so roofs and rock sides need no winding care).

**Rock stands on the ground as a solid block**, `TerrainDraw::RockHeightM` (4 m) tall, a flat top
that follows the ground height and sides wherever the neighbour is not rock. The sim keeps no rock
height (its ground height under rock is the hillside itself), so the block is only the picture; the
sim's cover and sight already treat rock as the tallest obstacle. **Mining** turns the cell into
rough stone at ground level: the block goes, the tunnel's walls are the sides of the blocks around
it. The CellMined event rebuilds the chunk and the chunks of the four neighbouring cells (their
blocks grow or lose a side).

Picking and the overseer camera know about the blocks (`TerrainDraw::SurfaceBonusCm`): a click
on top of a rock block picks the rock cell, and the camera does not dip into a block or into the
mountain's roof.

## Roofs

Section 1 of each chunk. Two kinds, both drawn as a slab seen from above and below:

- **Thick** (`Map::roof == Thick` over a cell that is no longer rock): the mountain's roof over mined
  rock, level with the top of the rock blocks, so a mined tunnel is closed from above.
- **Thin** (the snapshot's `roofCells`): the roof C5 puts over enclosed rooms, `RoofHeightM` (3 m)
  above the ground. `Snapshot::roofChanges` says when the set changed; the terrain compares it with
  its own copy and rebuilds only the chunks whose cells differ.

**Roofs are hidden while the overseer view is the one in use** (so the colony can be managed from
above) and shown in any other view (direct mode, first person). `Terrain.Roofs` overrides it:
-1 automatic (the default), 0 hidden, 1 shown.

## Plants

Read straight from `sim.plants()`, not from the snapshot (docs/design/plants.md said U3 would
decide). A full copy of 119,000 plants each step would be wasteful, and plants change slowly, so:

- A plant's look is its def's `visual` (below). Each plant kind gets one static mesh made when the
  sim starts (cones, cylinders and low spheres with their colours baked into the vertices, lit by
  the shared `VertexColorMaterial`). A def with `visual.mesh` (an asset path, A1's job) uses that
  mesh instead.
- Each plant is one instance. Where it stands in its cell, which way round it faces and a few
  percent of size come from a hash of the cell, so a forest isn't a grid; none of it is sim state.
- A plant is drawn at one of six sizes between a sprout (20%) and fully grown. A chunk's plants are
  rebuilt when one of them steps to another size, dies, is felled, picked or built over.
- How a chunk finds out: the sim's `PlantRemoved` and `PlantHarvested` events mark it at once;
  everything else (growth, death, a building going up, a plant placed by a dev tool) is found by a
  scan that works out a signature of each chunk's plants, `Terrain.PlantScan` (24) chunks a frame,
  so a 1 km map is covered in about 45 frames. The scan costs about the same whether or not
  anything changed and needs no events.
- Grass and flowers have a cull distance (90 m for grass), so a hundred thousand tufts are never
  all drawn. Trees are drawn out to 1.5 km. Anything under 1.5 m tall casts no shadow.

### The `visual` field of a plant def

```json
"visual": {
  "stem":  { "shape": "cylinder", "radius": 0.28, "height": 2.6, "color": [0.30, 0.20, 0.12] },
  "crown": { "shape": "sphere", "radius": 2.1, "height": 3.4, "lift": 1.8, "color": [0.16, 0.34, 0.12] },
  "cullDistance": 1500
}
```

`shape` is `cylinder`, `cone` or `sphere`; `radius`, `height` and `lift` (how far the part's bottom
is above the ground) are in metres at full growth; `color` is red, green, blue from 0 to 1. Both
`stem` and `crown` are optional; a plant with neither gets a small green cone so it still shows.
`cullDistance` is in metres (0: never culled), `scale` and `jitter` (0.15) change the size.
`mesh` is an asset path for A1's real meshes; if it does not load, the shapes are used.

## Upscalers (DLSS and later)

Everything here uses Unreal's ordinary mesh and material path: static meshes, procedural meshes and
instanced meshes with a normal lit material, no custom render passes, no screen-space tricks, no
materials that move vertices without motion vectors. That keeps NVIDIA's DLSS plugin (and later
upscalers) possible to switch on. Later art tasks should keep to it: no world-position-offset
animation without a velocity, no dithered fake transparency drawn as noise (the cull-distance fade of
grass is the one dither in use, and it is ordinary).

## Console and numbers

- `Terrain.Enable` 1 or 0, `Terrain.Roofs` -1, 0 or 1, `Terrain.PlantScan` chunks a frame.
- `TerrainDraw.h`: `RockHeightM` 4, `RoofHeightM` 3 (walls will be drawn that tall by U4), `ChunkCells` 32.
- `UTerrainSubsystem::GetStats()` says what is drawn and what rebuilds cost; the smoke test writes
  them to its notes.

## Not here yet

Megascans materials and slope blending (A2); terrain defs replacing the four placeholder terrain
types (the colours and fertility change when they do); water as a surface rather than a coloured
floor; shadows of roofs on the floor below (there is no light model for interiors yet); fire, snow
and weather; collision (nothing uses Unreal collision, picking reads the sim).
