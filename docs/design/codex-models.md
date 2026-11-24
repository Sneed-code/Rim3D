# Codex models: the 15 things free Fab didn't have

sneed asked on 2026-11-19 for 15 models the game needs that free Fab doesn't have, made with Codex (GPT 6.2 Sol)
on his PC. Fab was searched first (free listings); good free models exist for the frag grenade, a single-tube
rocket launcher (the doomsday launcher), an EMP-style grenade, solar panels, a wind turbine and a ration pack, so
those were left to Fab.

| Model | Def it draws | Notes |
| --- | --- | --- |
| GrenadeLauncher | `Gun_GrenadeLauncher` (guns.json) | break-open 40 mm; second pass after round 1 looked like a toy |
| TripleRocketLauncher | `Gun_TripleRocketLauncher` (guns.json) | three tubes, rockets inside |
| Molotov | `Grenade_Molotov` | bottle, fuel, rag; glass is drawn opaque (M_GunPBR has no translucency) |
| ComponentIndustrial | `ComponentIndustrial` | the stack object in the GLB is left out |
| MiniTurret, AutocannonTurret, Mortar | `Turret_Mini`, `Turret_Autocannon`, `Turret_Mortar` | head and barrel at rest, joined (see Not yet) |
| IEDTrap | `TrapIEDExplosive` | second pass (real shells and wires) |
| NutrientPasteDispenser, CommsConsole, HiTechResearchBench | same names | emissive screens and lamps |
| TransportPod, GeothermalGenerator, DeepDrill, BiofuelRefinery | same names (`TransportPod`, ...) | |

### Batches 2 and 3 (2026-11-19/20)

sneed asked for 20 more, then 20 more again. Batch 2 (all 20 made and wired in): hydroponics basin, ship
cryptosleep casket, multi-analyzer, fabrication bench, simple research bench, electric smelter, crematorium, drug
lab, chemfuel generator, watermill generator, moisture pump, orbital trade beacon, long-range mineral scanner,
ground-penetrating scanner, pod launcher, foam turret, firefoam popper, rocketswarm and uranium slug turrets, hive.
Batch 3 (15 of 20 made before the ChatGPT usage limit; 13 wired in): vitals monitor, fermenting barrel, hopper,
passive cooler, flood light, telescope, tube television, poker table, horseshoes pin, animal sleeping box, animal
bed (all `visual.mesh`), monosword and plasma sword (`assets/guns.json` melee, palm at the origin, so no
`autoOrient`). The autodoor (frame plus two sliding leaves) and the in-wall cooler are made but not wired in: a door's
mesh is one sliding leaf and the cooler def is a wall-height block. Zeushammer, breach axe, HE mortar shell,
spacer component and bionic arm were cut off by the limit; their briefs stay, rerun them later.

### Batch 3b and batch 4 (2026-11-22, high effort)

After the usage limit reset, the 5 cut-off models were made (zeushammer and breach axe in `assets/guns.json`
melee; HE mortar shell, spacer component, bionic arm by `visual.mesh`). Batch 4 ran with `CODEX_EFFORT=high`
(sneed: quality matters): 8 guns in `assets/guns.json` (chain shotgun, charge rifle, heavy SMG, machine pistol,
revolver, sniper rifle, charge lance, needle gun; hold points first guesses from the model bounds and Muzzle
sockets, tune on the PC), and by `visual.mesh` the double bed, flatscreen TV, spike trap, ship engine, ship
reactor, sandbags, barricade, bionic leg, EMP grenade and industrial medicine. Made but not wired in: the recurve
bow (no bow entry in guns.json yet) and the vent (like the cooler, the def is a wall-height block). Neon made the
LMG, minigun, hatchery, power switch, ship computer core and sensor cluster, so Codex skipped them.

### Batch 5 (2026-11-24): remnants, chitinids and the stand-ins

sneed asked for 10 on Neon high (the hardest, animated, most detailed) and 20 on Codex 6.2 Sol high. Neon
(`tools/neon_assets/`): `Mech_Scyther`, `Mech_Lancer`, `Mech_Pikeman`, `Mech_Centipede`, `Megaspider`, `Spelopede` as
**rigged creatures** (one armature, rigid-skinned plates, in-place clips `Idle`, `Walk`, `Run`, `Attack`, `Hit`,
`Death`, plus `Shoot`/`Aim` for the gunners and `Charge` for the reaper; facing Blender -Y), and `WoodFiredGenerator`,
`FueledSmithy`, `ElectricSmithy`, `TailorBenchElectric` with moving parts (`Running`, `Working`, `Idle` clips). Codex:
butcher, stonecutter, machining and hand tailor benches, fueled and electric stoves, bed, hospital bed, chess and
billiards tables, battery, heater, sun lamp, standing lamp, torch lamp, campfire, dresser, end table, armchair and the
megascreen TV, replacing the Poly Haven stand-ins in `assets/placeholder_art.json` (entries removed).

