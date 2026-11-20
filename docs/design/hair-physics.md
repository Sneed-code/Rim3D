# Hair physics

sneed (2026-11-19): "the hair on my female character (long) clips through my body!! why?! how hard is it to have
natural hair that obeys laws of physics?" Long hair must hang, swing and rest on the body, on every pawn and every
hair style, without costing much when many pawns are on screen.

## Why it clipped

Long hair on MakeHuman bodies moves on bone chains (`mh_common.add_hair_bones`: five chains, `hair_c1_1` to
`hair_c5_4`, down the back, the back corners and the sides; a ponytail has its own `ponytail1...` chain). Until now
the game swung those chains with a home-made spring that knew only where the head was: it bent the hair towards the
ground and swung it against the head's starts and stops. Nothing in it knew where the back, shoulders, chest or arms
were. Whenever the body moved into the hair (turning the head, hunching to aim, a shoulder rolling forward in a swing,
an arm coming up, bending forward) the hair went straight through. (An earlier AnimDynamics version had collision
spheres but flung the hair into a sheet now and then, so it was replaced, and the collision went with it.)

## What it does now

Cosmetic only, never sim state. The solver is plain C++ (`ColonySim/HairStrands.h`, so it builds and is tested in the
cloud); `FMakeHumanHairNode` in `MakeHumanAnimInstance` feeds it and turns its answer back into bone rotations.

- **Strands.** Each hair chain is a strand of points (its joints and a tip). The root is pinned to the head. Every
  other point falls under gravity (980 cm/s²), keeps its length exactly, and a spring pulls it back towards the hair
  style's own shape: 400/s² at the root (it sags about 2.5 cm) down to 25/s² at the tip, so the ends hang loose and a
  bowed head lets the hair fall forward. Its swing relative to the head dies away (damping 7/s; a sprint start throws a
  waist-length strand's tip back about 25 cm, settling in about a second), so it lags behind starts, stops and turns.
  Each point is moved after the one above it (follow the leader) with the move taken back off the point above's speed,
  so the chain never gains energy. A segment never turns more than 80° from the style's shape. Steps of at most
  1/120 s, three constraint passes each.
- **The body.** Capsules round the skull, the neck, the chest and upper back (across the body at the top spine bone),
  a yoke towards the base of the neck, the middle of the back, the small of the back (lowest spine bone), the front of
  the chest (a bust bar ahead of the spine, sized from the chest shape: hair falling forward over the shoulders lies on
  it when she leans back), the hips (across the pelvis), both shoulders (collarbone to arm) and both upper arms.
  Each rides on its bone, so it moves exactly as the body is drawn. Every hair segment is pushed out of them by turning
  it about its upper end, just to the surface. **Hair that touches the body lands and stays**: the push out is never kept
  as speed away from the body (no bounce), sliding slower than 4 cm/s stops dead (grip) and faster sliding dies away at
  20/s (friction), so hair lying on the back or shoulders comes to rest and stays still. Between frames the body
  and the animated hair are blended step by step, so a quick turn does not jump through.
- **Fitted to each body.** The shapes are sized from the skeleton (its height and shoulder width), then fitted to the
  hair as it was styled in the reference pose, where it lies on the body: a shape is shrunk if the resting hair would
  be inside it, and the ones hair rests on (neck, chest and back, shoulders) are grown to touch it if it lies a little
  off them. So hair at rest stays exactly where the artist (`add_hair_bones`) put it, and any body shape works.
- **One sheet.** Neighbouring strands round the head (never across the face) are kept between 0.55 and 1.35 times
  their styled distance apart, so the sheet of hair does not tear open or bunch up when one strand is pushed.
- **Ponytails** use the same strands (they were an AnimDynamics chain with no collision).

## Why it pulsed (fixed 2026-11-20)

sneed (2026-11-20): "it just seems to me like the hair touches the body and freaks out to pulse off it ... why can't
it just rest normally?" Two mistakes in the first version (#376): the push out of the body was kept as speed (about 70%
of the speed it came in with, so hair was thrown off the body, fell back, was thrown again, for ever, even standing
still), and the follow-the-leader correction passed every push of a point back up to the point above as a kick into
the body. The push could also overshoot the surface by over three times the depth. Measured on long hair styled lying
on a still body: the old solver never settled (points still moving at 260 cm/s after ten seconds); now nothing moves at
all once it settles, and an idle's lean moves the hair only as fast as the body (tests "comes to rest and stays still",
"lands and stays").

## Aiming (2026-11-20)

