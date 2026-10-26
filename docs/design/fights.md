# Fights on screen

Code: `game/Source/Rim3D/FightSubsystem.*` (the effects, the protagonist's gun, the hitbox view),
`PawnActor.*` (hit reactions, ragdolls, facing), `game/Source/Rim3DEditor/FightMaterialsCommandlet.*`
(the decal material). Smoke test: `fights` (`tools/smoke.ps1 -Test fights`). Task U11. Unreal side only;
the sim is untouched.

**The sim decides, Unreal shows.** Everything here is drawn from the sim's `ShotFired` and
`ProjectileHit` events and the snapshot ([combat.md](combat.md), [bridge.md](bridge.md)). Nothing here
decides who is hit; a bug in these effects cannot change a fight. The one way the player affects a
fight is a `ShootCommand` (and `ReloadCommand`, `SetStanceCommand`, `EquipCommand`), like any pawn.

## What is on screen

| Effect | When | How |
| --- | --- | --- |
| Muzzle flash | `ShotFired` | two bright blobs (0.05 s and 0.1 s) at the muzzle the sim reports, a short point light (six in a pool, no shadows), two puffs of smoke |
| Tracer | `ShotFired` to `ProjectileHit` | a thin streak that flies from the muzzle to where the bullet ended, at the bullet's own speed in **real time** (not one jump per sim step), 1.5 to 18 m long, longer when it is faster; follows the game speed and freezes when paused |
| Impact | the streak arrives | ground: dust and a puff; rock: dust, chips and a few sparks; building: sparks, chips and dust; armor: sparks and a small flinch; flesh: a spray of blood. Bullets that fly off the map draw nothing |
| Hit reaction | a bullet hits a pawn | the pawn leans away from the shot (up to 14 degrees, snapping over in 0.08 s and settling in half a second; 30% of that for armor that stopped the bullet) |
| Ragdoll | the pawn is `downed` or `dead` in the snapshot | the mannequin goes limp and falls under physics, pushed by the last hit; the MetaHuman follows it through its leader pose; gets up again (animation takes over) if the sim says the pawn is up |
| Wound decal | a bullet hits flesh | a dark red patch on the body at the nearest bone where it hit (stays with the bone), and a blood pool on the ground; 160 decals at most, the oldest go first |
| Held gun | a pawn has a gun | a placeholder from the weapon def's `visual.parts` floating at the chest, pointing where the pawn faces (118 cm up standing, 78 crouching), tilted to the camera's pitch for the player; hidden for pawns that are down |
| Facing | a pawn is aiming or just shot | when standing still it turns to the sim's `facingX/facingY`; walking, it faces where it walks (the mannequin has no strafe animation yet) |

**Cosmetic means cosmetic.** Particles, tracers and flashes are drawn by a few instanced meshes on the
engine's vertex colour material (the same path as buildings and plants), simulated on the CPU in game
time. Numbers (counts, lifetimes, sizes) are constants at the top of `FightSubsystem.cpp`. The random
choices use a fixed stream, not the sim's: they never touch the sim's state. `Fight.Effects 0` turns
all of it off.

**Ragdoll floor.** The ground is drawn from the sim and has no collision, so a ragdoll gets an
invisible 10 m x 10 m slab made under the pawn, tilted to the ground's slope, that goes away when the
pawn gets up. The mannequin's physics asset is `PA_UEFN_Mannequin`.

**Wound decals** need `game/Content/Rim3D/Generated/Fights/M_WoundDecal` (not in Git, `assets/manifest.json`
says how to make it): `UnrealEditor-Cmd.exe game\Rim3D.uproject -run=FightMaterials -unattended -nosplash -nullrhi`.
Without it the effects still draw, minus decals, and the log says so.

## The protagonist's gun

In direct mode ([protagonist.md](protagonist.md)):

| Key | Does |
| --- | --- |
| left mouse button | fire at the crosshair (held: as often as the gun allows) |
| R | reload now (guns also reload by themselves when empty) |
| C | crouch or stand; Z lie down or stand |
| E | pick up the nearest gun within 8 m (a command; the pawn takes it at once) |

Firing sends `ShootCommand{pawn, point, player = true}`: the player aims, so there is no warm-up and the
spread is the gun's own, plus recoil ([combat.md](combat.md)). The point comes from the sim's own trace
(`CombatSystem::trace`) from the camera along the look direction (starting beside the colonist in
third person, so a wall behind them doesn't count): the ground, rock, a building or a pawn's hitbox the
crosshair is on, else a point 150 m out. One order per sim step, only when the gun is ready, never
while paused. The shot leaves the **muzzle** (above the pawn's centre, by stance), aimed at that point
with drop allowed for. The HUD at the bottom shows the gun, rounds and `reloading`; a red cross round
the crosshair flashes when a shot of yours hits a pawn.

## The hitbox view

`Fight.Hitboxes 1` (or `UFightSubsystem::SetShowHitboxes`) draws every pawn's hitboxes within 70 m: the
capsules and boxes of `CombatSystem::hitboxesOf`, coloured by body part, on top of everything. They are
placed where the pawn is drawn (shifted by how far the blend between two sim steps has moved the body).

To check that hits match bodies the subsystem tests every pawn hit against the hitboxes as the sim has
them when the event arrives (`DistanceToHitboxesCm`, kept in `GetHitLog()`); the smoke test requires
every hit to lie within 5 cm of a hitbox (it is on the surface). It also compares the union of the
target's hitboxes with the drawn body's bounds, which must agree to 60 cm in height and position.

## Smoke test `fights` (about 90 s)

On a 128 m flat map with the Crashlanded start and three plain pawns 12 m east of the protagonist:

1. `setup`: the sim, three pawns, the protagonist in direct mode.
2. `equip_and_aim`: E picks up a gun (or the test equips one by command), the placeholder gun shows
   at the chest, C crouches and the gun comes down, and the crosshair finds the middle pawn's chest.
3. `fire_at_pawn`: the trigger is held for 14 s: every shot has a flash and a tracer, bullets make
   impacts, at least one hits a pawn and that pawn flinches, shows a wound decal and blood on the ground,
   the player gets a hit marker, R reloads, and every hit is within 5 cm of the pawn's hitboxes.
4. `frozen_shot`: one shot is stopped (game paused) while the tracer is in the air: screenshot of the
   flash, light and streak; the tracer does not move while paused.
5. `frozen_hit`: shots until one hits a pawn, stopped a moment later: blood in the air, the pawn leaning.
6. `wounds`: a close screenshot from the shooter's side of the pawn that was hit last, with its decals.
7. `hitbox_view`: the debug view on, from the side: shapes drawn and matching the drawn body.
8. `ragdoll`: 400 damage to a pawn's torso: it is a ragdoll, its pelvis lies low after falling and it
   stays near where it fell; every pawn that is down is limp and every one that is up is not.
9. `firefight`: four colonists (drafted, assault rifles) and four raiders 30 m away: shots, flashes
   and tracers match, nothing is left dangling, hits match the hitboxes.

The keyboard and mouse themselves are not covered (the test drives the same methods); a person has to
try the left mouse button, R, C, Z and E once.

## Not done here

- **Melee, weapon switching and holstered weapons** are U22: [melee-draw.md](melee-draw.md). The held weapon is drawn by `UWeaponDrawSubsystem` since then.
- **A real gun in the hands.** The gun floats at the chest, and nobody has an aiming or recoil
  animation; the sample's aiming databases (and strafe) are needed, with real weapon meshes (A1).
  In first person the player's own body is hidden, so the gun is too: only flashes, tracers and impacts show.
- **Sounds.** None yet (nothing in Stage 1 has sound).
- **Decals on the ground are flat**, round and one colour; wounds don't change with the hurt body part,
  and bodies don't show their wounds after the decal cap.
- **Hit reactions** are a lean, not an animation; pawns hit while walking keep walking.
- **Dead pawns** stay as ragdolls for as long as the sim keeps them; corpse removal is a later task.
- **MetaHuman ragdolls** go through the leader pose (the hidden mannequin simulates); strand hair and
  clothes follow it but have not been looked at closely.
- **Grenades and explosions** have no sim yet.
