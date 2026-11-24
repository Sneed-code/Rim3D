# Blasts on screen

Code: `game/Source/Rim3D/BlastFxSubsystem.*` (`UBlastFxSubsystem`), names shared with the art in `BlastFxData.h`; the art
from `game/Source/Rim3DEditor/BlastArtCommandlet.*` and the flipbook drawing in `BlastFlipbooks.h` (plain C++). Previews
without Unreal: `tools/blast_flipbook_preview.cpp` and `tools/blast_preview.py`. The camera's feel and the debris stay in
`UShooterFeelSubsystem` ([shooter-feel-draw.md](shooter-feel-draw.md)). Sound is the sound subsystem's ([sound.md](sound.md):
recorded blasts, a far layer past 100 m, heard late by the speed of sound).

**The sim decides, Unreal shows.** The sim's `Explosion` event (centre, radius, damage type) has already hurt who it hurts
([explosives.md](explosives.md)); everything here is for looks. sneed 2026-11-19: "make explosions look realistic".

## What a frag blast looks like

| What | How |
| --- | --- |
| Flash | a white-hot fireball sprite for 0.14 s (the flipbook's first frames, 3 x the fire's glow) |
| Fireball | 7 flipbook sprites thrown out and up, each 1.3 to 1.8 s from white-hot through orange to dark smoke, and 6 smaller white-hot pockets burning out inside it in half a second so the ball churns (sneed 2026-11-23: "far more realistic detailed explosions") |
| Smoke column | 6 soot puffs released over the first second, rising at 1.5 to 2.4 m/s and swelling to 5 to 6 m, lit orange from below at first: the cloud climbs into a stem and cap instead of only spreading |
| Burning debris | 4 to 16 burning bits flung out at 6 to 13 m/s trailing smoke and flame, each smouldering a few seconds where it lands, and 3 small fires left in the crater for 1.5 to 3.5 s (the same burning pools as a molotov's, smaller and smokier) |
| Light | a shadow-casting point light: a white peak of 400,000 cd gone in about 0.05 s, under it an orange glow of 60,000 cd fading over a second, rising with the smoke (x radius / 1.9 m, x `Blast.LightScale`). A real fireball is about a million candelas; 14,000 did nothing visible at noon (PC round 2) |
| Smoke | 8 grey-brown puffs of soot and dust, staggered over half a second, lifting, swelling to 4 to 6 m (x the square root of radius / 1.9 m) and drifting with the sim's wind; they hang `Blast.SmokeSeconds` (14 s, each 70% to 120% of it) and glow orange from the fire under them for the first second |
| Dirt | 7 brown puffs thrown 5 to 9 m/s up from the ground under the blast that fall back as a haze over 2 to 3.5 s |
| Shock wave | a ring of 20 dust puffs racing out along the ground at 11 to 16 m/s and stopping 3 to 4 m out |
| Heat shimmer | a ring of bent light swelling from 1.2 to 11 m in a third of a second |
| Fragments | 40 glowing streaks at 15 to 35 m/s (each as long as it flies in 1/25 s) and 10 slower embers that arc down |
| Soil jets | 8 to 30 jets of dark earth flung up in a cone from the crater at 9 to 15 m/s, three blobs down each so it stretches as it flies, raining back down over 2 s |
| Debris | `UShooterFeelSubsystem`'s Chaos rigid bodies: lumpy clods of dark and brown soil and a few grey stones (320-triangle noise-pushed balls, smooth shaded, mottled colour, rolling on a sphere), mostly small; the fast ones trail dust for their first second. sneed 2026-11-22: the old 10 cm cubes read as "ultra low poly" |
| Scorch | a ragged charred core (mottled soot darker along cracks, patches of grey ash, a little sheen, embers a few centimetres across glowing and flickering for the first half minute) with spokes of soot thrown out past it, pale blown-out dust between the spokes, a small pit of churned soil in the middle and dark fragment pocks, 1.4 to 1.8 radii across, at a random turn, on a flat square laid on the ground along its slope 4 cm up (a deferred decal never showed on the PC); fades over `Blast.ScorchSeconds` (5 minutes, slowly at first), at most 40 at once |
| Camera | `UShooterFeelSubsystem`'s shake, blur and vignette by distance (unchanged) |

**In flight**, a rocket is a rocket (`UShooterFeelSubsystem`, the shape in `RocketShape.h`: an olive body 55 cm long with a
pointed nose, a yellow band, four swept fins and a dark nozzle, nose along its flight, turning slowly; a def's
`visual.parts.0.size.0` over 0.45 m scales it), not a streak. It leaves pale grey exhaust smoke (45 puffs a second, each
swelling from 70 cm to 2.8 to 3.8 m over 3 to 4 s, drifting with the wind) and a strong thrust plume: a white-hot core at
the nozzle and a tongue of flame about 1.2 m long streaming back, each piece drawn long along the flight line and
flickering in length, and a 14,000 cd motor light carried with the nearest rocket; a launcher shell a thin grey puff; a molotov small flames
(`UBlastFxSubsystem::AddTrail`, from the Projectiles def's `visual.trail`). Without the art the old solid balls stay.

Everything scales with the blast's radius against the frag grenade's 1.9 m.

A **fire blast** (damage type with `mix.sub` Fire: a molotov) is **liquid fire** (sneed 2026-11-23: "photorealistic
proper liquid fire with molotovs"): a soft orange whoomp of 10 wide fire sprites as the vapour goes up, 18 sparks of glass
and fuel, about 16 burning drops of fuel flung out in arcs trailing flame (45 small flames a second each) that splash into
small burning pools where they land, and the splash itself: a pool 0.8 of the radius across where it burst with 3 to 5
lobes splashed round it, so the burning patch is ragged. A pool spreads to full size in a third of a second, burns
`Blast.FuelSeconds` (11 s; lobes and drops less) and dies down over the last 40%, covered in tongues of the photographed
fire flipbook packed 30 a second per square metre (tallest in the middle where the fuel is deepest), a column of black
soot, embers lifting off, and a flickering shadow-casting light from the biggest pool (9,000 cd x the square root of its
burning area, `Blast.PoolLight`). It leaves a dark stain (a fainter scorch mark) under the sim's burnt ground. The sim lights
the cells under it, and those fires are drawn the same way (below), so the fire carries on seamlessly when the fuel is gone.

**Every fire in the game** (burning ground, burning pawns, campfires, torches) draws its flames and smoke here too
(`UBlastFxSubsystem::AddFlames`, called by `UFireSubsystem`): tall tongues of the fire flipbook (1.5 to 2.2 times as tall as
wide, kept upright as seen, at least 10 a second per fire so a torch never gaps) and soot rising off it; the embers and
lights stay the fire subsystem's. `Fire.Flipbook 0` (or no blast art) goes back to the simple cones.

Blasts beyond 600 m make only their light. Everything freezes while the game is paused and moves in real time otherwise.

## How it is drawn

Each kind (fire, smoke, sparks, shimmer) is one instanced mesh of the engine's 1 m plane, turned to face the camera every
frame and **sorted far to near** so the see-through layers blend right. Per-instance data (`BlastFxData.h`): flipbook
frame, opacity, tint, fire glow, a left-right mirror (so neighbours differ) and the orange glow from below.

The **flipbooks** are 8 x 8 frames of 512 px (4096 textures, BC7, never streamed), drawn by marching rays (128 steps, 6 toward
the light) through a noise volume: billowing lumps (6 octaves of gradient-noise turbulence round a cluster of spheres) that swell and lift; for the fireball a heat field, hot in the
middle and cooling from the outside in. Each pixel keeps heat (R), the light the smoke gets from above (G, baked
self-shadowing), a little depth noise (B) and opacity (A). The materials (`M_BlastFire`, `M_BlastSmoke`) blend the two frames
either side of the sprite's moment, colour the smoke by its tint and baked light (lit translucent, volumetric, so the sun,
the sky and the blast's own light reach it), make the heat glow as a black body (dark red, orange, yellow, white) times
`Blast.FireBrightness` **through the inverse of the camera's exposure** (so a glow of 1 is plain white at noon, EV 15, and at night alike;
a plain emissive of 40 was invisible at noon in PC round 2), and fade softly where a sprite cuts into the ground (depth fade 60 cm).

sneed 2026-11-22 ("the explosion graphic is low pixel"): the frames were 256 px, the texture was streamed at a small mip
(the streamer judges a 1 m plane, not the 5 m it is scaled to), the clouds were cut off square at the frame's edges (the
volume reached past what a frame showed) and value noise drew boxy lumps. Now 512 px, never streamed, the frame shows
the volume's -1.05..1.05 with a soft fade before the edge (sprites drawn `BlastFxData::SpriteScale` = 1.05 / 0.74 bigger to
keep their size), and gradient-noise turbulence draws round cauliflower billows.