PC pictures after the weapon hold polish (#402) showed long hair covering the rifle's stock and the front hand in the
over-the-shoulder aim (the camera sits 120 cm behind and 45 cm right of the colonist), and nothing stopped hair from
going through a gun held across the body. Fading the hair near the camera was the other option; a shooter with long
hair really does put it over the other shoulder, so the game does that, through the physics:

- **The gun and the forearms are solid.** While a two-handed gun is held at its place (`HoldDraw` pinned, the
  `hasOff` long guns), `WeaponDrawSubsystem` hands the hair node the gun (`UMakeHumanAnimInstance::SetHeldGun`: the
  gun in the main hand's space, its butt and muzzle, as thick as its model is wide) and the node adds a capsule along
  it and one round each forearm (elbow to wrist, about 4 cm, scaled to the body). Hair rests on them like on the body.
- **Aimed, the hair goes over the other shoulder.** `colony::sweepHairGoals` turns every strand's styled shape towards
  the side away from the stock and a little back, by up to 45 degrees (`Pawns.HairAimSweep`, 0 off) times the aim
  blend: strands growing on the stock's side fully, down the middle 0.7, the far side 0.4, the first segment (on the
  scalp) by half, roots and lengths kept. Only the goal changes, so the hair swings there with its usual lag (about a
  second for waist-length hair), slides over the neck and shoulders instead of through them, and falls back to its
  style when the gun comes down. Every colonist aiming a long gun does it, not just the player.
- Tests (`HairStrandsTests.cpp`): the sweep keeps roots and lengths and turns the stock side more; on the test body
  with a shouldered rifle, seen from the aim camera, the hair lay across the lines of sight to the stock and the front
  hand without the sweep and clears both by more than 4 cm with it, never going into the body, the gun or the arms;
  lowering the gun lets the hair settle back to where it hangs unaimed (within 3 cm; measured 0.01).
- On the PC: `Pawns.HairDebug 1` draws the gun and forearm capsules with the body's. The gun range's hold pictures
  (`hold_<gun>_3p_aimed`) show it on the long-haired protagonist.

## Cost and distance

About 30 points against 10 capsules: a few tens of microseconds per pawn per frame, on the animation worker threads.

| Distance from the camera | Hair |
|---|---|
| up to 25 m (`Pawns.HairSimFar`) | full simulation |
| 25 to 50 m (`Pawns.HairCollideFar`) | as animated, but pushed out of the body (no swinging) |
| beyond 50 m | as animated |

## Console and checks

- `Pawns.HairPhysics 2` (default) full, `1` only pushed out of the body, `0` as animated (stiff).
- `Pawns.HairDebug 1` draws the capsules (green) and the strands (yellow).
- Cloud tests: `Tests/HairStrandsTests.cpp` (hangs, falls, swings and settles, pushed out of a body styled through it,
  a shoulder sweeping into it never passes through, rough motion at any frame rate stays steady, teleports, pausing,
  the sheet, the fitted shapes clear the resting hair with no pop at the start).
- On the PC: the creator's `-CreatorHairDemo` (`-CreatorFemale -CreatorHair=Long -CreatorHairDemo -CreatorRecord=36`):
  the woman seen from behind shadowboxes and swings a sword with physics on, with the hair as animated, then on with
  the capsules drawn; pictures `Saved/Smoke/creator/hair_NN_*.png`, and the notes say how deep the hair went into the
  body against how deep the animated hair alone would have gone.

## Not yet

- MetaHuman strand hair (grooms, `Pawns.MetaHumans 1`, not used by the game's pawns now) has no physics.
- Short styles and the afro have no bones and stay as styled (they don't reach the body).
- Hits, blasts and wind don't push the hair yet; clothes and armour will need their own capsules once bodies wear them.
- A pistol or a one-handed weapon has no capsule, and aiming one does not sweep the hair; melee weapons don't push it.
- The bust bar is sized from the skeleton and the back hair, not from the mesh, so a very large or very flat chest is
  only roughly matched; reading the body mesh needs CPU copies of the meshes (not kept in packaged builds).
- Five chains carry the whole sheet of hair; the mesh between two chains is blended between them, so where two chains
  part round a shoulder the hair between can cut the corner. More chains need the MakeHuman hair rebuilt
  (`add_hair_bones` azimuths) and re-imported on the PC.
- Animals: the solver and the body shapes take any chain of points and any capsules, but none of the animal models has
  bones in its mane, fur or tail to swing yet (their manes are solid mesh) and animals play single clips with no
  animation graph; a model with mane or tail bones would get the same node.
