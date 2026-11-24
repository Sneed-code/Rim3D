# Mech_Lancer (Rim3D Codex)

Game-ready, photoreal, rigged and animated **Remnant Striker (`Mech_Lancer`)** built for **Rim3D** (Unreal Engine 5.8, tropical frontier world, year 6650).

## Asset Summary

- **Deliverable GLB**: `Mech_Lancer.glb` (single skinned mesh + single armature, default Blender glTF 2.0 binary export with embedded textures and NLA animation tracks)
- **Final Dimensions (X × Y × Z)**: **0.637 m × 1.544 m × 2.114 m** (rest pose; top of elongated helmet at `2.096 m`, backpack comms fin tip at `2.114 m`, soles planted flat on `Z = 0.000 m`)
- **Geometry Budget**: **20,584 triangles** (budget ≤ 25,000), 11,044 source vertices, 12,700 polygons, **0 n-gons**, consistent outward normals, applied transforms (scale `1.0`), clean non-overlapping `UV0`
- **Skinning**: Single skinned mesh object (`Mech_Lancer`) bound to `Armature` via Armature modifier, max **2** bone influences per vertex (limit ≤ 4), **0** shape keys, **0** runtime constraints/drivers, **0** IK bones
- **Materials**: **1 material** (`Mat_Mech_Lancer`, Principled BSDF + `glTF Material Output` Occlusion)

## Axis Convention

- **Blender Scene**: `+Z` up, metres, origin and `Root` bone at ground centre `(0, 0, 0)` with feet flat on `Z = 0`. Character faces **`-Y`** (mech's right/charge-lance side toward `-X`, mech's left/manipulator side toward `+X`).
- **glTF Export (`Mech_Lancer.glb`)**: Exported with Blender's default glTF `+Y` up conversion (`export_yup=True`), so the character stands `+Y` up and faces **`+Z` forward** in glTF space.
- **`Muzzle` Bone**: Positioned at the exact Charge Lance muzzle tip, pointing straight out of the barrel along its bone `+Y` (head-to-tail) firing axis.

## Textures (2048×2048, Embedded in GLB & Saved Separately)

| File | Color Space | Channels / Usage |
| :--- | :--- | :--- |
| `Mech_Lancer_BaseColor.png` | sRGB | Clean albedo only (no baked lighting or shadows): weathered off-white ceramic plates chipped to primer/bare grey steel along bevels, dark gunmetal titanium chassis, chrome piston shafts, matte rubber hoses/foregrip with grease wear, carbon scorches & heat tint near muzzle, tropical clay mud splatters on feet/ankles/lower shins |
| `Mech_Lancer_Normal.png` | Non-Color (Linear) | Tangent-space OpenGL (`+Y`) micro-detail normal map (paint chip step relief, ceramic micro-grain, directional brushed metal, rubber ribbing, mud crust, fine battle scratches) |
| `Mech_Lancer_ORM.png` | Non-Color (Linear) | Packed **R** = Ambient Occlusion (Cycles baked, wired to `glTF Material Output`), **G** = Roughness, **B** = Metallic |
| `Mech_Lancer_Emissive.png` | sRGB | Violet-white emissive (`Emission Strength = 5.5`) for horizontal helmet sensor bar, recessed backpack power core window, lance top optic, three muzzle accelerator coils, and `MuzzleFlare` core |

## Skeleton & Bone Hierarchy (28 Bones)

All hard-surface armor plates and limb segments use clean rigid 100% single-bone skinning; flexible waist hydraulic cables blend smoothly across adjacent spine bones (max 2 influences).

