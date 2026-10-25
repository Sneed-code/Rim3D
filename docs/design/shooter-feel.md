# Shooter feel: recoil, shake, blur, explosions, dodge roll

Research note, 2026-10-25. Nothing here is built. It says what exists, what to use and what to watch for.
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
