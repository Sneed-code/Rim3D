# Shooter feel: recoil, shake, blur, explosions, dodge roll

Research note, 2026-10-25. The sim half (gun kick and dodge roll) is built by P2, see "Built (P2)" at the end; the Unreal half (shake, blur, camera blend, animation, effects) is U21. The rest says what exists, what to use and what to watch for.
Fit with the rules: the sim decides what happens, Unreal shows it.

## What is already planned
Real projectiles with speed, drop and spread, explosions hitting everything in a radius, ragdolls,
gore, fighters dodging grenades (`game-design.md` combat; F2, F5, U11). Recoil and spread are in
F2's weapon rows. Not planned anywhere: camera shake, motion blur, recoil patterns for the player's
own gun, explosion screen effects, and a protagonist dodge roll.

## Camera rule (sneed, 2026-10-25)
Third person by default. Holding aim switches to an aimed view (first person or over-the-shoulder,
to be tried both ways); letting go returns to third person. V still forces first or third. The
camera code from U9 has both views; this needs an aim-hold trigger and a smooth blend.

## Melee feel
Same idea as guns: the sim decides, Unreal shows. Swings, blocks and dodges are timed in the sim
(wind-up, hit moment, recovery). Unreal adds a hit-stop (a 50 to 100 ms freeze of the picture),
camera shake, a small body flinch on the target (additive hit-react animation, or ragdoll for big
hits), blood and sparks. Free melee animation packs are on Mixamo and Fab; retarget like the roll.
Pitfall: weapon trace in Unreal must not decide hits; the sim checks reach and hitboxes.

## Tools (all free)
| Need | Use | Effort |
| --- | --- | --- |
| Recoil / spray pattern | Not built into Unreal. A per-weapon list of kicks (up, sideways, per shot number) in the weapon def, played by the sim; the camera kick and gun bounce are Unreal-side, driven by the same numbers | Small |
| Camera shake | Unreal's built-in camera shake assets (Perlin and wave types); one per weapon and per explosion size, started by sim events | Small |
| Motion blur | Built-in post-process setting; also a short blur/vignette pulse near explosions | Tiny |
| Rockets and grenades | Sim projectile (already planned) plus Niagara trails/explosions; free Niagara packs exist on Fab and in Epic's content examples | Medium (art) |
| Explosion physics | Sim works out damage and who flies; Unreal Chaos adds cosmetic debris, ragdoll impulses and fracturing walls | Medium |
| Dodge roll animation | Mixamo's free combat rolls (Adobe account, free for games), retargeted with Unreal's IK Retargeter; or free/cheap rolls on Fab. Elden Ring's feel comes from timing, not the animation | Small to medium |

## Pitfalls
1. **Determinism.** Only the sim's recoil, spread and explosions count. Unreal physics must stay
   cosmetic: debris and ragdolls never decide who is hit or hurt.
2. **Dodge roll is a sim feature.** The roll must be a sim command with a start-up, a short window
   where hits miss, and a recovery, moving the pawn through the sim. If the animation moved the body
   (root motion) the sim and the picture would disagree. The animation plays on top, stretched to fit the
   sim's timing.
3. **Motion matching.** A roll is a one-off action, so play it as a montage over the motion-matched
   body, not as database entries; blend back to walking afterwards. Test in first and third person.
4. **Imported animations** differ in skeleton and scale. Retarget once, check feet and hands, and
   keep copies in the asset manifest (`assets/manifest.json`).
5. **Camera shake and motion blur** make some players sick: add sliders in settings.
6. **Recoil for AI fighters** is spread from Shooting skill (F5); they don't need the pattern tables.

## Suggested work (not scheduled)
One cloud task (recoil kicks, dodge-roll command and sim tests, after F2), one PC task (shake, blur,
roll animation and effects, after U11).

## Built (P2)

Code: `ColonySim/Combat.h` (`RecoilKick`, `DodgeRoll`, `CombatSystem::dodgeRoll`, `kickOf`),
`Private/Combat.cpp`, `DodgeRollCommand` in `Commands.h`. Tests: `Tests/ShooterFeelTests.cpp`. Data: `recoilKicks` on each gun
in `Things/Weapons.json`, the kick and `dodgeRoll` numbers in `Rules/CombatRules`. All numbers are ours (RimWorld has neither).