- Creatures: prep kind `creature` copies the GLB to `assets/raw/codex/<Name>/<Name>.glb`; `assets/animal_art.json`
  `imports` name the clips (`Run` is the gallop role; `Attack` and `Shoot` are new roles). The remnants are pawns, not
  animals: `USimAnimalSubsystem::DrawsMachine` takes any non-animal pawn whose def has a model in the art table's `defs`,
  and `USimPawnSubsystem` skips it. The actor plays `Attack` once when a melee swing winds up and `Shoot` once when a shot
  goes (the gun's cooldown starts again); a downed remnant lies in its death pose. The pikeman model is drawn at 0.6.
- Moving benches: `visual.moving` rules `working` (a pawn on a bill or butchering stands within reach of it) and
  `fueled` (fuel in it), next to `always`, `running`, `on`, `off`.

## How a model is made

`tools/codex_assets/`: `_common.md` is the brief every job shares (GLB in metres, +Z up, real scale, PBR BaseColor /
Normal (OpenGL) / ORM 2048 maps, named moving parts, axis rules, previews and a contact sheet, a self-critique pass),
`<Name>.md` the asset's own brief, `critiques/<Name>.md` a second-pass critique. `run_codex_assets.sh` runs the briefs
three at a time with `codex exec` (Git Bash on sneed's PC, `CODEX_MODEL=gpt-6.2-sol`); `run_critique.sh` runs the
second passes. Each job's folder is `C:\Users\thill\OneDrive\Documents\ChatGPT\Models\Rim3D_Codex\<Name>\` (not in
Git): BRIEF.md, the build script Codex wrote, the GLB, textures, previews, contact_sheet.jpg, README.md and the log.
The whole batch of 15 took 1 h 48 min, 16 to 34 minutes a model.

## Into the game

1. `blender -b --factory-startup -P tools/codex_prep.py -- <Rim3D_Codex folder>` (from the repo root) reads each
   `<Name>.glb` and writes `assets/raw/codex/<Name>/` (not in Git): `<Name>_Game.fbx`, `T_<slot>_BaseColor/Normal/ORM
   (/Emissive).png` per material and `<Name>_info.json` (size, parts, sockets and slots in Unreal's frame, cm).
   Buildings and items are joined into one object, stood up if the GLB was written Z-up into glTF (the bounds are
   matched against the expected size in `assets/codex_art.json` `prep`), set with the footprint middle at the origin
   (`centerOn` picks the base for turrets, so the barrel doesn't shift it) and the bottom at z = 0. The front stays at
   Blender -Y, which the FBX import makes Unreal +Y: the sim's north, the way building meshes face. Guns keep their
   origin (the palm on the grip, +X out of the muzzle) and are joined into one mesh named after the file.
2. `UnrealEditor-Cmd.exe Rim3D.uproject -run=GunArt -List=assets/codex_art.json -unattended -nosplash -nullrhi`
   imports the buildings and items: GunArt's new `buildings` list puts each combined mesh at
   `/Game/Rim3D/Generated/Buildings/<Name>/SM_<Name>`, which the def's `visual.mesh` names, and its new
   `slotTextureDir` gives every material slot an `M_GunPBR` instance from its `T_<slot>_*` maps. `M_GunPBR` is now marked
   for instanced meshes (buildings are drawn instanced).
3. `UnrealEditor-Cmd.exe Rim3D.uproject -run=GunArt -unattended -nosplash -nullrhi` (assets/guns.json) imports the two
   launchers into `/Game/Rim3D/Generated/Guns/<Name>/`.

Without these steps the defs' box parts are drawn, as before (the `buildings` smoke test lists the missing meshes).

## Not yet

- Turret heads don't turn to their target and barrels don't recoil: the head, gun and barrel are joined. The GLBs keep
  them as separate objects (`Head`, `Gun`, `Barrel`, pivots in each README) for a later turret-drawing task.
- The rocket launcher's rockets don't leave the tubes as it fires; the launcher's trigger doesn't move.
- The molotov in flight is still the projectile shape; its glass is opaque.
- The incendiary, smoke and EMP launchers have no guns.json entry yet (they could share the GrenadeLauncher model).
