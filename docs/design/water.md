# Water

sneed (2026-11-16): water and swimming are part of the game, and should look and behave like real
water, not a flat coloured floor that bodies clip through. Rivers flow, bodies wade and swim, and the
water reacts to them. It must stay light on the PC.

The sim decides where water is, how deep, how fast it flows and what it does to bodies. Unreal draws
a surface from that and makes it react to what moves in it. Nothing in the drawing changes the game.

## Sim (built, cloud)

Code: `Map::waterSurface`, `Map::waterDepthAt`, `Map::flowAt` (`Map.h`), `Water.h`/`Water.cpp`, the
movement in `Movement.cpp` and `MovementSystem.cpp`. Tests: `Tests/WaterTests.cpp`.

**One water table.** Every lake, river and shallow rim on a generated map has its surface at one height,
the water table: 22% of the top height, 13.2 m (the old line between water and land, so no cell
changed). A cell's depth is the table minus the ground under the point (the same corner heights the
terrain mesh is drawn from). Lake rims run 0 to 1.5 m deep, deep lake water deeper.

**Rivers** (`carveRivers`) were changed so the water table fits them: the shallows' bed rises from 12 m at
the channel's edge to 0.25 m under the surface at the shore, the deep channel stays at 10.2 m (3 m of
water), and the banks slope down to 0.15 m above the surface instead of to 12 m, so no dry ground lies
below the water's level (a dry pit beside a river would show a wall of water). A ford is waist to chest
deep in the middle. Terrain is unchanged; heights on maps of 192 m and up changed.

**Flow.** Each river cell has a current along the river's course (its direction from the seed, turned by
the bends of its middle line): 1.0 m/s in the middle, 0.35 m/s at the edge of the shallows (ours: a
lowland tropical river; real seconds at 1x, so a thirtieth of that a step, against a man's walk of 4.6 m/s). Lakes and marsh are still. Flow is made from the seed with the map and not saved
(like geysers). `Map::setFlow` sets it by hand for tests.

**Marsh** has a 0.1 m film of standing water over its ground (ankle-deep for a person). Mud has none.
Water put down by hand (tests, flat maps), or anywhere above the table, stands 2.5 m (deep) or 0.5 m
(shallow) over its own ground.

**Bodies.** How deep water is for a pawn depends on its height (`Sim::bodyHeightM`: a person 1.80 m times
their looks' height, an animal 0.9 m times the square root of its body size, ours). By depth as a share
of that height (`WaterBand`):

| Band | Depth | Speed | Current carries |
|---|---|---|---|
| Dry | under 3% | the ground's | none |
| Ankle | 3% to 30% | water cells: RimWorld's shallow water 30; marsh and mud keep their 14 | none |
| Waist | 30% to 60% | RimWorld's shallow water 30 (30% of a walk) | 25% |
| Chest | 60% to 80% | RimWorld's chest-deep moving water 42 (24%) | 50% |
| Swimming | 80% and over | 20% of a walk (about 0.9 m/s for a man), slope ignored | all |

The current adds to or takes from a wader's or swimmer's pace along its heading (never below a quarter of
it), so walking downstream is quicker than up. Water at least 0.3 m deep puts out a burning body.

**Swimming.** Deep water still can't be walked, and the pathfinder still never plans through it, so other
pawns wade rivers at the shallows and fords but don't swim across. **The player's colonist can swim**:
`DirectMoveCommand` counts deep water as open (`swimmerFitsAt`). A swimmer can't roll, shoot or fight
(the commands fail). A pawn in deep water that isn't controlled (let go mid-swim) swims for the nearest
walkable cell within 24 m, carried by the current, and walks on from there.

**Snapshot.** `ThingView::waterDepth` (metres over the ground at the pawn's feet; the surface is
`z + waterDepth`) and `ThingView::swimming`. Unreal reads `Map::waterSurface`, `Map::waterTable` and
`Map::flowAt` to build and animate the surface.

## Unreal, W1 as built (PC, 2026-11-17)

- `USimWaterSubsystem` (`SimWaterSubsystem.h/.cpp`; the engine's Water plugin owns the name `WaterSubsystem`) builds a
  surface mesh per 32 m chunk over every Water and ShallowWater cell with a surface, plus **two cells of margin** under
  the bank. A vertex stands at the cell's `waterSurface`; on the margin's outer edge it dives 0.15 m under the ground (a
  skirt), so the surface is always cut by the bank and never ends in the air. UV 0 is the map position in metres, UV 1 the
  flow (m/s), UV 2.x the depth over the ground (negative under the bank). Marsh is left to the terrain material. A few
  chunks a frame (`Water.ChunksPerFrame`, 4) are hashed and remade when their open water changed (a moisture pump).
  `GetStats()`: chunks, triangles, vertices, full build and rebuild ms. Seed 42, 512 m rainforest: 31 chunks, 16,626
  triangles, built in 25 ms.
- `M_Water` from the **WaterArt commandlet** (`-run=WaterArt`, needs the terrain's `T_water_normal`, so run
  TerrainMaterials first): Single Layer Water; absorption (0.9, 0.6, 1.6)/m and scattering (0.05, 0.06, 0.035)/m (ours,
  murky green-brown, nearly clear in the first 20 to 30 cm); two ripple normal layers (3.2 m and 1.3 m, the second turned
  a quarter) advected along the flow with the two-phase flow-map blend (4 s cycle), a slow wind drift on still water,
  ripples stronger with speed; foam from the vertex depth at the shore (under 0.2 m) and where flow is over 0.7 m/s,
  through the Opacity pin; refraction 1.33; reflections from Lumen. All numbers are material parameters.
- The terrain paints Water/ShallowWater cells as a **wet bed** (dark mud and gravel, wet sand in the shallows) while the
  surface is drawn; `Water.Enable 0` (or no `M_Water`) brings back the old opaque water colour.
- **Bodies (W4 minimum):** `WaterDraw.h`: a swimming pawn or animal is drawn floating, its feet 80% of its body height
  (`Sim::bodyHeightM`) under the surface, eased in and out; the protagonist's camera follows the floating body. Not yet
  seen in a smoke test (compiled only).
- Cost (smoke test `water`, 1080p offscreen, river scene): GPU 5.2 ms without the surface, 5.5 ms with it (+0.3 ms);
  render thread +0.3 ms; game thread unchanged.
- Left: the water still reads a little flat and opaque from above (tune absorption/ripple strength in play); the
  shoreline follows the cell-sized bed heights, so diagonal banks show small steps; the seed-42 512 m map has no still
  lake (776 still cells, all lake rims), so the `lake` shots show the river; cover leaves on the shallows are the terrain's.

## Unreal (planned, PC)

How real water is done in today's games, and what we do. A true 3D fluid simulation (every litre of water
simulated, as in film) costs 5 to 20 ms a frame for a tank a few metres across, so no game does it for
lakes and rivers. Instead, a few cheap layers together look and behave real:

1. **The surface (W1).** A mesh at the water's height over every wet cell (32 m chunks, like the
   terrain), drawn with Unreal's Single Layer Water shading: light is absorbed and scattered by depth
   (clear at the edge, murky green-brown in the deep middle, as tropical water is), the ground shows
   through and bends (refraction), and the sky and trees reflect (Lumen, or the ray tracing already on).
   Ripples are normal maps that move with each cell's flow vector (a flow map, blended in two phases so
   it never stretches), faster and rougher where the river runs fast, with foam at the shore and in
   fast water. Banks get a wet band just above the surface. Marsh becomes wet ground with puddles in the
   terrain material. We use the shading of Unreal's Water plugin, not its water bodies: those are built
   around the Landscape and splines, and our ground is our own mesh made from the sim.
