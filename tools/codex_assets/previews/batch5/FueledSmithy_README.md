# FueledSmithy — Rim3D Game-Ready 3D Asset

Photorealistic, game-ready wood-burning blacksmith's forge workstation built for **Rim3D** (Unreal Engine 5.8, tropical frontier year 6650 aesthetic). Includes a brick-and-stone hearth with arched wood/ash alcove, riveted industrial sheet-metal hood and chimney with butterfly damper and worn hazard-stripe brow, glowing coal bed, half-finished incandescent blade, heavy timber firewood stand, pleated leather-and-wood hand bellows with press lever, hanging forged tongs/hammers/poker rack, London-pattern anvil on a banded tree-stump block, stave-built quench bucket with reflective water, and scattered ash/hammer-scale floor debris.

---

## Final Dimensions & Scale

- **Real-world size (Blender metres, X × Y × Z)**: **2.877 m × 0.867 m × 1.535 m**
  - **X (Width)**: `-1.418 m` to `+1.459 m` (**2.877 m**, fits 3 × 1 m cells / 2.9 m target)
  - **Y (Depth)**: `-0.437 m` to `+0.430 m` (**0.867 m**, fits 1 m cell / 0.9 m target)
  - **Z (Height)**: `0.000 m` to `+1.535 m` (**1.40 m** at the sheet-metal hood top; **1.535 m** at the chimney flue collar stub)
- **Ergonomic scale check**: Hearth firebed & anvil face sit at `~0.79–0.81 m` (waist/knuckle height for a 1.80 m human); bellows lever handle sits at `~0.94 m`. Verified in [`preview_scale.png`](preview_scale.png).

---

## Axis Convention

- **Blender authoring space**: `+Z` up, `-Y` front (working side where the colonist stands), `+X` right, footprint centered around `(0, 0, 0)`, bottom grounded strictly at `z = 0.000 m`, applied scale `(1, 1, 1)`.
- **glTF export (`FueledSmithy.glb`)**: Exported using Blender's default glTF 2.0 binary exporter (`+Y` up as required by the glTF specification, all textures embedded, NLA tracks exported as discrete animations).

---

## Geometry & Parts (Triangle Budget: 15,000 max)

| Mesh Object | Triangles | N-gons | Origin / Pivot (Blender metres) | Role |
|---|---:|---:|---|---|
| `Body` | 11,686 | 0 | `(0.000, 0.000, 0.000)` | Static hearth, hood, chimney, firewood stand, lower bellows & nozzle, anvil, stump, quench bucket, hanging tools, hot blade, ash/scale floor mounds |
| `Bellows` | 1,058 | 0 | `(-0.640, +0.060, +0.720)` | Moving bellows top wood board, iron straps, intake valve & overhanging upper leather pleat skirt (pivot on nozzle-end hinge) |
| `BellowsLever` | 532 | 0 | `(-0.660, +0.060, +0.930)` | Moving forged iron press lever, roller saddle shoe & turned hardwood grip (pivot on hearth-mounted fulcrum pin) |
| `Coals` | 1,206 | 0 | `(-0.030, +0.020, +0.700)` | Separate glowing coal bed & 54 faceted charcoal/coke lumps inside firepot (dedicated emissive material; pivot at bottom-center of bed) |
| **Total** | **14,482** | **0** | — | **Within 15,000 triangle budget**, consistent outward normals, clean non-overlapping UV0 |

---

## Materials & PBR Textures (2048 × 2048)

Two PBR materials are used (embedded inside `FueledSmithy.glb` and delivered as standalone PNGs):

### 1. `M_FueledSmithy_Main` (used by `Body`, `Bellows`, `BellowsLever`)
- `FueledSmithy_BaseColor.png` — 2048×2048 sRGB albedo (no baked shadows/lighting/AO; weathering reflects physical soot, ash, rust, chipped paint, scorched wood, wet bucket rim, and handle finger grease)
- `FueledSmithy_ORM.png` — 2048×2048 linear packed (`R` = raytraced Ambient Occlusion, `G` = Roughness, `B` = Metallic)
- `FueledSmithy_Normal.png` — 2048×2048 OpenGL (`+Y`) tangent-space normal map (brick mortar, stone grain, hammered steel dimples, weld seams, bark/wood grain, leather pores/creases, water ripples)
- `FueledSmithy_Emissive.png` — 2048×2048 sRGB emissive mask (`emissiveStrength = 4.8`) driving the incandescent yellow → orange → cherry-red heat gradient along the half-finished blade