All numbers are ours, found by eye in the previews (no RimWorld numbers apply: RimWorld draws a blast as a flat ring).

## Art and fallbacks

`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=BlastArt -unattended -nosplash -nullrhi [-force]` makes
`/Game/Rim3D/Generated/Blast` (not in Git): T_BlastFire, T_BlastSmoke, T_BlastScorch, M_BlastFire, M_BlastSmoke, M_BlastSpark,
M_BlastHaze, M_BlastScorch. It takes a few minutes (the flipbooks are drawn on all cores). Without the art, or with
`Blast.Enable 0`, `UShooterFeelSubsystem` draws its old simple blast (coloured balls) as before.

If the fire comes out upside down on the PC (the plane's texture runs the other way), `Blast.FlipV 1` turns it.

## Console

`Blast.Enable`, `Blast.FireBrightness` (2), `Blast.LightScale` (1), `Blast.SmokeSeconds` (14), `Blast.ScorchSeconds` (300), `Blast.MaxSprites`
(2600, raised from 1500 for the fires' flames), `Blast.FuelSeconds` (11), `Blast.PoolLight` (1), `Fire.Flipbook` (1), `Blast.FlipV`.

## Smoke test

`feel` ends with `blast_fireball` (a frag blast 11 m off to one side at 14:00 in a patch cleared of plants, the camera turned to it, pictured 0.3 s in; it must draw 40+ sprites, a light
over 2,000 cd and a scorch mark), `blast_smoke` (pictured about 4 s in), `blast_linger` (about 11 s in, looking at the hanging cloud: 3+ smoke puffs and the
scorch still there), `blast_scorch` (a scorch mark alone, no smoke, looked down at from 5 m), `blast_scorch_close` (a fresh one 3 m off at dusk, for its embers) and `blast_molotov` (a fire blast, pictured 1.1 s in when the splash lies burning; it must make 3+ burning pools and 5+ burning drops), `blast_jets` (a frag blast 8 m off at 0.45 s: soil jets, billows and clods) and `blast_rocket` (a rocket trail laid by `AddTrailForTest` to a 3.9 m blast, at 0.25 s). Without the art they note it and pass.

## Not yet

- The scorch mark showed first in PC round 8 (`blast_scorch`, a mark alone looked down at from 5 m): the earlier pictures had
  it 11 m off at a grazing angle under the smoke, and the old texture was a soft round blotch that read as a shadow. Round 9
  drew it as a starburst (reads as burnt); its pale dust was then made lighter. `blast_linger` now pictures the cloud only:
  at 11 m the mark sits behind the colonists or the player's body.

- Blasts in water (a column of spray), on snow (white powder) and on stone floors (grey dust): the dust is soil-coloured
  everywhere.
- Plants and grass flattened or burnt by the blast; ragdolls pushed by it (the sim's knock-down already throws pawns).
- Niagara or GPU particles: not needed at these counts.
