# HospitalBed

HospitalBed is a frontier medical bed for Rim3D. The raised head deck, alternating brake pedals, two-stage IV pole and welded steel lift frame are modeled at real-world scale.

| Specification | Final asset |
|---|---|
| Bounds in Blender XYZ | 0.950 m wide × 2.000 m long × 1.796 m tall |
| Cell footprint | 1 × 2 |
| Triangles | 9,980 total · 6,788 Frame · 3,192 Bedding |
| Materials and UVs | One shared PBR material. Non-overlapping UV0 with padded islands. |
| Scale reference | 1.800 m from sole to crown |

## Objects and coordinates

| Node | Blender XYZ origin in metres | Purpose |
|---|---|---|
| `Frame` | `(0.000, 0.000, 0.000)` | Floor-centred origin for placement. Includes the IV assembly, clipboard, rails and castors. |
| `Bedding` | `(0.000, 0.270, 0.710)` | Swappable soft goods with the origin at the transverse head-deck hinge. |
| `Sleeper1` | `(0.000, -0.130, 0.795)` | Empty at the sleeping spot on the level mattress. Its glTF translation is `(0.000, 0.795, 0.130)`. |

Blender uses metres, +Z up, +X across the bed, +Y toward the head and −Y toward the working foot end. The footprint is centred on the origin and the castors touch Z = 0. Blender’s default glTF export uses +Y up, with +Z toward the foot.

## Textures

The three 2048 × 2048 PNGs are embedded in `HospitalBed.glb` and available separately. The atlas is original procedural paintwork.

| File | Interpretation |
|---|---|
| `HospitalBed_BaseColor.png` | sRGB reflectance. Paint, wear and fabric colour without baked lighting or AO. |
| `HospitalBed_Normal.png` | Linear tangent-space normal, OpenGL +Y. |
| `HospitalBed_ORM.png` | Linear packed RGB. Red = ambient occlusion, green = roughness, blue = metallic. |

## Build and review

- `HospitalBed.glb` is the game asset. `HospitalBed.blend` is the Blender 5.3 Beta authoring scene with packed textures.
- Run `python build_hospitalbed.py` to rebuild the asset and previews. The launcher uses NumPy and Pillow, and Blender 5.x runs headlessly. Set `BLENDER_EXE` to override the default executable.
- `preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png` and `preview_scale.png` were rendered at 1600 × 1200 from the exported GLB after re-importing into a clean Blender scene. `contact_sheet.jpg` contains all five at 2400 × 1200.
- The first render pass showed cropped full-asset views, overly uniform paint chips and a blanket that read as a single thin sheet. The revision re-framed the renders, concentrated chipped enamel at edges, added blanket thickness and a turned hem, conformed the sewn patch to the mattress and refined the lift joints and IV stow clip.
- Verified the GLB and packed Blender scene locally. Unreal Editor import remains untested.
