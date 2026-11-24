# Mech_Scyther — Rim3D Codex Asset

Autonomous Remnant melee reaper mech built for **Rim3D** (Unreal Engine 5.8, realistic AAA survival-shooter aesthetic, tropical frontier world, year 6650).

## Asset Summary

| Property | Value |
| --- | --- |
| **Asset File** | `Mech_Scyther.glb` (glTF 2.0 Binary, +Y up, metres, embedded textures) |
| **Final Dimensions (Rest Pose)** | `0.875 m (X width) × 1.011 m (Y depth) × 2.086 m (Z height)` *(head crown ~1.94 m, antenna tip 2.086 m, feet flat on `z = 0.000 m`)* |
| **Triangle Count** | `23,864` triangles *(budget: 25,000 tris; 0 n-gons)* |
| **Mesh Objects** | `1` skinned mesh (`Mech_Scyther`) parented to `1` `Armature` |
| **Rig / Skinning** | `38` bones, max `2` bone influences per vertex *(limit: 4)*, zero unweighted vertices, zero runtime constraints/drivers/shape keys |
| **Materials** | `1` PBR material (`M_Mech_Scyther`) |
| **Textures** | 4 × `2048 × 2048` PNGs (procedural, non-overlapping UV0, 0.045% pixel overlap) |

## Axis & Orientation Convention

- **Blender build space**: `+Z` up, **`-Y` forward** (direction the mech faces), `+X` = mech's Left (`_L`), `-X` = mech's Right (`_R`). Origin `(0, 0, 0)` and `Root` bone sit on the ground directly beneath the body centre; feet rest cleanly on `z = 0.000 m`.
- **glTF 2.0 export space** (`Mech_Scyther.glb`, default Blender glTF exporter with `export_yup = True`): `+Y` up, **`+Z` forward**, metres, scale `1.0`.

## Textures (2048 × 2048)

| File | Colour Space | Contents |
| --- | --- | --- |
| `Mech_Scyther_BaseColor.png` | sRGB | Pure albedo (no baked lighting/shadows/AO): faded olive-grey military armour paint with chipped bare-steel bevels, dark forged gunmetal chassis, scratched mirror-steel curved scythe blades, chrome piston rods, braided rubber hoses, tarnished brass collars, worn shoulder hazard stripes, tactical stencils, hydraulic oil leaks, exhaust scorch, subtle seam rust, and lower-leg tropical jungle grime |
| `Mech_Scyther_Normal.png` | Linear / OpenGL (+Y) | Tangent-space micro-detail: panel seam grooves, recessed Allen bolts, brushed steel blade grain & battle scratches, radiator louvers, knurled brass collars, braided hose diamond weave |
| `Mech_Scyther_ORM.png` | Linear (Non-Color) | Packed PBR channels: **R** = true 3D raytraced Ambient Occlusion (BVH baked + crevice seams), **G** = Roughness, **B** = Metallic |
| `Mech_Scyther_Emissive.png` | sRGB | Cold blue optical visor slit (`#3CE0FF`) and tiny cyan/amber telemetry status LEDs on chest, head temple, shoulders, and dorsal pack |

## Skeleton Hierarchy & Pivots (`38` Bones)

All armour plates, joints, pistons, and blades use rigid 100% single-bone skinning; flexible braided waist, neck, and shoulder/knee hoses use smooth 2-bone blending. All 6 hydraulic piston pairs (`WaistPiston`, `KneePiston`, `AnklePiston`, L/R) telescope analytically and are baked directly to bone keyframes with zero runtime constraints.

