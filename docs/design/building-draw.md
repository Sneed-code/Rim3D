# Buildings and items on screen

Code: `game/Source/Rim3D/BuildingSubsystem.*`, `VisualMesh.*`. Smoke test: `buildings`
(`tools/smoke.ps1 -Test buildings`). Task U4. Unreal side only; the sim is untouched. The data
grew `visual` fields on the building and item defs (below).

## What it is

`UBuildingSubsystem` (one per game world) draws every building, blueprint, frame and item the
snapshot lists. It starts by itself whenever a sim exists and starts over when the sim is replaced
(`USimSubsystem::GetSimGeneration`: a new start or a load). Nothing is kept in Unreal objects that the
sim doesn't know about, so a load always draws the right picture.

- **Finished buildings** are meshes made from the def's `visual.parts`, coloured by the stuff they are
  made of (steel grey, wood brown; a stuff def's `visual.color`).
- **Blueprints** are see-through blue ghosts of the whole building.
- **Frames** (some materials delivered, work under way) are a solid, part-built stage inside the ghost
  of the whole: the building's mesh squashed to a fraction of its height, from 10% with nothing
  delivered up to the full height as the work is done (a quarter of the height comes from the
  materials delivered, three quarters from the work). Heights come in ten steps.
- **Doors** are a fixed frame plus a leaf that slides sideways into the wall as the sim's door opens
  (`ThingView::doorOpen`, blended between the last two snapshots), and slides back when it shuts. Which
  way the leaf runs comes from the walls beside the door; with none it follows the door's facing.
- **Items** (anything on the map that is an item, in stockpiles or not) are pieces in a pile. A def's
  `visual.pieces` is how many pieces a full stack shows; a stack of one shows one, a full stack
  all of them, in between by the fraction of the stack limit. Pieces sit in a 2 x 2 square, then on
  top, with a small hash of the thing's id for where in the cell and which way round, so a pile isn't
  a grid. Items are not drawn beyond 150 m.

Carried items are not on the map, so they are not drawn here (the pawn's hands show them; U5).

## Cost: instanced meshes per chunk

Like the terrain the map is cut into 32 m chunks. A chunk has one `UInstancedStaticMeshComponent` for
each kind of thing in it: a building kind made of one stuff (and for ghosts, frames and door leaves a
variant of it), an item kind. A new snapshot (one per step) is read once; each chunk gets a signature
summed from the things in it (id, def, stuff, stage, rotation, frame height step, a door's axis, an
item's piece count). **Only chunks whose signature changed are rebuilt**, so a wall going up or a pawn
dropping steel costs one chunk, and a quiet map costs one pass over the snapshot's things (a
few microseconds per thousand). Doors are the one thing that moves every frame: the leaf instance is
updated in place while it is opening or closing.

Meshes are made when a kind is first met (one per def and stuff, so a steel wall and a wood wall are two).
The material is the same engine `VertexColorMaterial` as the terrain. The ghosts use `M_BuildingGhost`
(unlit, translucent, two sided, with a Color and an Opacity), one dynamic instance of it. **It is made by
a commandlet and is not in Git**: run
`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=BuildingMaterials -unattended -nosplash -nullrhi`
once on each machine (`assets/manifest.json`). The engine's own translucent materials would be simpler, but
they are not marked for instanced meshes, and the game draws them as the dark default material. Without the
generated material the game still runs and draws plans solid, with a warning in the log.

Shadows: buildings cast them, items and ghosts don't.

## The `visual` field of a building or item def

```json
"visual": {
  "tint": [1, 0.9, 0.7],
  "pieces": 6, "layer": 0.1,
  "parts": [
    { "shape": "box", "size": [0, 0, 3.0], "stuff": true },
    { "shape": "cylinder", "radius": 0.2, "height": 0.03, "at": [-0.6, 0, 0.91], "color": [0.75, 0.3, 0.1] },
    { "shape": "box", "size": [0.12, 0, 2.2], "at": [-0.44, 0, 0], "stuff": true, "corners": [0.07, 0.07] },
    { "shape": "box", "size": [-0.26, 0.1, 2.15], "slide": 0.85, "stuff": true }
  ]
}
```

All lengths are metres, in the thing's own frame facing north: x east, y north, z up from the ground,
the origin the middle of its footprint (the `size` of its def, 1 x 1 for an item).

| Field | Means |
| --- | --- |
| `shape` | `box` (default), `cylinder`, `cone`, `sphere` |
| `size` | box only: width x, depth y, height z. A width or depth of 0 or less is the footprint's length plus that number: 0 fills the footprint, -0.2 leaves 10 cm bare on each side |
| `radius`, `height` | cylinder, cone, sphere |
| `at` | `[x, y, z]`: where the bottom centre of the (unrotated) part is |
| `rot` | `[pitch, yaw, roll]` degrees, about the part's own middle (a log lying down is `[90, 0, 0]` with `at` z lowered by half its length less its radius) |
| `color` | `[r, g, b]` 0 to 1, as picked in a paint program (the engine reads vertex colours as linear and the code converts). With `stuff` it multiplies the stuff's colour |
| `stuff` | the part takes the colour of the stuff the building is made of |
| `corners` | `[ix, iy]`: repeat the part at the footprint's four corners, that far in from each edge (table legs) |
| `slide` | metres the part moves along x when a door is fully open; a def with a sliding part is drawn as two meshes |
| `tint` (on `visual`) | multiplies every part's colour (the rock chunks are one shape with five tints) |
| `pieces`, `layer` (on `visual`) | items: pieces in a full pile, and the height of one layer of the pile |

A def with no `visual` is drawn as a grey block (a metre high for a building, a hand's breadth for an item),
so a new def always shows. A def's `visual.mesh` is a real mesh (below).

### Real meshes (A1)

A def can name a static mesh, `"visual": { "mesh": "/Game/Rim3D/Generated/Art/SM_Bed.SM_Bed", ... }`. The mesh is
already sized and stood on the ground (middle of its footprint at x = y = 0, bottom at z = 0, facing north), so it
is drawn as it is, the way the plants' meshes are. It replaces the parts for the finished building or the item; a
**door's** mesh is its sliding leaf (the frame and lintel stay as shapes). Frames and ghosts keep the plain shapes,
because the ghost material replaces only the first material of a mesh. The stuff colour does not tint a mesh. If a
mesh does not load, a warning is logged and the parts are drawn, so a fresh clone still shows everything; the
`buildings` smoke test fails if a named mesh is missing.

The meshes are free CC0 models from Poly Haven (1k glTF), listed in `assets/placeholder_art.json` (def, Poly Haven id,
the size in metres it stands in, which footprint axis its long side takes, the triangle count to cut it down to):

| Def | Mesh |
| --- | --- |
| Door | rollershutter_door (stretched to the leaf) |
| Bed | vintage_day_bed |
| Stool | wooden_stool_01 |
| Table1x2, Table2x2 | wooden_table_02 (stretched to the footprint) |
| Table2x4 | dining_table (stretched) |
| FueledStove | barrel_stove (one barrel in the middle of its 3 x 1) |
| WoodLog | dead_quiver_trunk, lying down |
| MealSimple, MealFine | wooden_bowl_02, wooden_bowl_01 |
| RawBerries | food_pomegranate_01 |
| Plant_TreeOak, Plant_TreePoplar | quiver_tree_01, pine_sapling_small (both stretched; the trees are not what the names say) |
| Plant_Bush, Plant_Berry | shrub_02, wild_rooibos_bush (stretched) |
| Plant_Rice, Plant_Corn, Plant_Haygrass | grass_medium_02, grass_medium_01 (tall), grass_bermuda_01 (all stretched; A6) |
| Plant_Potato, Plant_Strawberry, Plant_Cotton, Plant_Healroot, Plant_Devilstrand | weed_plant_02, shrub_sorrel_01, shrub_03, calathea_orbifolia_01, fern_02 (stretched) |
| TailorBenchHand, TailorBenchElectric, StonecutterTable, ButcherTable | wooden_table_03, wooden_table_01, wooden_table_03, wooden_table_02 (stretched to the footprint) |
| FueledSmithy, Heater | scandinavian_masonry_heater; ElectricSmithy metal_tool_chest; MachiningTable and Dresser drawer_cabinet (stretched) |
| ElectricStove, HospitalBed, CraftingSpot, ChessTable, BilliardsTable | electric_stove, old_bed_frame, wooden_cutting_board, wooden_table_01, dining_table |
| WoodFiredGenerator, Battery, Campfire | portable_generator, old_military_crate, stone_fire_pit |
| StandingLamp, SunLamp, TorchLamp | street_lamp_01 (two heights), vintage_oil_lamp |
| Armchair, EndTable, PlantPot | ArmChair_01, painted_wooden_nightstand, potted_plant_01 |
| *Still shapes* | walls, Column, HorseshoesPin (no fitting free model), and floors (floors are terrain defs, drawn by the terrain, not as meshes) |
| *Shapes in the data* (2026-11-19, power thread) | PowerConduit and PowerSwitch (a low dark cable stub; the switch a post with a box and a red lever), SolarGenerator and WindTurbine (a dark blue panel tilted 20 degrees on three posts; a 6.6 m white mast, nacelle and three blades: their frames and plans, and the finished look until their downloaded models are made, below), ChemfuelGenerator (an engine block with a red tank), Cooler (a piece of wall with a grille high on its cold side and a fan unit low on its hot side; the old aircon mesh stood loose in the room and is gone since the cooler went into the wall), Vent (a piece of wall with a louvred grille through it), PassiveCooler (a wooden box with a water tray) |

Wall, steel, silver, the survival pack, components, medicine, chunks and guns stay as shapes: Poly Haven has no
plain wall or ingot, and a box already reads as both.

To make them (not in Git, like the other generated assets; run again after a fresh clone):

```
python tools/placeholder_art.py          # downloads the glTF files to game/Content/ThirdParty/PlaceholderArt/ and sets visual.mesh in the defs
UnrealEditor-Cmd.exe C:\Dev\Rim3D\game\Rim3D.uproject -run=PlaceholderArt -unattended -nosplash -nullrhi
```

The `PlaceholderArt` commandlet (`game/Source/Rim3DEditor/PlaceholderArtCommandlet.*`) imports each file, turns it,
scales it, cuts it down with the engine's mesh reduction and saves `SM_<def>`. The glTF materials are instances of
the engine's glTF material, which instanced meshes cannot draw (the engine shows its dark default instead), so it
also makes `M_ArtLit` and `M_ArtLitMasked` (our own, colour texture only, marked for instanced meshes, two-sided;
masked for leaves) and an instance of one of them per mesh and material slot. Normal maps are not used. To use
another mesh, change `id` and `size` in the list and run both steps again.

### Downloaded models with a moving part

The solar generator and the wind turbine (sneed, 2026-11-22) use free Fab models, downloaded once into
`assets/raw/fab/` (not in Git; `assets/manifest.json` says where from): "Animated Wind Turbine" by Glowbox3D (CC BY,
its GLB with the blades' animation) and "Solar Panel Array Rack" by FunkFX (free Personal licence, a `.blend` of six
panels on a rack). Two art steps make them on every PC, run by `update-and-play.bat` (`tools/art_steps.json`):

- `power-models`: `tools/fab_prep.py` in Blender (it borrows `codex_prep.py`'s helpers) reads `assets/power_art.json`
  `prep` and writes FBX files, one texture set per material and an info file to `assets/raw/power/<Name>/`, plus a small
  Cycles preview picture. The **turbine** (kind `rotor`): the objects its animation turns are the rotor; the hub and
  axis are read from two animation frames. The model is turned so the rotor faces north, scaled so the rotor is
  `rotorDiameter` (4.8 m, inside the 5 x 2 footprint) across, and its tower stretched below the nacelle so the hub
  stands at `hub` [0, 0.55, 6.6] m. The rotor is its own FBX with its origin on the hub. The **solar rack** (kind
  `building`): turned long side east-west, scaled to fit, and set down twice side by side (`rows` 2) to fill the 4 x 4
  footprint. Its colours are shader nodes, not pictures, so they are baked onto a new UV layout.
- `power-art`: the GunArt commandlet (`-List=assets/power_art.json`) imports each FBX as one mesh,
  `/Game/Rim3D/Generated/Buildings/<name>/SM_<name>`, with `M_GunPBR` instances from the maps.

A def's **`visual.rotor`** (`mesh`, `at` [x, y, z] m in the building's frame, `axis`, `rpm`) is drawn beside its
`visual.mesh` as one more instance, turned every frame about the axis through `at` (`UBuildingSubsystem::AnimateRotors`;
the same per-instance update the doors use, kept through chunk rebuilds). Speed follows the power it makes (on no grid, where the sim counts no power, the weather's wind instead): still below
2% of `power.output`, then from 30% of `rpm` up to full `rpm` (24 turns a minute for the turbine) at full output, eased
over about 4 s; it runs in real time while the game is unpaused, since at 1x a game second is a thirtieth of a real one.
Without its `visual.mesh` the def's shapes are drawn whole and no rotor is added. Smoke test `power` stands both in the
open, takes pictures and fails if the turbine's blades do not turn (the wind never drops below a quarter).

Building heights are what picking uses (`UBuildingSubsystem::GetDrawnHeightCm`), so a click lands on what
you see. Walls are 3 m, the height of the thin roof ([terrain.md](terrain.md)); a door has a 0.8 m lintel
above its 2.2 m opening so the wall line has no gap under the roof.

**A building stands on the lowest ground under its footprint**, so on a slope it sinks into the high side
rather than hanging off the low one. Rotation is the sim's `Rot4` turned in 90 degree steps about the
footprint's middle.

## Stuff surfaces and joined walls (L4)

Task L4 of the look pass ([art-stage.md](../plans/art-stage.md)). A building made of stuff now shows the stuff's own
material, not just its colour, and walls join their neighbours.

**Surfaces.** A stuff def names its surface in `visual.surface`; the nine surfaces are free CC0 ambientCG and Poly Haven textures (2k
colour, DirectX normal and roughness maps), listed in `assets/stuff_textures.json` with their tile size, tint, saturation,
roughness and metalness:

| Stuff | Surface | Texture |
| --- | --- | --- |
| Wood | wood | Poly Haven wood_planks (brown planks; sneed 2026-11-25: ambientCG's WoodSiding008, grey weathered boards, read as sandstone) |
| Granite, limestone, sandstone, slate, marble blocks | granite, limestone, sandstone, slate, marble | Bricks089, Bricks083, Bricks100, Bricks066, Marble012 |
| Steel | steel | MetalPlates013 (rusty plates) |
| Plasteel, uranium | plasteel, uranium | MetalPlates004 (clean panels; uranium tinted green-grey) |

The `SurfaceMaterials` commandlet makes `M_Surface` and one `MI_Surface_<surface>` per surface in
`/Game/Rim3D/Generated/Surfaces/` (not in Git, `assets/manifest.json`). The material projects its textures along the
world's three axes by the surface's normal, so the generated meshes need no UVs and walls side by side run on without a
seam; brick courses and boards lie level on the sides. Its colour is the texture times the tint times the part's vertex
colour, so a part's own shade still darkens it (a wall's base course). The meshes made from a def's parts get a second
material slot for the parts made of stuff (`VisualMesh::MakeMesh`'s `StuffMaterial`); the other parts keep the vertex
colour material. A stuff with no surface, or a surface not made on this machine, is drawn in its plain colour as before
(with a warning in the log). Ghosts put the ghost material on every slot. Real meshes (A1) do not take the stuff yet (L7).

**Joined walls.** A def with `visual.link` (`Wall`, `AncientWall`; `thickness` in metres, 0.5) is drawn as a post in the
middle of its cell with an arm out to each side where there is another linked building, a door or natural rock: straight,
corner, T, cross and end pieces all come from that one rule, so no piece meshes are needed. Each piece has a base course
35 cm high and 4 cm wider on each side (shade 0.72) and an 8 cm cap 2.5 cm wider (shade 0.85). The neighbours are worked out
from the snapshot on every refresh and folded into the chunk's signature, so a wall going up next door rebuilds the chunk.
A linked building is never turned (its arms point at the map's neighbours). One mesh is made per def, stuff and neighbour set
(at most 16 a stuff). The placement ghost of the architect menu still shows the whole cell. Door frames (lintel and jambs) are
as deep as a wall now, 0.5 m.

**Walls join coolers and vents** (power thread, 2026-11-19): a def with `visual.joinsWalls` (the cooler and the vent, which
stand in a wall line) counts as a neighbour for the walls beside it, as a door does, so the wall's arm reaches it and
there is no gap.

**Power cables** (power thread, 2026-11-19): a def with `visual.cable` (`thickness` and `height` in metres, `color`; the
power conduit and the power switch) is drawn, once built, as a small junction with a low arm out to each side where a
built building with a power part stands (another conduit or switch, a generator, battery or machine, any of its
footprint cells), the way the sim's nets join them (power.md "Nets"). Like walls the neighbour set is in the chunk's
signature and one mesh is made per neighbour set. The colour is the cable's own, not the stuff's. Before it is built,
and on a machine without this code, the def's `parts` (a small dark stub) are drawn.

The look bench (`look` smoke test) builds its room of wood and a sample wall of every block, steel and plasteel west of
it, and takes an extra shot, `walls`, of the sample at noon. Spawned buildings have no stuff, so the test sets each wall's
`BuiltOf` itself.

Cost: none worth measuring at the bench (the same instanced meshes, a few more triangles a wall); the bench's frame times
did not move.

**Windows** ([windows.md](windows.md)): a linked wall with a window is cut at the sill and the lintel with an opening, a frame
and a glass pane between (`BldWindowParts`); the pane is its own mesh in `M_WindowGlass` (made by the `BuildingMaterials`
commandlet with the ghost) and casts no shadow. The window (none, glass, broken) is part of the look's key and the chunk's
signature, so breaking a pane rebuilds one chunk.

## Not here yet

- Floors, damage and fire, minified furniture.
- From L4: roofs seen from outside (the thin roof is only drawn from inside and is hidden in the overseer view), frames
  that look like a part-built wall with scaffolding, columns and fences joining walls, bevelled edges.
- Real art beyond A1's placeholders; steel and the other resources are still boxes (L8).
- Stockpile zone markings (the cells' tint). The items show where a stockpile is for now.
- Beds don't show who owns them; stoves don't show a flame; doors have no sound or frame animation.
- Ghosts and frames are one blue and one height rule for every def.
- Sub-snapshot motion for carried or moving things; those belong to the pawn actors.

## Upscalers (DLSS and later)

Everything here uses Unreal's ordinary static-mesh path: instanced static meshes with the engine's
opaque lit material, plus one normal translucent material for the ghosts. No custom render passes, no
dithered fake transparency, no world-position-offset (the door leaf moves by changing the instance's
transform, which carries correct motion vectors). The rule in [terrain.md](terrain.md) holds.
Translucent ghosts are the one thing upscalers handle less well than opaque meshes; they are a handful
of blue shapes shown only while something is being planned.

## Console and numbers

- `Buildings.Enable` 1 or 0.
- `UBuildingSubsystem::GetStats()` says what is drawn (things by kind, instances, components, meshes) and what
  rebuilds cost; the smoke test writes them to its notes.
