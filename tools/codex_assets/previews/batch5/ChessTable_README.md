# ChessTable

ChessTable is a two-player recreation prop for Rim3D. The oiled timber top has a flush 8 × 8 inlay, cup rings, and a repaired plank joint. Twenty-two carved pieces are on the board and four captures sit beside it. Each long side has a recessed storage drawer with a forged pull.

## Runtime asset

| Property | Value |
| --- | --- |
| Runtime file | `ChessTable.glb` |
| Overall size (Blender X × Y × Z) | 0.940 × 1.940 × 0.835 m, including pieces |
| Playing surface height | 0.750 m |
| Footprint | 1 × 2 game cells, long dimension along Y |
| Triangles | 5,648 |
| Materials | One metallic-roughness PBR material |
| UVs | One non-overlapping UV0 set |

The Blender scene uses metres with +Z up and the front short end facing −Y. The footprint is centred on the origin and the steel feet rest at Z = 0. Blender’s default glTF conversion exports +Y up and points the front toward glTF +Z.

| Node | Type | Origin and facing in Blender |
| --- | --- | --- |
| `Body` | Mesh | (0, 0, 0) m at floor level, with identity transforms. Includes tabletop, frame, drawers, hardware, and pieces. |
| `Player1` | Seat empty | (−0.780, 0, 0) m at the floor of the −X seat. Local −Y faces +X toward the board. |
| `Player2` | Seat empty | (+0.780, 0, 0) m at the floor of the +X seat. Local −Y faces −X toward the board. |

The two closed drawers and the displayed game position are static, in keeping with the single `Body` mesh in the brief.

## Textures

All maps were made procedurally for this asset and are embedded in the GLB. The matching standalone files are 2048 × 2048 PNGs.

| File | Color space and channels |
| --- | --- |
| `ChessTable_BaseColor.png` | sRGB surface color, free of baked lighting and ambient occlusion |
| `ChessTable_Normal.png` | Linear tangent-space OpenGL normal, +Y green |
| `ChessTable_ORM.png` | Linear packed data, R ambient occlusion, G roughness, B metallic |

## Preview and inspection

The five 1600 × 1200 previews were rendered in Cycles after importing the final GLB into a clean Blender scene. The scale view includes a 1.800 m grey figure. `contact_sheet.jpg` combines the five views in a two-column, three-row grid at 2160 × 2621 and JPEG quality 85.

- [Front](preview_front.png)
- [Side](preview_side.png)
- [Three-quarter](preview_34.png)
- [Close-up](preview_close.png)
- [Scale](preview_scale.png)
- [Contact sheet](contact_sheet.jpg)

Use the **Blender 5.3 Beta** Open action on `ChessTable.blend` for native inspection. The source scene contains the asset and packed textures.

## Rebuild

Run these from the asset folder with Blender 5.3 Beta and a Python installation with Pillow. The launcher uses the installed Pinokio Blender by default on this PC. Set `CHESSTABLE_BLENDER` to override the executable path.

```text
python run_headless.py build
python run_headless.py preview
python make_contact_sheet.py
```

`build_chess_table.py` authors the geometry, UVs, and GLB. `chess_atlas.py` paints the three maps with Blender’s bundled NumPy. `render_previews.py` builds the studio and scale figure around the re-imported GLB. Pass `--first-pass` to each command to regenerate the original review renders in `.first_pass`.

## Review and verification

The first full render showed that the tabletop and dark pieces were too glossy, the wear was hard to see, and the front camera clipped the table. The improvement pass varied the grain across planks, brought out the cup rings and surface wear, adjusted roughness and inlay contrast, refined the carving silhouettes, and reframed the front and close shots. The final frame joints extend into the legs with the buried rail ends removed.

The final GLB was checked for triangle count, dimensions, transforms, outward-facing normals, UV0 overlap, and byte-decoded texture equivalence with the standalone PNGs. The finished deliverables were verified in Blender. Unreal Engine 5.8 import and in-game interaction were outside the Blender asset check and remain untested.