### Gun kick

A gun's `recoilKicks` is a list of `{up, side}` in degrees, one per shot of a spray (`side` positive is to the
right); a shot past the end of the list repeats the last. It only matters for the player's shots (`ShootCommand` with
`player`): fighters' aim comes from skill (F5) and ignores it. Each player shot flies where the shots before it have
lifted the aim (added on top of the player's aim point and the spread, before the drop is aimed for), then the shot's
own entry is added for the next one, less 2.5% a Shooting level like the spread recoil. The kick **holds** for
`recoilKickHoldSteps` (5) after a shot, then **wears off** straight toward the aim by `recoilKickRecoveryPerStep` (0.2
degrees a step); the spray restarts at the first entry after `recoilKickResetSteps` (15) without a shot; the kick
never passes `recoilKickMaxDegrees` (10). So a single shot from the revolver or bolt rifle has fully worn off before the
next can be fired (the kick is then a camera effect only), while the assault rifle's burst of three climbs about 0.7,
1.5 and 2.4 degrees and the next burst starts again. Same inputs give the same shots: the kick is plain arithmetic, no
trigonometry.

| Gun | First kick up, side (degrees) |
| --- | --- |
| revolver | 2.8, 0.3 |
| autopistol | 1.2, 0.1 |
| bolt-action rifle | 3.0, 0.2 |
| pump shotgun | 4.0, 0.3 |
| assault rifle | 0.7, 0.05; the list has six entries, 0.7 to 0.9 up, the side kick swinging right and left in turn |

For the camera: the snapshot has `kickUp` and `kickSide` (degrees, the offset now). Unreal shows the same offset as a
visual-only lift of the view and still sends the point the player aims at **without** it, so the crosshair on screen
is where the bullet goes and the player pulls the mouse down to cancel the kick.

### Dodge roll

`DodgeRollCommand{pawn, dx, dy}` for a colonist under direct control. Steps (one step is a game second, 30 a real
second):

| Phase | Steps | What happens |
| --- | --- | --- |
| Start-up | 3 | stands where it is, facing the roll, can be hit |
| Rolling | 15 | moves 4 m (0.27 m a step, sliding along walls and stopping at the map edge); **bullets pass through the pawn for the first 12 steps** |
| Recovery | 9 | stands still, can be hit |
| Wait | 6 | the next roll is refused until it is over |

That is 27 steps (0.9 s) from the key to being free, and a pawn can be shot for the 12 steps around the hit-proof
window. The direction is any length; zero or not a number is a backstep away from where it faces. A roll is refused for
a pawn that isn't under direct control, is downed, is prone, or is rolling or waiting. The roll cancels a shot being
aimed or fired (not a reload), and `Shoot` and `Reload` are refused while it lasts. A pawn knocked down mid-roll ends
the roll where it lies. **The sim moves the pawn:** `DirectMoveCommand` is ignored (no failure) from the command to the
end of recovery, so Unreal must follow the snapshot's position then and not keep its own. Hit-proof is a projectile
test (`CombatSystem::trace` skips the pawn); other damage that arrives later (fire, melee, blasts) should check
`CombatSystem::hitProof(pawn)` too.

The snapshot has `rollPhase` (0 none, 1 start-up, 2 rolling, 3 recovery), `rollStepsLeft`, `rollDirX/Y` and `hitProof`;
`PawnAction::Roll` goes out when it starts with the cell, the direction and the total steps (27), so the animation can
be stretched to fit the sim's timing and played as a montage over the motion-matched body (pitfall 3 above).

### Not done (U21 and later)

The key and the controls (a roll key in direct mode that sends the direction of the movement keys), the roll animation,
the camera kick from the snapshot, shake, blur and the aim-hold camera blend. Stamina for rolls (none: the wait is the
limit), stance effects on the kick (crouching and prone could steady it), recoil kicks for a held full-auto gun with
its own list longer than a burst (no such gun yet), and the talent tree's "steadier aim" (P3) as a factor on the kick.