2. **The water reacts (W2).** A small simulation of waves (a wave equation in a 256 by 256 texture that
   follows the camera over about 64 m) runs on the graphics card. Every body in the water pushes it each
   frame, by its size and speed: a swimmer leaves a V-shaped wake, a wader rings that spread and bounce,
   a thing dropped in a splash ring. The surface reads it for its ripples (and, close up, a little real
   height). This is what makes bodies and water touch instead of clip.
3. **Splashes and wet bodies (W3).** Niagara spray and droplets when a body jumps or falls in, at each
   step in the shallows, at each swim stroke and where a bullet hits water; the sounds (`Step_Water`) by
   depth. Skin and clothes darken and shine when wet and dry over a minute or so; drips as a body
   climbs out.
4. **Swimming bodies (W4).** Treading water and a swim stroke (from the Mixamo packs), the body floating
   with its shoulders at the surface and bobbing gently; wading slows the hips and swings the arms
   higher. In first person, an underwater look (fog, colour lost with depth, light patterns) when the eye
   goes under.
5. **Floating things (W5).** Once the loose-props physics from the animation work is in: light props
   float and drift with the flow (a few sample points per prop read our surface height and current).

**Cost on the PC.** About 1 to 1.5 ms a frame together at 1440p on the RTX 5080: the surface is one
material over a few chunks, the wave texture is tiny, splashes live a second. Only water near the camera
gets the wave simulation and splashes; far water keeps its flowing ripples. A Water quality setting
(Low: no wave simulation, simpler reflections) goes in the graphics options.

## Not yet

- Other pawns swimming across deep water (needs the pathfinder to know terrain costs, movement.md).
- Drowning, and downed pawns in deep water.
- Things that fall in drifting in the sim, fishing, waterfalls and rapids (the table is flat; a river
  over 1 km would fall about a metre, too little to see).
- The current pushing a swimmer sideways in the sim (it speeds or slows it along its way only); the
  player's colonist will drift in Unreal (W4) and the sim accepts its moves.
- Watermills on generated rivers: `Map::setFlowing` is still never set by the map generator.
