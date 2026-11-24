# Mech_Pikeman — Rim3D Codex Asset

Game-ready, photoreal, rigged and animated four-legged Remnant ranger spider-tank walker for **Rim3D** (Unreal Engine 5.8).

## Dimensions & Mesh Stats
- **Overall Rest Pose Size (X × Y × Z)**: `2.348 m × 2.704 m × 1.583 m`
  - **Bounds in Blender**: `X: [-1.174, +1.174] m`, `Y: [-1.620, +1.084] m`, `Z: [0.000, +1.583] m`
  - **Hull**: ~`1.30 m` long (`Y: -0.66 .. +0.62 m`), back/turret height ~`1.24 m` (`Z: 0.54 .. 1.24 m`), tall comms whip antenna tip at `1.583 m`.
  - **Railgun ("Needle Gun") Barrel**: `1.40 m` long (`Y: -0.22 .. -1.62 m`) with 7 copper-wound electromagnetic coil rings and 4 swept stabiliser fins.
- **Mesh Object**: 1 single skinned mesh (`Mech_Pikeman`) parented to `Armature` (rigid 100% skinning for hard-surface armor/mechanical plates; smooth 2-bone blending for under-hull cable bundles and whip antennas; max 4 bone influences per vertex; zero shape keys, constraints, or drivers).
- **Triangle Count**: `23,368 tris` (within the `25,000` triangle budget), zero n-gons, consistent outward normals, crisp split hard-surface bevel normals, applied transforms (scale `1.0`).
- **UVs**: Clean, non-overlapping `UV0` (`55.2%` packed coverage across 2048×2048).

## Axis Convention
- **Blender Source**: `+Z` up, metres, origin and `Root` bone at `(0, 0, 0)` on the ground under the body centre, all four flat clawed feet resting flush on `z = 0.000`, facing **`-Y`** forward (`+X` = mech's Left, `-X` = mech's Right).
- **glTF Export (`Mech_Pikeman.glb`)**: Exported with Blender's default glTF 2.0 `+Y` up setting (`export_yup=True`), making the mech stand `+Y` up and face **`+Z` forward** in glTF space.

## Material & Textures (2048×2048, 1 Material: `M_Mech_Pikeman`)
All four textures are procedurally generated (no downloaded assets), saved in this folder, and embedded inside `Mech_Pikeman.glb`:

| File | Resolution | Color Space | Channels / Contents |
|---|---|---|---|
| `Mech_Pikeman_BaseColor.png` | 2048×2048 | sRGB | Pure albedo (NO baked lighting, shadows, or AO): sand-khaki & dark slate-grey splinter camouflage plates, scratched bare steel edge wear, subtle rust streaks, weathered yellow/black hazard chevrons (no text), caked tropical ochre mud/dust on feet & lower shins, copper accelerator coils, matte black braided cables. |
| `Mech_Pikeman_Normal.png` | 2048×2048 | Non-Color (OpenGL `+Y`) | Panel seams, recessed rivets, rolled armor micro-grain, copper coil wire windings, braided cable weave, radiator louver slats, and gritty mud micro-bump. |
| `Mech_Pikeman_ORM.png` | 2048×2048 | Non-Color | Packed `R` = 40-ray BVH Ambient Occlusion (wired via `glTF Material Output`), `G` = Roughness, `B` = Metallic. |
| `Mech_Pikeman_Emissive.png` | 2048×2048 | sRGB | Layered amber-red tactical optical reticles on the 3 front sensor eyes + turret rangefinder lens, plus green/amber status indicator LEDs. |

## Armature & Bone Hierarchy (25 Bones)
All pivots listed in Blender rest-pose coordinates `(X, Y, Z)` in metres:

| Bone | Parent | Pivot `(X, Y, Z)` m | Role |
|---|---|---|---|
| `Root` | *(none)* | `(0.000, 0.000, 0.000)` | Ground root under body centre (stays in place for locomotion) |
| `Body` | `Root` | `(0.000, 0.000, 0.800)` | Armoured hull chassis & rear powerpack |
| `SensorHead` | `Body` | `(0.000, -0.580, 0.780)` | 3-eye sensor gimbal under overhanging brow plate |
| `Antenna_L` | `Body` | `(+0.270, +0.440, 1.030)` | Rear-left flexible comms whip antenna |
| `Antenna_R` | `Body` | `(-0.270, +0.440, 1.030)` | Rear-right telemetry mast & beacon |
| `Turret` | `Body` | `(0.000, -0.020, 1.000)` | Top remote weapon turret (yaw) |
| `Gun` | `Turret` | `(0.000, -0.100, 1.150)` | Railgun receiver & breech cradle (pitch) |
| `Barrel` | `Gun` | `(0.000, -0.220, 1.150)` | 1.40 m needle railgun barrel assembly (recoil slide) |
| `Muzzle` | `Barrel` | `(0.000, -1.620, 1.150)` | Barrel tip VFX/projectile socket |
| `Leg_FL_Hip` | `Body` | `(+0.360, -0.340, 0.720)` | Front-left coxa (yaw) |
| `Leg_FL_Upper` | `Leg_FL_Hip` | `(+0.477, -0.449, 0.720)` | Front-left femur (pitch) |
| `Leg_FL_Lower` | `Leg_FL_Upper` | `(+0.890, -0.833, 0.977)` | Front-left tibia/shin (knee pitch) |
| `Leg_FL_Foot` | `Leg_FL_Lower` | `(+0.940, -0.880, 0.110)` | Front-left flat clawed foot (ankle pitch, sole on `z=0`) |
| `Leg_FR_Hip` | `Body` | `(-0.360, -0.340, 0.720)` | Front-right coxa (yaw) |
| `Leg_FR_Upper` | `Leg_FR_Hip` | `(-0.477, -0.449, 0.720)` | Front-right femur (pitch) |
| `Leg_FR_Lower` | `Leg_FR_Upper` | `(-0.890, -0.833, 0.977)` | Front-right tibia/shin (knee pitch) |
| `Leg_FR_Foot` | `Leg_FR_Lower` | `(-0.940, -0.880, 0.110)` | Front-right flat clawed foot (ankle pitch, sole on `z=0`) |
| `Leg_RL_Hip` | `Body` | `(+0.360, +0.340, 0.720)` | Rear-left coxa (yaw) |
| `Leg_RL_Upper` | `Leg_RL_Hip` | `(+0.477, +0.449, 0.720)` | Rear-left femur (pitch) |
| `Leg_RL_Lower` | `Leg_RL_Upper` | `(+0.890, +0.833, 0.977)` | Rear-left tibia/shin (knee pitch) |
| `Leg_RL_Foot` | `Leg_RL_Lower` | `(+0.940, +0.880, 0.110)` | Rear-left flat clawed foot (ankle pitch, sole on `z=0`) |
| `Leg_RR_Hip` | `Body` | `(-0.360, +0.340, 0.720)` | Rear-right coxa (yaw) |
| `Leg_RR_Upper` | `Leg_RR_Hip` | `(-0.477, +0.449, 0.720)` | Rear-right femur (pitch) |
| `Leg_RR_Lower` | `Leg_RR_Upper` | `(-0.890, +0.833, 0.977)` | Rear-right tibia/shin (knee pitch) |
| `Leg_RR_Foot` | `Leg_RR_Lower` | `(-0.940, +0.880, 0.110)` | Rear-right flat clawed foot (ankle pitch, sole on `z=0`) |

## Animations (8 Actions, 30 fps, Exported via NLA Tracks)
All actions are baked via analytical 3D spider-leg IK and verified on re-import of `Mech_Pikeman.glb` into a fresh 30 fps Blender scene:

| Action | Frames (30 fps) | Duration | Type | Notes / Speed & Stride Length |
|---|---|---|---|---|
| `Idle` | `0–120` | `4.00 s` | Seamless loop | Hydraulic suspension sway, sensor cluster scan, subtle turret sentry sweep, weight shift. |
| `Walk` | `0–24` | `0.80 s` | Seamless loop (in place) | Diagonal scuttling gait (`FL+RR` / `FR+RL`), **1.30 m/s**, **stride length = `1.04 m` / cycle**, zero foot sliding (`z = 0` stance lock). |
| `Run` | `0–12` | `0.40 s` | Seamless loop (in place) | Fast aggressive diagonal scuttling gait, **3.20 m/s**, **stride length = `1.28 m` / cycle**, crouched forward pitch, gyro-stabilized turret. |
| `Attack` | `0–27` | `0.90 s` | One-shot (Idle → Idle) | Pneumatic melee stab with front-left leg (`Leg_FL`) while bracing weight on the other three legs. |
| `Hit` | `0–12` | `0.40 s` | One-shot (Idle → Idle) | Sharp ballistic impact flinch & shock-absorber compression, recovering to Idle. |
| `Death` | `0–45` | `1.50 s` | One-shot (held at frame 45) | Power-loss stagger and collapse onto `z = 0.000` (verified `min_z = 0.000 m`, nothing below ground), held motionless on final frames. |
| `Shoot` | `0–18` | `0.60 s` | One-shot (Idle → Idle) | Railgun discharge with `Barrel` recoiling `0.20 m` backward along the gun axis and the hull rocking back to brace against all 4 legs. |
| `Aim` | `0–30` | `1.00 s` | Seamless loop | Legs braced wide and low on `z = 0`, gun levelled horizontally with fine servo tracking micro-corrections. |

## Build & Reproduction Scripts
- `build_mech.py` — Master Blender 5.x build, UV unwrap, BVH AO/curvature cache, rigging, analytical IK animation baking, and GLB export (`blender -b --factory-startup -P build_mech.py`).
- `generate_textures.py` — Procedural 2048×2048 PBR texture generator (NumPy + Pillow), invoked automatically by `build_mech.py`.
- `render_previews.py` — Re-imports `Mech_Pikeman.glb` into a fresh Blender scene, verifies armature/mesh/material/all 8 actions, and renders `preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, `preview_scale.png`, `contact_sheet.jpg`, and `preview_anim.gif`.

## Delivered Previews
- `preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, `preview_scale.png` (1600×1200)
- `contact_sheet.jpg` (2400×1200, 2×3 grid, JPEG quality 85)
- `preview_anim.gif` (800×600, all 8 actions sequentially with corner labels)

## Status / Known Limitations
- All brief requirements and a full Pass 2 visual/technical self-critique improvement pass are complete. No unfinished items.