### 2. `M_FueledSmithy_Coals` (used exclusively by `Coals`)
- `FueledSmithy_Coals_BaseColor.png` — 2048×2048 sRGB matte charred charcoal with powdery grey ash crust on upper lump faces
- `FueledSmithy_Coals_ORM.png` — 2048×2048 linear packed (`R` = AO, `G` = Roughness, `B` = Metallic)
- `FueledSmithy_Coals_Normal.png` — 2048×2048 OpenGL (`+Y`) fractured porous charcoal relief
- `FueledSmithy_Coals_Emissive.png` — 2048×2048 sRGB fiery ember glow (`emissiveStrength = 4.2`), hottest in crevices and tuyere core, dark charred crust on lump crests

---

## Animations (Embedded in `FueledSmithy.glb`)

All actions are authored at **24 fps** with **linear interpolation** on every keyframe so loops are seamless, pushed to matching NLA tracks, and animate only the moving parts (`Body` is never animated):

| Action Name | Frame Range (24 fps) | Length | Looping | Animated Objects & Motion |
|---|---:|---:|---|---|
| `Working` | `1–49` (48 frame intervals) | `2.00 s` | Seamless | `BellowsLever` and `Bellows` pump down and up twice (`rot_y` stroke `+0.062` to `-0.118 rad`); `Coals` surges in scale from `1.00` to `1.08` as air is forced into the firepot |
| `Idle` | `1–97` (96 frame intervals) | `4.00 s` | Seamless | `Coals` pulses gently in scale between `0.98` and `1.02`; all other parts remain still |

---

## Delivered Previews & Media

Rendered from `FueledSmithy.glb` re-imported into a fresh factory-startup Blender 5.2 scene (Cycles, neutral grey studio, soft key + fill + rim + procedural sky):

- `preview_front.png` (1600×1200) — front (`-Y`) working side
- `preview_side.png` (1600×1200) — side profile showing bellows, lever, firewood stack, hearth, and hood
- `preview_34.png` (1600×1200) — elevated three-quarter hero view
- `preview_close.png` (1600×1200) — detail close-up of glowing coals, hot blade, masonry, tool rack, and anvil horn
- `preview_scale.png` (1600×1200) — scale verification next to a plain 1.80 m grey human silhouette
- `contact_sheet.jpg` (2368×1244, JPEG quality 85) — 2×3 summary grid of all five renders + technical card
- `preview_anim.gif` (800×600, 36 frames, looping) — elevated three-quarter playback of `Working` followed by `Idle`

---

## Build Scripts & Reproducibility

All procedural build scripts are kept in this folder so the asset can be regenerated start-to-finish:

1. `blender -b --factory-startup --python-exit-code 1 -P build_smithy.py -- --mode model_and_bake` — builds geometry, unwraps UV0, validates topology/budget/bounds, and bakes intermediate geometry utility maps into `_bake/`.
2. `python generate_textures.py` — synthesizes all eight 2048×2048 PBR PNG textures using NumPy and Pillow.
3. `blender -b --factory-startup --python-exit-code 1 -P build_smithy.py -- --mode export_glb` — attaches PBR materials, creates `Working` and `Idle` NLA animations, and exports `FueledSmithy.glb`.
4. `blender -b --factory-startup --python-exit-code 1 -P render_previews.py` — re-imports `FueledSmithy.glb` into a fresh scene and renders the 5 studio stills and 36 animation frames.
5. `python make_sheets.py` — assembles `contact_sheet.jpg` and `preview_anim.gif`.

---

## Self-Critique Improvement Pass Summary

After reviewing the Pass 1 renders and animation frames, Pass 2 resolved:
- **BMesh `mat_id` tagging bug**: Creating the `mat_id` face layer after `create_cube`/`add_loft` and beveling left flat faces untagged; fixed via `new_bmesh()`, pre/post-bevel tagging, and per-material n-gon triangulation, verified with `assert zero_mat == 0`.
- **Bellows compression clipping**: Lowered the fixed lower leather accordion wall (`z <= 0.698`) and extended the moving upper leather skirt (`z = 0.798 → 0.645`) with a calibrated `0.18 rad` pump stroke so the bellows telescopes cleanly with zero poke-through when closed and zero gap when open.
- **Coal bed density & emissive color**: Increased coal lumps from 36 to 54 overlapping jagged chunks and tuned emissive strength (`4.2`) and hue so glowing crevices contrast sharply against charred black/ash-dusted lump crests without AgX washout.
- **Sheet-metal hood realism**: Reduced over-shiny metallic chipping and bumpy normal noise on the hood in favor of weathered matte/satin dark industrial olive-charcoal enamel, restrained edge chips, and crisper front hazard stripes.
- **Masonry & grounding**: Smoothly aligned the 7 arched alcove stone voussoirs, reduced brick relief thickness, raised the bar-stock pieces cleanly above `z = 0.000`, raised quench bucket water visibility, improved studio camera margins, and upgraded the 1.80 m human scale silhouette.

---

## Status / Unfinished Items

None — all required files, textures, animations, previews, contact sheet, and documentation are complete and verified.
