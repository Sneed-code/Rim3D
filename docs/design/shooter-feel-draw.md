# Shooter feel on screen

Code: `game/Source/Rim3D/ShooterFeelSubsystem.*` (`UShooterFeelSubsystem`: kick, shake, blur, grenades, blasts, debris, the
settings), `ProtagonistPawn.*` (the aim-hold blend and the camera nudges), `FightSubsystem.*` (the roll and grenade keys),
`PawnActor.*` (the roll's somersault), `OverseerSubsystem.cpp` (the settings panel). Smoke test: `feel`
(`tools/smoke.ps1 -Test feel`). Task U21. Unreal side only; the sim ([shooter-feel.md](shooter-feel.md) P2,
[explosives.md](explosives.md) F9) is untouched.

**The sim decides, Unreal shows.** Every number below is read from the snapshot or an event; no offset here reaches the aim.
The view direction the shots use (`AProtagonistPawn::GetViewDirection`) is the player's aim without the kick, the shake or
a roll's dip, so the crosshair is where the bullet goes (the smoke test checks the aim never moves with the kick, to 0.001
degrees).

## Controls (direct mode)

| Key | What |
| --- | --- |
| Hold right mouse button, gun in hand | aim: the view blends to over the shoulder (camera 120 cm back and 42 cm to the right, field of view 80 to 62 degrees) in about a fifth of a second, and back when let go. `Feel.AimView 1` goes to first person instead (camera to the eyes, body hidden). A melee weapon blocks with the same button |
| Left Shift | dodge roll in the direction of the movement keys (relative to the view; none held is a backstep): `DodgeRollCommand`. Refused while rolling or waiting out the last roll |
| G | throw the first grenade on the belt at the crosshair's point (`ThrowCommand` with `player`) |

## What the camera does

`UShooterFeelSubsystem::GetCameraFeel()` (worked out once a frame) gives the camera component a rotation and a place relative
to the aim, and the post-process:

- **Gun kick**: the snapshot's `kickUp` and `kickSide` (degrees, what P2's sim has now) lift and swing the view by that much
  times `Feel.Kick`. It holds for 5 steps after a shot and wears off by itself, as the sim says.
- **Shake**: a few sines per shake, dying away squared over its time, started by sim events: the player's shots (0.22 degrees
  plus 0.07 per degree of the gun's first kick, 0.13 s), a hit on the player (1.1 degrees, 0.3 s), a knock-down (3.5 degrees,
  0.9 s), a blast by its nearness (5.5 degrees x a strength from its radius x closeness squared, out to 6 radii plus 30 m).
  Times `Feel.Shake`.
- **Roll**: the camera ducks 55 cm and tips down 9 degrees under a roll, following the sim's 27 steps.
- **Motion blur, vignette, chromatic fringe**: blur is 0.5 (the engine's) times `Feel.Blur`, plus a pulse of up to 0.8 near
  blasts and in the rolling steps; the vignette (on top of the engine's 0.4) and the fringe pulse when the player is hit or
  knocked down and near blasts.

## Roll animation

There is no roll animation asset (Mixamo needs an Adobe sign-in), so the body does a **forward somersault**: over the 15
rolling steps (the turn is fitted to the sim's `rollPhase` and `rollStepsLeft`, so it is always in time with the sim) the body
turns over 360 degrees about its middle, ducking 22 cm, along the sim's roll direction (`rollDirX/Y`). The sim moves the pawn 4 m
and the actor follows the snapshot; the animation never moves it. A real roll montage over the motion-matched body can replace
this later (pitfall 3 in shooter-feel.md).

## Grenades and blasts

- A grenade (a projectile whose def has `explosion`) in the air or lying with its fuse lit is a small olive sphere that spins,
  blinking faster as the fuse runs out.
- An `Explosion` event (a grenade, the dev command, later traps and bombs) makes: a fireball (yellow core spheres and orange outer ones
  that swell and burn out in half a second), smoke that rises and swells, sparks with gravity, dust, a flash of light (9,000
  candelas at its peak for the frag grenade, gone in half a second), and the camera's shake, blur and vignette by nearness. All
  scale with the blast's radius.
- **Debris**: 10 plus 8 per metre of radius small cubes (at most 60 a blast, 140 alive) thrown outward and up as Chaos rigid bodies,
  landing on an invisible 30 m floor made under the blast and leaning with the ground there (the ground is drawn from the sim and has
  no collision), gone after 5 seconds. Pieces ignore each other and pawns. Debris is for looks only: it never decides who is hurt.
  `Feel.Debris 0` turns it off.

## Settings

Console variables `Feel.Shake`, `Feel.Blur`, `Feel.Kick` (0 turns each off, 1 is normal, 2 double), `Feel.AimFov`, `Feel.AimView`,
`Feel.Debris`. The overseer view has a folded **Feel** panel (top right, under the alarm button) with sliders for them; they are
kept in the user settings file (`GameUserSettings.ini`, section `Rim3DFeel`) and read at the next start. (A value typed in the
console after a start wins over the slider for that run.)

## Smoke test `feel`

A flat 128 m map with the Crashlanded start, the protagonist in third person with an assault rifle, at midday. It checks: the
camera is not nudged when nothing happens; holding aim gives blend 1, the 62 degree field of view and a camera 120 cm back with the body
still drawn; the first person aim view hides the body and puts the camera at the eyes; letting go restores everything; a burst lifts
the view (up to 2.3 degrees), moves the aim by nothing, shakes the camera and settles to 0; with shake, kick and blur at 0 the view does not
move and the settings are saved; a roll (the sim moves the pawn 4 m, the body turns over 360 degrees, the camera ducks 8 degrees, the
blur rises to 1, a second roll is refused); a thrown grenade is drawn in the air, then its blast makes particles, a flash, shake, blur
and 25 debris pieces that go about 6 m out and 2 m up without falling through the ground and are gone after 5 s; a 5 m blast 9 m away
shakes the camera over 5 degrees. About 45 s, 220 to 240 fps at 888 x 500, game thread 4 ms.

## Not done

Aim-down-sights sight picture and a held gun that follows the aim; a real roll animation; hit-stop for gunfire; camera shake assets
(a procedural shake stands in); blast decals, scorched ground and fire from blasts (E4's fire starts from Burn blasts through the sim);
debris from walls (a destroyed building leaves nothing to fall yet); sound; the kick for AI fighters (none by design); stance effects on
the kick; a `Feel` panel in direct mode itself (the mouse is the camera there, so the sliders live in the overseer view).
