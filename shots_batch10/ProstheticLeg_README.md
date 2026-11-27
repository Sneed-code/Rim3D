# ProstheticLeg

A worn industrial prosthesis for Rim3D. The leg rests on its side with the knee straight. The assembly includes a hollow, padded thigh socket with woven straps and buckles, a steel hinge and coil-spring damper, a tubular steel pylon, and a flexible composite foot with a siped rubber outsole.

## Mesh and orientation

| Property | Value |
| --- | --- |
| Final size in Blender, X × Y × Z | 0.9421 × 0.3192 × 0.1974 m |
| Length along the leg | 0.9421 m |
| Triangles | 7,809 |
| Mesh object | `Body` |
| Body pivot | (0, 0, 0) m, at the bottom center of the footprint bounds |
| Blender coordinates | Metres, +Z up, socket toward −X and foot toward +X |
| GLB coordinates | Metres, +Y up and foot toward +X, exported at Blender’s defaults |
| Transforms | Scale (1, 1, 1), with the resting pose applied to the mesh |
| UVs | One non-overlapping UV0 atlas |
| Material | `ProstheticLeg_PBR` |

The hinge, damper, straps, and foot are built into `Body` for the static ground-item pose.

## Textures

All three maps are original procedural 2048 × 2048 PNGs, embedded in [ProstheticLeg.glb](ProstheticLeg.glb) and supplied separately.

| File | Interpretation |
| --- | --- |
| [ProstheticLeg_BaseColor.png](ProstheticLeg_BaseColor.png) | sRGB surface color. The atlas carries paint, exposed metal, oxidation, grease, and dirt. Lighting and occlusion are excluded from this map. |
| [ProstheticLeg_Normal.png](ProstheticLeg_Normal.png) | Linear tangent-space normal, OpenGL +Y. Contains fabric weave, machining, knurling, scratches, and sole tread relief. |
| [ProstheticLeg_ORM.png](ProstheticLeg_ORM.png) | Linear packed data. Red is ambient occlusion, green is roughness, and blue is metallic. |

## Previews

All five PNGs are 1600 × 1200 Cycles renders of the final GLB re-imported into a fresh Blender 5.3 Beta scene. The scale view uses a 1.800 m gray mannequin on the same floor as the asset.

- [Front](preview_front.png)
- [Side and outsole](preview_side.png)
- [Three-quarter](preview_34.png)
- [Knee and damper detail](preview_close.png)
- [Human scale](preview_scale.png)
- [Contact sheet](contact_sheet.jpg) with five views in a two-row, three-column grid. The JPEG is 2400 px wide at quality 85.

## Art review and verification

The first renders showed pale, similar-looking surfaces and a break in the load path between the socket and knee. The revised mesh uses a stepped connector, a smoother coil and hinge cap, strap returns through the buckles, and grooves in the outsole edge. The revised maps deepen the olive polymer, distinguish rubber from composite, and cut back the coarse highlights on the machined steel. The final side view exposes more of the tread.

The exported GLB passed checks for triangle count, mesh and UV winding, UV overlap, bounds, transform, material count, and embedded texture content. All five final views were reviewed after re-import. The assembly pass removed 759 fully buried triangles.

The brief’s deliverables are complete. Unreal Engine import remains untested.

## Rebuild

From this directory, run `python rebuild.py` with Python, NumPy, Pillow, and Blender 5.3 Beta available. Use `--blender <path-to-blender.exe>` to override the installed path. The build, render, and verification scripts are included. The runner builds the mesh and maps headlessly, exports and re-imports the GLB, renders the five views, and makes the contact sheet. Logs and temporary processing data are written to `.review`.
