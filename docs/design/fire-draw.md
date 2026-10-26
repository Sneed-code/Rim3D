# Light and fire on screen

Code: `game/Source/Rim3D/FireSubsystem.*` (`UFireSubsystem`: lights, flames, embers, smoke, burning pawns, burnt ground
and ash). Data: `glower.lift` and `visual.flame` in `data/defs/Things/BuildingsPowerClimate.json`. Smoke test:
`fire` (`tools/smoke.ps1 -Test fire`). Task U14. Unreal side only; the sim ([light.md](light.md) E3, [fire.md](fire.md) E4)
is untouched.

**The sim decides, Unreal shows.** Nothing here lights or burns anything. Lamps come from `sim.lights().sources()`,
fires from the snapshot's `fires`, burning pawns from `ThingView::onFire`, burnt ground from `burnt` and ash from the
`Ash` filth. A lamp that runs out of fuel or has no power is not in the sim's list, so its light goes out the same frame.

## What is on screen

| What | From | How |
| --- | --- | --- |
| Lamp light | `lights().sources()` (building, radius, glow) | a point light at the building, coloured by the def's `glower.color`, `glower.lift` metres up (campfire 0.3, torch 0.35, standing lamp 1.4, sun lamp 2.1), 40 candelas times the glow, out to the sim's radius (10 m). Lamps with a fuel part (campfire, torch) flicker |
| Lamp flames | `visual.flame` on the def (`lift`, `size`) | the campfire and the torch lamp have flames and embers; the campfire smokes a little |
| Fire | the snapshot's `fires` (cell, size 0 to 1.75) | per burning cell: flames (cones that go yellow, orange, then red as they rise and shrink), embers that drift up and wink out, smoke that rises and leans with the wind. How many and how big follow the fire's size, which the drawn fire follows over about a second (so a fire the rain shrinks, or one that spreads, changes smoothly) |
| Fire light | the same | one orange light for each 4 m block of fire, 10 candelas plus 3.5 per unit of size (at most 60), radius 5.5 to 15 m, flickering |
| Burning pawn | `ThingView::onFire` | flames over the body (to head height), a small orange light that follows it |
| Burnt ground | the snapshot's `burnt` cells | a charcoal skin on the ground, a little lighter or darker at each corner so it has no cell edges; it goes when the sim lets the burn fade (6 days) |
| Ash | `Ash` filth cells | a small dark-grey patch, turned and moved a little by the cell |

Only the nearest `Fire.MaxLights` (24) lights are real; the rest of the lamps and fires are simply not lit (a lamp across
the map has nothing near it to light). The four nearest lamps and fires cast shadows. At most `Fire.MaxParticles` (3,500)
flames, embers and smoke puffs are alive; emitters farther than 150 m from the camera make none. Everything freezes while the
game is paused and moves in real time otherwise.

## How it is drawn

Particles are instanced meshes on the engine's vertex colour material (the same path as the fight effects, so DLSS stays
possible), moved on the CPU. The engine's unlit view-mode material would have been better for flames, but it is not usable on
instanced meshes (it draws as the default grey), so flames are lit like everything else: they take the colour of the light
over them, so at night they glow orange and in daylight they show their own colour. Real Niagara fire is a later job.

The burnt ground and ash are procedural meshes, one per 32 m chunk (the terrain's and the snow's), made only for chunks that
have some and remade only when the cells in a chunk changed, a few chunks a frame.

## Numbers

All ours, found by eye in the `fire` smoke test's screenshots (the engine's default exposure puts a clear noon at 10 lux, so a
candela at a few metres is a lot of light): `FireLampCandelas` 40, `FireCellCandelas` 10, `FireCandelasPerSize` 3.5, flame
base height 58 cm, 20 flames, 2.4 embers and 1.5 smoke puffs a second for a fire of size 1. They are constants at the top of
`FireSubsystem.cpp`; `Fire.LampIntensity` scales every light.

## Console

`Fire.Enable` (0 draws none of it), `Fire.MaxLights`, `Fire.MaxParticles`, `Fire.LampIntensity`, `Fire.Smoke` (0 draws no smoke).

## Smoke test `fire`

A flat 96 m map at night (`Sky.Hour 0.5`). It lights a campfire and a torch (with fuel), a standing lamp on a generator's wire
and one with no wire, and checks: the sim lists exactly the three lit lamps; each has a warm light of its own; the campfire and
the torch have flames and embers; the dark lamp has no light. Then wood piles are set alight (88 fires after the grass catches):
the drawn fires are the sim's fires (cells and total size within 35%), hundreds of flames and embers and smoke puffs, an orange
fire light. A burning pawn has flames and a light. Six game hours later the burnt ground and ash drawn are **exactly the
sim's burnt cells and ash piles**, no chunk takes more than 50 ms. Last, 200 piles alight on a fresh map: the particle cap (3,500)
and the light cap (24) hold. Frame times: 240 to 300 fps at 888 x 500 on the RTX 5080, game thread 3 to 3.6 ms. Screenshots in
`game/Saved/Smoke/fire/`.

## Not done

Niagara fire with soft sprites and heat shimmer; fire under roofs lighting the room through windows (the lights are real, so
walls shade them, but there is no glow on the ceiling); the home-area overlay (U19); sparks flying from lightning strikes;
burnt plants and scorched walls and trees (a burnt tree is gone from the snapshot, so only the ground shows it); rain steam;
sound; a burning pawn running about (the sim has none yet).
