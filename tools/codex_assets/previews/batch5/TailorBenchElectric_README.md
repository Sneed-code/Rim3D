# TailorBenchElectric

Game-ready, photoreal PBR 3D workstation asset for **Rim3D** (Unreal Engine 5.8) — an industrial frontier electric tailoring bench (year 6650 colonial utility aesthetic).

## Final Dimensions & Axis Convention

- **Blender Authoring Space:** `+Z` up, `-Y` front (operator working side), footprint centred on `(0, 0, 0)`, bottom resting on `Z = 0.000 m`.
- **glTF Export (`TailorBenchElectric.glb`):** Default Blender glTF 2.0 export (`+Y` up, metres, embedded textures & NLA animations).
- **Overall Bounding Size (Blender X × Y × Z):** `2.9060 m × 0.9120 m × 1.4469 m`
  - Footprint: ~`2.90 × 0.90 m` (`3 × 1` 1 m cells), tabletop working surface at `Z = 0.950 m`
  - World bounds: `X [-1.4530, +1.4530]`, `Y [-0.4560, +0.4560]`, `Z [0.0000, +1.4469]`

## Mesh Parts, Pivots & Triangle Budget

- **Total Triangles:** `11,644 / 12,000` (`0` n-gons, consistent outward weighted normals, scale `(1, 1, 1)` on all objects, clean non-overlapping UV0 atlas).

| Object Name | Triangles | Origin / Real Pivot (Blender XYZ, m) | Role |
|---|---:|---|---|
| `Body` | 10,400 | `(0.000, 0.000, 0.000)` | Stationary bench, welded steel frame, hardwood plank top, inset brass ruler, cast sewing machine body, motor & V-belt, gooseneck lamp shade, presser foot, half-sewn shirt, 6 fabric bolts + draped flaps, scissors, measuring tape, pin cushion + pins, tailor's chalk, spare cones/bobbins, thread scraps, caution-striped power switch box, foot pedal & cables |
| `HandWheel` | 736 | `(0.580, 0.010, 1.240)` | Spoked flywheel with V-belt groove, knurled outer rim & brass crank lug; rotates around local `X` axle |
| `Needle` | 120 | `(0.125, -0.010, 1.105)` | Reciprocating steel needle bar, brass needle clamp, thumbscrew & needle; translates vertically along `Z` (`-0.028 m` plunge) |
| `Spool` | 244 | `(0.380, 0.015, 1.290)` | Flanged amber thread spool with brass thread-retainer tab; rotates around local `Z` spindle |
| `Lamp` | 144 | `(0.195, -0.065, 1.345)` | Frosted halogen/LED work lamp bulb with dedicated emissive material |

## Materials & PBR Textures

Two materials total (shared PBR atlas for `Body`, `HandWheel`, `Needle`, `Spool`; dedicated emissive material for `Lamp`):

| File | Resolution | Color Space / Channels | Material |
|---|---:|---|---|
| `TailorBenchElectric_BaseColor.png` | 2048×2048 | sRGB — pure albedo only (no baked lighting, shadows, or AO) | `Mat_TailorBenchElectric` |
| `TailorBenchElectric_Normal.png` | 2048×2048 | Linear (OpenGL `+Y`) — wood grain, woven textiles, cast iron pebble, weld beads, panel seams, screws, knurling, etched ticks | `Mat_TailorBenchElectric` |
| `TailorBenchElectric_ORM.png` | 2048×2048 | Linear — `R` = Cycles Ambient Occlusion, `G` = Roughness, `B` = Metallic (shared single packed image for glTF `occlusionTexture` & `metallicRoughnessTexture`) | `Mat_TailorBenchElectric` |
| `TailorBenchElectric_Lamp_BaseColor.png` | 512×512 | sRGB — frosted warm glass bulb albedo | `Mat_TailorBenchElectric_Lamp` |
| `TailorBenchElectric_Lamp_Emissive.png` | 512×512 | sRGB — warm work lamp filament emission (`strength = 4.0`) | `Mat_TailorBenchElectric_Lamp` |

## Animations

Exported in `TailorBenchElectric.glb` as independent glTF animations via Blender NLA tracks (`24 fps`, `LINEAR` interpolation, seamless 1.0 s loops; `Body` is never animated):

| Action Name | Frame Range | Length | Frame Rate | Motion |
|---|---|---:|---:|---|
| `Working` | `0–24` | `1.000 s` | `24 fps` | `HandWheel` rotates 3 full turns (`+6π rad` around `X`), `Needle` plunges down (`-0.028 m`) and up 6 times along `Z`, `Spool` rotates 2 full turns (`+4π rad` around `Z`), `Lamp` held at scale `1.0` |
| `Idle` | `0–24` | `1.000 s` | `24 fps` | Rest pose held static across all moving parts, `Lamp` held at scale `1.0` |

## Deliverables & Build Pipeline

- **Asset:** `TailorBenchElectric.glb`
- **Textures:** `TailorBenchElectric_BaseColor.png`, `TailorBenchElectric_Normal.png`, `TailorBenchElectric_ORM.png`, `TailorBenchElectric_Lamp_BaseColor.png`, `TailorBenchElectric_Lamp_Emissive.png`
- **Previews (rendered from re-imported GLB in a fresh Cycles studio scene):** `preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, `preview_scale.png`, `contact_sheet.jpg`, `preview_anim.gif`
- **Rebuild scripts (Blender 5.2 background mode + Python/NumPy/Pillow):**
  1. `blender -b --factory-startup -P build_tailor_bench.py`
  2. `python generate_textures.py`
  3. `blender -b --factory-startup -P export_glb.py`
  4. `blender -b --factory-startup -P render_previews.py`
  5. `python make_contact_and_gif.py`

## Notes / Unfinished Items

- None — all geometry, UVs, PBR textures, animations, GLB export checks, previews, contact sheet, animated GIF, and self-critique refinement passes are complete.