- `Root` `(0.000, 0.000, 0.000)` — ground origin under body centre (stationary for in-place locomotion)
  - `Pelvis` `(0.000, 0.020, 1.040)` — hip saddle & gyro chassis
    - `Spine_Lower` `(0.000, 0.015, 1.120)` — lower segmented mechanical waist
      - `Spine_Upper` `(0.000, 0.000, 1.270)` — upper waist & brass gyro ring
        - `Chest` `(0.000, -0.015, 1.400)` — layered sternum/pectoral carapace, recessed vents, dorsal radiator pack
          - `Neck` `(0.000, -0.045, 1.670)` — articulated ringed hydraulic neck
            - `Head` `(0.000, -0.095, 1.790)` — stealth sensor head & cold blue optical slit
              - `Antenna` `(0.052, -0.045, 1.905)` — short tactical comms whip
          - `Shoulder_L` / `Shoulder_R` `(±0.195, -0.025, 1.600)` — shoulder gimbal & layered pauldron
            - `UpperArm_L` / `UpperArm_R` `(±0.315, -0.020, 1.575)` — humerus frame & bicep piston
              - `Forearm_L` / `Forearm_R` `(±0.375, 0.055, 1.255)` — elbow drum & blade-mount gauntlet
                - `Blade_L` / `Blade_R` `(±0.385, -0.170, 1.020)` — ~0.70 m curved mirror-steel scythe blade with chipped edge & parrying spur
          - `WaistPistonRod_L` / `WaistPistonRod_R` `(±0.118, -0.010, 1.385)` — upper waist hydraulic rod
    - `WaistPistonCyl_L` / `WaistPistonCyl_R` `(±0.095, 0.020, 1.085)` — lower waist hydraulic barrel
    - `Thigh_L` / `Thigh_R` `(±0.165, 0.015, 1.035)` — upper digitigrade thigh & forward knee shield
      - `KneePistonCyl_L` / `KneePistonCyl_R` `(±0.168, 0.035, 0.775)` — upper shin hydraulic barrel
      - `Shin_L` / `Shin_R` `(±0.185, -0.155, 0.665)` — backward-angled shin & hock spur
        - `KneePistonRod_L` / `KneePistonRod_R` `(±0.172, 0.162, 0.395)` — lower shin hydraulic rod
        - `AnklePistonCyl_L` / `AnklePistonCyl_R` `(±0.178, 0.145, 0.365)` — upper ankle hydraulic barrel
        - `Metatarsal_L` / `Metatarsal_R` `(±0.178, 0.110, 0.345)` — lower forward-angled digitigrade strut
          - `AnklePistonRod_L` / `AnklePistonRod_R` `(±0.172, 0.055, 0.088)` — lower ankle hydraulic rod
          - `Foot_L` / `Foot_R` `(±0.172, -0.020, 0.062)` — ankle drum, arch pad, rear heel spur
            - `Toe_L` / `Toe_R` `(±0.172, -0.105, 0.032)` — splayed articulated front raptor claws

## Animations (30 fps, Exported via NLA Tracks)

All 7 animations were verified by re-importing `Mech_Scyther.glb` into a fresh Blender 5.2 scene.

| Action | Frames | Length | Loop | Notes |
| --- | --- | --- | --- | --- |
| `Idle` | `0–120` | `4.00 s` | Yes | Hydraulic breathing sway, weight shift, alert predator head scan, subtle blade & antenna motion |
| `Walk` | `0–30` | `1.00 s` | Yes | In place, **1.6 m/s**, stride length **`1.60 m` / cycle**; planted foot translates rearward at exact `1.60 m/s` on `z = 0` with zero sliding |
| `Run` | `0–18` | `0.60 s` | Yes | In place, **4.7 m/s**, stride length **`2.82 m` / cycle**; predatory forward-leaning sprint with arms & blades swept back and ballistic flight phase |
| `Charge` | `0–18` | `0.60 s` | No | Deep crouch-and-coil starting from `Idle` frame 0 and launching seamlessly into `Run` frame 0 |
| `Attack` | `0–27` | `0.90 s` | No | Fast two-blade slash (right blade cross-slash stepping in, then left blade follow-through), starts and ends in `Idle` pose |
| `Hit` | `0–12` | `0.40 s` | No | Sharp kinetic stagger/flinch backward with damped servo recovery back to `Idle` pose |
| `Death` | `0–45` | `1.50 s` | No | Servo-failure knee buckle and collapse onto the ground (`min_z = 0.003 m` across all frames; zero floor clipping), held still on final frames `39–45` |

## Previews & Build Scripts

- **Previews (rendered from re-imported GLB in Cycles GPU)**: `preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, `preview_scale.png`, `contact_sheet.jpg` (2400×1200, quality 85), `preview_anim.gif` (800×600, all 7 actions labelled sequentially).
- **Reproducible build pipeline**:
  1. `blender -b --factory-startup -P build_mech_scyther.py` *(uses `mech_config.py`, `generate_textures.py`, `animate_mech_scyther.py` to build geometry, UVs, 2048 PBR maps, rig, 7 NLA animations, and export `Mech_Scyther.glb`)*
  2. `blender -b --factory-startup -P render_previews.py` *(re-imports `Mech_Scyther.glb` into a fresh scene, verifies rig/mesh/actions, and renders all stills, `contact_sheet.jpg`, and `preview_anim.gif`)*

## Improvement Pass Notes & Known Limitations

- **Self-critique pass completed**: Pass 1 revealed overly broad orange rust triggered by AO thresholds, fragmented UV island margins (16.2% coverage), an overly flat upper chest core, slightly tight camera framing at the antenna tip, and a slightly elevated upper blade on Death settle. Pass 2 recalibrated seam-localized dark oxide rust so dark forged gunmetal and faded olive-grey armour read accurately, switched to Smart UV Project + island packing for sharper micro-detail, sculpted forward-projecting layered chest armour + visible radiator vents + shoulder yokes, tucked the knee pistons neatly behind the thighs, improved the grounded `Death` collapse, and widened all studio camera framing.
- **Not finished / Notes**: None — all required deliverables, textures, animations, re-import checks, and previews are complete and verified.