| Bone | Parent | Pivot / Role |
| :--- | :--- | :--- |
| `Root` | *(none)* | Ground origin `(0, 0, 0)` under body centre (stationary in `Walk`/`Run`) |
| `Pelvis` | `Root` | Hip/pelvic core pivot `(0.000, 0.015, 1.120)` |
| `Spine_Lower` | `Pelvis` | Lower mechanical abdomen & exposed waist pistons `(0.000, 0.010, 1.250)` |
| `Spine_Upper` | `Spine_Lower` | Chest carapace & structural shoulder yoke `(0.000, 0.002, 1.460)` |
| `Backpack` | `Spine_Upper` | Rear power cell, radiator cooling fins & glowing core window `(0.000, 0.135, 1.630)` |
| `Neck` | `Spine_Upper` | Articulated mechanical neck & hydraulic tilt struts `(0.000, -0.008, 1.780)` |
| `Head` | `Neck` | Elongated helmet-like head & horizontal violet-white sensor bar `(0.000, -0.022, 1.895)` |
| `Thigh_L` / `Thigh_R` | `Pelvis` | Hip joints `(±0.145, 0.012, 1.115)` |
| `Shin_L` / `Shin_R` | `Thigh_L` / `Thigh_R` | Knee drum joints `(±0.158, -0.062, 0.615)` |
| `Foot_L` / `Foot_R` | `Shin_L` / `Shin_R` | Ankle joints `(±0.168, 0.032, 0.115)` |
| `Toe_L` / `Toe_R` | `Foot_L` / `Foot_R` | Splayed twin armored front toe hinges `(±0.168, -0.095, 0.038)` |
| `Shoulder_R` | `Spine_Upper` | Right clavicle / pauldron pivot `(-0.135, 0.000, 1.715)` |
| `UpperArm_R` | `Shoulder_R` | Right shoulder drum joint `(-0.235, -0.005, 1.705)` |
| `Forearm_R` | `UpperArm_R` | Right elbow drum joint `(-0.245, -0.055, 1.355)` |
| `Lance` | `Forearm_R` | Integrated Charge Lance receiver, barrel, rail spars & accelerator coils |
| `Muzzle` | `Lance` | Lance muzzle tip socket, bone `+Y` pointing out of barrel |
| `MuzzleFlare` | `Muzzle` | Emissive muzzle energy core (scales radially/axially during `Shoot` coil flare) |
| `Shoulder_L` | `Spine_Upper` | Left clavicle / pauldron pivot `(0.135, 0.000, 1.715)` |
| `UpperArm_L` | `Shoulder_L` | Left shoulder drum joint `(0.235, -0.025, 1.705)` |
| `Forearm_L` | `UpperArm_L` | Left elbow drum joint |
| `Hand_L` | `Forearm_L` | Left wrist pivot & manipulator palm steadying the lance foregrip |
| `Finger1_L`, `Finger2_L`, `Thumb_L` | `Hand_L` | Articulated three-fingered manipulator digits |

## Animations (8 Actions, 30 fps, NLA Tracks -> glTF)

Verified by re-importing `Mech_Lancer.glb` into a fresh 30 fps Blender scene.

| Action | Frames (30 fps) | Duration | Type | Ground Speed / Stride | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `Idle` | `1–121` (120 steps) | `4.000 s` | Seamless loop | — | Servo breathing, weight shift, alert horizontal head sensor scan, feet locked on `Z = 0` |
| `Walk` | `1–37` (36 steps) | `1.200 s` | Seamless loop (in-place) | **1.50 m/s** / **1.800 m per cycle** | Constant-velocity ground stance (zero foot slide), patrol lance carry with left hand locked to foregrip |
| `Run` | `1–21` (20 steps) | `0.667 s` | Seamless loop (in-place) | **4.00 m/s** / **2.667 m per cycle** | Athletic forward sprint, zero foot slide during 34% stance phase, Charge Lance held diagonally across body |
| `Attack` | `1–28` (27 steps) | `0.900 s` | One-shot (Idle -> Idle) | — | Wind-up and forceful forward rifle-butt / forearm melee bash, returning cleanly to `Idle` |
| `Hit` | `1–13` (12 steps) | `0.400 s` | One-shot (Idle -> Idle) | — | Sharp ballistic torso/head flinch and servo recovery back to `Idle` |
| `Death` | `1–46` (45 steps) | `1.500 s` | One-shot (held at end) | — | Power-down collapse flat onto `Z = 0` (verified all geometry ≥ `0.000 m`, Charge Lance & body grounded), held still on final frames |
| `Aim` | `1–31` (30 steps) | `1.000 s` | Seamless loop | — | Bladed marksman stance, Charge Lance levelled straight forward along `-Y`, left manipulator bracing foregrip, subtle servo micro-corrections |
| `Shoot` | `1–16` (15 steps) | `0.500 s` | One-shot (Aim -> Aim) | — | Brief charge settle, recoil kick of lance/arm/torso + bright `MuzzleFlare` scale pulse, hydraulic recovery back to `Aim` |

## Previews Delivered

- `preview_front.png` (1600×1200, Cycles from re-imported GLB)
- `preview_side.png` (1600×1200, Cycles from re-imported GLB)
- `preview_34.png` (1600×1200, above-front three-quarter)
- `preview_close.png` (1600×1200, detail close-up on head visor, chest armor, left manipulator & lance)
- `preview_scale.png` (1600×1200, side-by-side with plain 1.80 m grey human silhouette)
- `contact_sheet.jpg` (2400×1200 2×3 grid, quality 85)
- `preview_anim.gif` (800×600 three-quarter animation reel covering all 8 actions with corner labels)

## Rebuilding From Source Scripts

All source scripts are included in this folder:

```powershell
# 1. Build mesh, rig, UV0, 2048x2048 PBR textures, 8 NLA animations and export Mech_Lancer.glb
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup --python-exit-code 1 -P build_mech_lancer.py

# 2. Re-import Mech_Lancer.glb into a fresh scene, verify rig/actions, and render studio previews + anim frames
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup --python-exit-code 1 -P render_previews.py

# 3. Assemble contact_sheet.jpg and preview_anim.gif (requires Python with Pillow)
python assemble_media.py
```

## Notes / Unfinished Items

- None — all geometry, texturing, rigging, animation, re-import verification, preview renders, and multi-pass art-director polish items are complete.
