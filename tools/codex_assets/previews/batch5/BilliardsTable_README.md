# BilliardsTable

A frontier recreation table for Rim3D. The body has turned timber legs, inset
rail sights, six open leather-lined pockets with woven baskets, a repaired
green felt bed, nine unnumbered balls, two cues and a chalk cube. Neutral
warm-brown timber leaves room for material tinting in the engine.

| Property | Value |
| --- | --- |
| Footprint and height | 1.940 × 3.940 × 0.850 m (X × Y × Z in Blender) |
| Placement grid | 2 × 4, with one-metre cells |
| Triangles | 9,896 |
| Mesh and pivot | `Body` at (0, 0, 0), centred on the footprint at floor level |
| Blender axes | +Z up, long side along Y, working end toward −Y |
| glTF axes | +Y up, long side along Z, working end toward +Z |
| Materials and UVs | One opaque material and one unique, padded UV0 atlas |

`Body` contains the table and all accessories. There are no separate moving
parts or secondary pivots.

## Texture files

All three original procedural PNGs are 2048 × 2048 and are embedded in
`BilliardsTable.glb`. The native `BilliardsTable.blend` also packs these maps.

| File | Use |
| --- | --- |
| `BilliardsTable_BaseColor.png` | sRGB albedo. Paint and material wear only, with no baked lighting or AO. |
| `BilliardsTable_Normal.png` | Linear tangent-space OpenGL (+Y). |
| `BilliardsTable_ORM.png` | Linear packed map. R is occlusion, G roughness, B metallic. |

The five studio previews were rendered in Cycles from a fresh GLB import.
The 1.80 m reference figure and studio rig are preview-only.

## Rebuild

Run `python build_billiards_table.py` with Blender 5.x and Python with NumPy
and Pillow. Pass `--blender "path/to/blender.exe"` to select a different
installation. The default builds the final version. `--pass initial` writes
the earlier treatment into the build folder for comparison.

## Review pass

The first renders showed pale felt, pocket covers that blended into the wood,
and two cues crossing on the bed. The final pass deepened the felt and
leather tones, wrapped the cushion cloth around the jaws, refined the pocket
weave and repair stitching, and separated the cues. The scale view was
reframed to show the full figure beside the table.

## Completion

All requested files are complete. Checks covered a fresh GLB import, UV
overlap, winding, exact texture parity and the packed native scene. The
asset has not been tested in Unreal Engine.
