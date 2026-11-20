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
| Fireball | 5 flipbook sprites thrown out and up, each 1.3 to 1.8 s from white-hot through orange to dark smoke |
| Light | a shadow-casting point light: a white peak of 400,000 cd gone in about 0.05 s, under it an orange glow of 60,000 cd fading over a second, rising with the smoke (x radius / 1.9 m, x `Blast.LightScale`). A real fireball is about a million candelas; 14,000 did nothing visible at noon (PC round 2) |
| Smoke | 8 grey-brown puffs of soot and dust, staggered over half a second, lifting, swelling to 4 to 6 m (x the square root of radius / 1.9 m) and drifting with the sim's wind; they hang `Blast.SmokeSeconds` (14 s, each 70% to 120% of it) and glow orange from the fire under them for the first second |
| Dirt | 7 brown puffs thrown 5 to 9 m/s up from the ground under the blast that fall back as a haze over 2 to 3.5 s |
| Shock wave | a ring of 20 dust puffs racing out along the ground at 11 to 16 m/s and stopping 3 to 4 m out |
| Heat shimmer | a ring of bent light swelling from 1.2 to 11 m in a third of a second |
| Fragments | 40 glowing streaks at 15 to 35 m/s (each as long as it flies in 1/25 s) and 10 slower embers that arc down |
| Debris | `UShooterFeelSubsystem`'s Chaos rigid bodies, now dark soil, brown soil and grey stone clods, mostly small; the fast ones trail dust for their first second |
| Scorch | soot thrown out in rays with a pale dusty rim, 1.2 to 1.6 radii across, at a random turn, on a flat square laid on the ground along its slope 4 cm up (a deferred decal never showed on the PC); fades over `Blast.ScorchSeconds` (5 minutes, slowly at first), at most 40 at once |
| Camera | `UShooterFeelSubsystem`'s shake, blur and vignette by distance (unchanged) |

Everything scales with the blast's radius against the frag grenade's 1.9 m. A **fire blast** (damage type with `mix.sub`
Fire: a molotov) is a wide low splash of 9 fire sprites, 6 black smoke puffs for 6 to 9 s, 14 sparks and an orange glow;
no flash, shimmer or scorch (the sim lights the ground and leaves burnt cells, drawn by the fire subsystem).

Blasts beyond 600 m make only their light. Everything freezes while the game is paused and moves in real time otherwise.

## How it is drawn

Each kind (fire, smoke, sparks, shimmer) is one instanced mesh of the engine's 1 m plane, turned to face the camera every
frame and **sorted far to near** so the see-through layers blend right. Per-instance data (`BlastFxData.h`): flipbook
frame, opacity, tint, fire glow, a left-right mirror (so neighbours differ) and the orange glow from below.

The **flipbooks** are 8 x 8 frames of 256 px (2048 textures, BC7), drawn by marching rays through a noise volume:
billowing lumps (turbulence noise round a cluster of spheres) that swell and lift; for the fireball a heat field, hot in the
middle and cooling from the outside in. Each pixel keeps heat (R), the light the smoke gets from above (G, baked
self-shadowing), a little depth noise (B) and opacity (A). The materials (`M_BlastFire`, `M_BlastSmoke`) blend the two frames
either side of the sprite's moment, colour the smoke by its tint and baked light (lit translucent, volumetric, so the sun,
the sky and the blast's own light reach it), make the heat glow as a black body (dark red, orange, yellow, white) times
`Blast.FireBrightness` **through the inverse of the camera's exposure** (so a glow of 1 is plain white at noon, EV 15, and at night alike;
a plain emissive of 40 was invisible at noon in PC round 2), and fade softly where a sprite cuts into the ground (depth fade 60 cm).

All numbers are ours, found by eye in the previews (no RimWorld numbers apply: RimWorld draws a blast as a flat ring).

## Art and fallbacks

`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=BlastArt -unattended -nosplash -nullrhi [-force]` makes
`/Game/Rim3D/Generated/Blast` (not in Git): T_BlastFire, T_BlastSmoke, T_BlastScorch, M_BlastFire, M_BlastSmoke, M_BlastSpark,
M_BlastHaze, M_BlastScorch. It takes about a minute (the flipbooks are drawn on all cores). Without the art, or with
`Blast.Enable 0`, `UShooterFeelSubsystem` draws its old simple blast (coloured balls) as before.

If the fire comes out upside down on the PC (the plane's texture runs the other way), `Blast.FlipV 1` turns it.

## Console

`Blast.Enable`, `Blast.FireBrightness` (2), `Blast.LightScale` (1), `Blast.SmokeSeconds` (14), `Blast.ScorchSeconds` (300), `Blast.MaxSprites`
(1500), `Blast.FlipV`.

## Smoke test

`feel` ends with `blast_fireball` (a frag blast 11 m off to one side at 14:00 in a patch cleared of plants, the camera turned to it, pictured 0.3 s in; it must draw 40+ sprites, a light
over 2,000 cd and a scorch mark), `blast_smoke` (pictured about 4 s in), `blast_linger` (about 11 s in, looking down at the spot: 3+ smoke puffs and the
scorch still there), `blast_scorch` (a scorch mark alone, no smoke, looked down at from 5 m) and `blast_molotov` (a fire blast, pictured 0.5 s in). Without the art they note it and pass.

## Not yet

- **The scorch mark does not show on the PC** (2026-11-20, rounds 3 to 7): first as a deferred decal, then as a ground square on the
  traced ground (`LogBlastFx: Scorch at ...` is logged at the right place). The fireball, smoke and sparks with the same texture
  path render fine. To find: the material (`M_BlastScorch`) or `T_BlastScorch`'s alpha. Round 8 (follow-up): the material is
  two-sided, the log says what it is drawn with (`Scorch material ...`, `Scorch drawn: ...`) and `blast_scorch` pictures one
  alone; the earlier pictures had it 11 m off at a grazing angle under the smoke.

- Blasts in water (a column of spray), on snow (white powder) and on stone floors (grey dust): the dust is soil-coloured
  everywhere.
- Plants and grass flattened or burnt by the blast; ragdolls pushed by it (the sim's knock-down already throws pawns).
- Niagara or GPU particles: not needed at these counts.
