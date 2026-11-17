# The shuttle and landing zone on screen

sneed (2026-11-17): the Unreal side of `landing.md`. The sim decides where a shuttle stands, which way it faces, its phase
and how far through it is (`Snapshot::shuttles`); everything here is looks. All numbers are ours.

Code: `game/Source/Rim3D/ShuttleSubsystem.*` (`UShuttleSubsystem`), the landing zone tool in `OrdersSubsystem.cpp`, the
effect materials in `Rim3DEditor/ShuttleArtCommandlet.*`, the grass push in `FoliageMaterialsCommandlet.cpp`, the model
import in `GunArtCommandlet.cpp`. Tuning: `assets/shuttle.json` (read at start and on **F6**). Smoke test `shuttle`.

## The model

The Codex `VTOLShuttle` (12.0 x 6.0 m, 4.7 m tall, 68.5k triangles, 4K textures, two pilot and eight passenger seats).
Its game files are copied from `Rim3D_Codex/VTOLShuttle/game/` on sneed's PC into `assets/raw/vehicles/VTOLShuttle/`
(not in Git) and imported by `GunArt -List=assets/shuttle.json` into `/Game/Rim3D/Generated/Vehicles/VTOLShuttle/`: one
static mesh per moving part (body, roof, two locker doors, door, step, eight lift vanes, two rear nacelles, four gear
legs), on `M_GunPBR` with base colour, normal, ORM and an emissive map (cabin strip lights, screens, nozzle rings).

The meshes come in with their vertices in the model's frame, so each part hangs under a pivot component placed at its
pivot point (measured from the Blender file, listed under `parts`) and offset back by the same amount; turning the pivot
turns the part about the right point. Model frame: +X nose, +Y right (the door side), +Z up, cm. The model's middle
(`modelCentre`) goes over the footprint's middle and its skids on the ground (`groundLift`).

Facing: Rot4 north puts the nose to +y and the door on the -x side, which is the model's right; the actor's yaw is
90 - 90 x rot. Without the model the def's box parts are drawn instead (turned to the model's frame).

The building the shuttle stands as (`Shuttle_VTOL`, `visual.drawnBy: "shuttle"`) is skipped by the building subsystem;
picking it still works from the sim.

## The flight

From the phase and progress (blended between the last two snapshots, so it is smooth at any speed; the parts' idle
motions run on game time, so pausing freezes it):

| Phase | Progress | What it does |
| --- | --- | --- |
| Descending (12 s) | 0 to 0.45 | flies in nose first from 160 m back (`approachM`) at 45 m (`cruiseHeightM`), braking to a hover at 22 m (`hoverHeightM`) over the spot, nose up to 8 degrees while braking; the rear nacelles swing from back to down over 0.2 to 0.45; lift vanes tilt to brake |
| | 0.45 to 1 | straight down, slow at both ends; small hover wobble and bob that settle as it nears the ground; gear comes down over 0.62 to 0.79 |
| Landed | | jets spool down over 2.5 s; 2 s after touchdown the door pops out and slides back along the hull and the step folds down (1.5 s) |
| Ascending (10 s) | 0 to 0.15 | door shuts, jets spool up |
| | 0.15 to 0.5 | straight up to the hover height; gear in over 0.28 to 0.48 |
| | 0.5 to 1 | nacelles swing back (0.48 to 0.78), off forward accelerating and climbing to cruise height, nose down a little |

A shuttle first seen already landed (a loaded game) does not replay the door.

In the overseer view with roofs hidden (`Terrain.Roofs`), a landed shuttle's roof is hidden so its cabin shows.

## Flames, light, dust and grass

- **Flames**: from each of the eight lift nozzles (down along the vane) and the two rear nozzles (out the back of the
  nacelle, so they point down in a hover): an outer orange cone and a pale blue core, as long as the jet burns hard
  (lift 1.5 m, rear 3.8 m at full), flickering. Instanced meshes on `M_JetFlame` (unlit, additive, per-instance glow).
- **Light**: one point light under the hull, as bright as the jets (400 cd at full).
- **Dust**: below 18 m the lift jets blow dust puffs out from under the hull in every direction, more and faster the lower
  it is; they slow, grow and thin away over about 2.4 s. Instanced spheres on `M_JetDust` (lit, translucent, per-instance
  opacity). `Shuttle.Dust 0` turns it off.
- **Grass**: below 20 m the plants within 14 m lean away from the shuttle and flutter, harder the lower and closer
  (`MPC_Downwash` Wash0 and Wash1, x y radius strength, read in `M_Foliage` and `M_FoliageTex`'s world position offset;
  two shuttles at once at most). The plants under the footprint are flattened by the sim on touchdown anyway.

## The landing zone tool

Architect menu, **Zones**: **Landing zone** (drag rectangles to add cells) and **Remove landing zone**
(`SetLandingZoneCommand{area, add}`). The zone is drawn as faint amber tiles, slightly above stockpile and growing tiles.

## Steps on a new checkout (sneed's PC)

1. Copy the files from `Rim3D_Codex/VTOLShuttle/game/` into `assets/raw/vehicles/VTOLShuttle/`.
2. `UnrealEditor-Cmd.exe Rim3D.uproject -run=GunArt -List=assets/shuttle.json -unattended -nosplash -nullrhi`
3. `UnrealEditor-Cmd.exe Rim3D.uproject -run=ShuttleArt -unattended -nosplash`
4. `UnrealEditor-Cmd.exe Rim3D.uproject -run=FoliageMaterials -unattended -nosplash` (for the grass push)

## Not done (BACKLOG)

- Passengers drawn in their seats during the flight, and walking out down the step (the sim keeps them aboard until
  touchdown and puts them at the door cell).
- Sounds: jet roar by thrust and distance, gear and door.
- Locker doors open, landing and nav lights blinking.
- Scorch marks on the ground after a take-off.
