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

## Strand hair (step 1, 2026-11-20)

sneed (2026-11-20) chose real strand hair over the wig meshes ("try a makehuman with strandgroom ... can you make it look
good / real?"). A groom is tens of thousands of hair curves rooted in the scalp, drawn as strands with hair shading and
swung by Unreal's groom physics. The wigs stay as they are (the default) until strands are proven.

- **Step 1 (this PR):** `StrandHairAttach` (`StrandHair.h`) puts a groom made for the MetaHuman head (the PawnFaces copies,
  `GA_Hair_*`) on any body with a `head` bone: the groom's curves are moved from the MetaHuman face mesh's component space
  onto this body's head (head bone onto head bone, scaled by the two necks' neck_01-to-head lengths, within 15%) and ride
  on the head bone; groom physics collides with the body through the mannequin physics asset (same bone names). Creator
  switch `-CreatorGroom=GA_Hair_Long` shows it on the MakeHuman bodies instead of the hair meshes; tuning cvars
  `Pawns.StrandHairScale`, `Pawns.StrandHairOffsetX/Y/Z`, `Pawns.StrandHairYaw`; the log line `STRANDHAIR` gives the fit.
- **Next:** our own grooms, the pawn's hair colour and strands only near the camera: step 2 below.
- **One-time per style:** each groom is made and imported once; fitting to each head is automatic. Cost measured on the
  PC (2026-11-20): one MetaHuman close-up with groom physics about 9-13 ms GPU, about the same as the creator scene with
  four card-hair bodies.

## Growing our own grooms (step 2, 2026-11-20)

`tools/makehuman/grow_hair.py` (headless Blender, the styles in `tools/makehuman/hair_styles.json`) grows strand hair
on a creator base's own body, so it fits that scalp exactly and can be grown again whenever the base changes:

```
blender.exe -b -noaudio --python tools/makehuman/grow_hair.py -- game/Saved/MakeHuman/CreatorFemale/CreatorFemale.fbx game/Saved/Grooms/CreatorFemale --preview
```

- **Roots** are scattered evenly over the scalp, inside a hairline round the skull (a sphere fitted to the crown): high
  at the forehead, lower at the temples, above and behind the ears, lowest at the nape.
- **Combed with gravity.** Each strand leaves the skin along the style's flow (away from a middle or side parting, straight
  back, or towards a ponytail's band) lying along the scalp; with each step it turns further towards the ground. Every
  point is pushed out of the body mesh to the strand's own small distance off the skin, so the hair has layers and
  volume and lies on the head, shoulders and back without passing through. Long hair falls behind the shoulders (some
  locks from in front of the ears fall in front) and may not spread outwards below the neck, so it never runs down the
  arms. A ponytail's strands are combed to the band at the back of the head, then hang from it, spreading a little and
  narrowing to the ends.
- **Then** strands are pulled into clumps towards their tips, waved (a whole clump together), given a little frizz, cut
  to length (`to`: waist, mid back, shoulder, chin, ear, with a soft U at the back) and checked against the body again.
- **Styles:** LongStraight (waist, middle parting), LongWavy (mid back, side parting), Ponytail, Bob, Short, Buzz,
  and (2026-11-21, below) Braid, Afro and Messy; 30,000 to 60,000 strands each. Each is written as `<Style>.abc` (centimetres, relative to the head bone, the body's
  axes) with `grooms.json`; `--preview` renders the head from three sides and the whole length from behind with Cycles.
  Blender also runs as a Python module in the cloud (`pip install bpy`), so styles can be tuned there with test renders
  on MakeHuman's base mesh (`--joints` with its head and neck joints).
- **Import:** needs the engine plugin AlembicHairImporter (on in `Rim3D.uproject`; off by default, and without it the groom importer reads no Alembic at all). The `HairGrooms` commandlet (`Rim3DEditor/HairGroomsCommandlet.*`) imports every style of every base in
  `assets/hair_grooms.json` as `/Game/Rim3D/Generated/Grooms/GA_<base>_<Style>`: Unreal's default Alembic conversion
  (gives the body's own axes), guides made from the strands for the physics (one in 40), the physics on, the MetaHuman
  hair material, and a second LOD with a third of the strands for further off.
- **Hair colour:** the pawn's hair colour drives the groom through the MetaHuman hair material's melanin and redness
  (`StrandHairSetColour`, the same mapping as the MetaHuman route's grooms).

## Fit to each head (2026-11-21)

sneed (2026-11-21), on the MetaHuman groom test: "way too big ... the hairline's too high ... looks like it was made to
fit a head 35% bigger". That groom was sized by the two skeletons' neck-to-head lengths, and our bodies have longer necks
but smaller heads than the MetaHumans, so it was blown up 15% instead of shrunk. Now:

- **MetaHuman grooms** (`-CreatorGroom`) are worn at a set 0.85 of their size (`Pawns.StrandHairMetaHumanFit`).
- **Our grown grooms** fit the base's own head exactly (grown on it). Height scales the whole body and the hair with it.
  The creator's sliders are morphs, which move no bones, so `grow_hair.py` measures each one: it fits the skull sphere
  again with that morph on and writes how much the skull grows and where its centre moves (`grooms.json`
  `head_morphs`, Unreal axes, cm). The game adds these up by each person's morph weights and grows or shrinks the hair
  about the skull centre to match, so a wider, longer or smaller head gets matching hair. The log line `STRANDHAIR`
  shows the head-slider scale and move.
- The grown styles' hairline is a little lower (forehead 22 degrees above the skull centre, was 27).

## Braid, afro and messy (2026-11-21)

The last three creator styles that still wore their wig meshes up close are grown too (`hair_styles.json`, new steps in
`grow_hair.py`), so every creator style now has strands:

- **Braid:** combed back flat to a band low at the nape (found on the head's own back surface), each strand to the part
  of the band on its own side of the head, then plaited: three round bundles weaving over the middle in turn (x =
  sin a, y = sin 2a / 2, a third of a turn apart; `braid` width 4.2 cm narrowing to 2.4 cm, a full turn every 11 cm)
  down the back to the waist, its middle line kept off the shoulder blades, tied 7 cm from the end with a small loose
  tuft below. Strands still wandering after 40 cm join the plait, so none hangs loose beside it. 30,000 strands on 200
  points (0.6 cm apart, so the weave stays round). Physics on, like the ponytail.
- **Afro:** each strand grows straight out from the head (sagging a little) until it reaches a rounded shell 6.8 cm out
  from the skull, so the outline is round; then wound into its own tight coil (2.8 mm across, about 1.1 cm a turn, each
  strand its own phase and pitch) and loosely clumped. 45,000 strands, 1.8 mm segments. No physics (`hair_grooms.json`
  "still": it holds its own shape, and Unreal's groom physics would let the coils sag). Under a hat the strands above
  the brim are combed down along the scalp, with no coils, and puff out below it behind the ears; in front of the
  ears they end at the brim, so nothing puffs over the cheeks.
- **Messy:** a 9 cm cut (sides shorter, lengths vary 15%) whose flow is a smooth random field over the head about 8 cm
  across (`messy`, `messy_size`), so each patch of hair goes its own way in locks rather than fuzz; it lies only partly
  flat on the scalp (`hug` 0.8) and leaves the skin at a steeper angle (`rise`), strongly clumped, with a little frizz.
  Physics on, like Short.

Cloud previews (MakeHuman's base mesh, `--preview`): `/mnt/project-files/hair-grown/braid_afro_messy.png` and
`..._hat.png`.

## Hats and helmets (2026-11-21)

Every grown style (but Buzz) is also grown as `<Style>Hat`: everything above the hat's brim (`hair_styles.json` "hat",
1.2 cm below the skull centre's height) is pressed within 4 mm of the scalp, and below the brim it hangs as usual, so a
hat or helmet sits over the hair with no strands through it while the hair below shows and swings. `Want(...,
bUnderHat)` picks it; headgear that covers all the hair still hides it all (`apparel.hidesHair`). The groom physics keeps
colliding with the body (below); when MakeHuman bodies wear hats (clothes thread), the hat also gets a collision sphere
so swinging hair below the brim cannot pass into it. Creator test: `-CreatorHairHat` (pressed hair, no hat drawn).

## What the strands collide with (2026-11-21)

First PC pictures of the grown hair: with Unreal's groom physics off it hung exactly as grown, with it on every style
puffed out into a ball (long hair spread from the crown to the waist, Short and Crop stood straight up, the hat
versions lifted). The cause: the strands collided with the body's own physics asset, whose shapes wrap each bone's
vertices and stand centimetres proud of the scalp and back, and our hair is grown lying within millimetres of the
skin, so it started inside them and was pushed out (the body was also added as a collision component, which brings in
the same shapes, so the mannequin asset had the same trouble).

Now `grow_hair.py` fits collision spheres inside the skin (`grooms.json` "collision", about 40): 12 filling the cranium,
the neck, and every 5.5 cm from the top of the shoulders to the waist one in the middle of the body and two towards
each side (two more at the shoulders), each at the middle of the body's depth with the largest radius that stays 4 mm
inside the surface. The game builds a physics asset from them once per body mesh (head spheres on `head`, the neck on
`neck_01`, the rest on the nearest spine bone) and the groom collides with that alone. Hair at rest never touches them;
hair swinging into the body is pushed back out at the skin. Buzz has no physics at all (`hair_grooms.json` "still").
`Pawns.StrandHairCollision` 1 fitted (default), 0 nothing, 2 the body's own asset (to compare). Head sliders do not
resize the spheres (the 4 mm keeps a slightly smaller head clear).

## Near and far (step 2)

`UStrandHairSubsystem` decides who wears strands. Every MakeHuman person the creator's apply dresses (the creator, the gun
range's random people) asks for strands for their hair style (`assets/hair_grooms.json` "creator" maps the creator's
style names to grown ones; every creator style now has one). Four times a second the people within
`Pawns.StrandHairNear` metres of the camera (10) are sorted by distance and the nearest `Pawns.StrandHairMax` (6) wear
strands, their wig hidden; the rest wear the wig. People wearing strands keep them a metre further, so nobody flickers at
the edge. Strand hair is the default (`Pawns.StrandHair 1`) with the wig as the fallback: far off, past the count, for a
style with no grown groom, or with `Pawns.StrandHair 0` (wigs for everyone). The colony's pawns are still MetaHumans
(their own MetaHuman grooms); they get this when they move to MakeHuman bodies. A style whose groom has not been grown and imported keeps its wig.

## Animal fur and manes (plan)

The same script can grow on any mesh: what changes per animal is where hair grows and how long it is. Plan, in order:

1. **Manes and tails first** (horse, thrumbo, lion-like predators, muffalo's shag): a `region` per animal style picked by
   painting, as a vertex group in Blender, or by bone weights (neck and head bones for a mane, tail bones for a tail),
   grown with the same comb-and-gravity steps, combed along the bone chain instead of from a parting. A few thousand
   strands each, so they cost about a quarter of a human's long hair.
2. **Attached to the animal's skeleton**, not a head bone: Unreal grooms on animals need a groom binding to the skeletal
   mesh (made by the commandlet, as PawnFaces does for the MetaHuman faces), so the roots follow the skin as the animal
   moves. Animals play single clips with no animation graph; groom physics runs on its own, so that is no obstacle.
3. **Short body fur** (dogs, cats, wolves) as strands only on the nearest few animals and only for the furriest kinds;
   further off, and for the rest, the textured mesh as now. Full-body fur is 100,000+ short strands, so it is a
   per-animal choice, not a default.
4. Same near/far rule as people (`UStrandHairSubsystem` with its own count), and the animal's own colour drives the fur.

Waiting on: animal models with clean UVs and skin weights (the free animals vary; some manes are solid mesh that would
need hiding where the strands grow).

## Cost and distance

About 30 points against 10 capsules: a few tens of microseconds per pawn per frame, on the animation worker threads.

| Distance from the camera | Hair |
|---|---|
| up to 25 m (`Pawns.HairSimFar`) | full simulation |
| 25 to 50 m (`Pawns.HairCollideFar`) | as animated, but pushed out of the body (no swinging) |
| beyond 50 m | as animated |

## Console and checks

- Strand hair: `Pawns.StrandHair`, `Pawns.StrandHairNear`, `Pawns.StrandHairMax`, `Pawns.StrandHairCollision`,
  `Pawns.StrandHairMetaHumanFit`, `Pawns.StrandHairScale`; Unreal's `r.HairStrands.Simulation 0` turns groom physics off.

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
- Hits, blasts and wind don't push the hair yet. Clothes are handled for strand hair (below); the wig node's capsules still ignore them.
- The bust bar is sized from the skeleton and the back hair, not from the mesh, so a very large or very flat chest is
  only roughly matched; reading the body mesh needs CPU copies of the meshes (not kept in packaged builds).
- Five chains carry the whole sheet of hair; the mesh between two chains is blended between them, so where two chains
  part round a shoulder the hair between can cut the corner. More chains need the MakeHuman hair rebuilt
  (`add_hair_bones` azimuths) and re-imported on the PC.
- Animals: the solver and the body shapes take any chain of points and any capsules, but none of the animal models has
  bones in its mane, fur or tail to swing yet (their manes are solid mesh) and animals play single clips with no
  animation graph; a model with mane or tail bones would get the same node.

## Body collision and the parting (sneed, 2026-11-22)

Long hair went through the chest, back and arms: the groom only collided with head, neck and a few torso spheres.
`grow_hair.py` now lays torso slices with spheres at the front and back (30% and 70% of depth, plus left and right of
centre at the front) and spheres along each upper arm (4) and forearm (3), each bound to its own bone in
`StrandHair.cpp`. The spheres stay 4 mm inside the skin, as before, so strands rest on the body instead of fighting it
(the wig's jitter came from shapes outside the skin). At most 90 spheres (Unreal reads about 100; the smallest torso
ones go first). A middle or side parting is now soft: each strand's side is picked from its distance to the line plus
noise (`parting_cross` 0.6, `parting_width` 1.4 cm), so strands cross over and the scalp line no longer shows.

## A shell with no gaps, grown out to the clothes (sneed, 2026-11-22)

After the parting and arm spheres, long strand hair still went into the back and through the shirt (gun range, aiming
the bolt-action rifle). Measured on the CreatorFemale body and its garments (cloud, `bpy`):

- **Gaps.** The spheres sat in a few columns, so between them a strand could sink 2 cm into the back (median), up to 5 cm,
  before anything stopped it; 1.4 cm on the chest and shoulders.
- **Clothes.** The garments sit 0.5 to 1 cm off the skin and the material pushes tops out 1.5 cm more (0.6 a layer), so
  hair grown 0.15 to 1.15 cm off the skin rests 1.5 to 3 cm inside the shirt, and the physics never saw the shirt.

**The shell.** `grow_hair.py` (`collision_shapes`) now picks the shapes instead of laying them in a grid: candidates are
spheres and capsules 1.5 to 5 cm under the skin (along its normal), each as big as stays 4 mm inside it; the one that
most cuts how deep hair could sink into the skin is taken, again and again, until 90 shapes in all (12 spheres in the
cranium, then about 40 capsules over the torso, 7 on the neck, 2 on each shoulder top, 13 on each arm). The depth is
measured at the skin's vertices from the waist to under the skull, the back and the shoulder tops counting double. Every
shape belongs to the skin of one bone (its vertices' largest skin weight: torso, neck, a clavicle, an upper arm, a
forearm), moves with that bone, and only counts for that skin. Left and right are mirror images. Hair can now sink 0.7 cm
into the back (median; 1.2 cm for 99 in 100 points, 1.8 worst), the chest, shoulders and arms the same, against 2 and 5
cm before; the 4 mm inside the skin is the floor, so resting hair still never starts inside a shape (no jitter). Eight more capsules fill the core (three columns down the middle of the torso's depth and one along the neck): the shell
alone was hollow, and in one PC walk the long hair, once past it, stayed sunk in the bare back; now 95% of the torso's
inside is held by a shape (89% without the core), so hair that gets in is pushed back out. Each
shape also keeps the skin's outward normal (`out` in `grooms.json`); capsules have `a_cm` and `b_cm`.

**Clothes.** `StrandHair.cpp` builds the physics asset per body mesh *and outfit*: for each shape (not the head's) and
each worn MakeHuman garment (`SK_MHG_*` following the body), if at least 3 of the garment's vertices lie within 6 cm of
the shape and within 50 degrees of its `out`, the shape grows to the garment's nearest vertex there (never past its
nearest vertex anywhere round the shape), plus the garment's puff, plus `Pawns.StrandHairClothGap` (0.3 cm: the drawn
strands follow the colliding guide strands give or take half a centimetre); the outermost covering garment wins, at
most 6 cm more. A short sleeve leaves the forearm's shapes alone. Measured with the blouse, tee, knit sweater and winter
coat: hair could sink 0.4 to 1 cm below the cloth's surface (median), against 2.5 to 4 cm with the shapes left inside the
skin. The outfit is checked four times a second, so clothes put on or off remake it (one log line per new asset:
`STRANDHAIR collision for ...: N shapes ..., K shapes grown out to the clothes by G cm`). The garment vertices come from
the mesh's CPU copy (kept in the editor build the game runs in; a mesh without one is logged and left out).

Console: `Pawns.StrandHairClothes 0` leaves the shapes inside the skin (to compare), `Pawns.StrandHairClothGap` (cm),
`Pawns.StrandHairDebug 1` draws the shapes on the posed body (green inside the skin, yellow grown to the clothes).

**PC check:** the creator's `-CreatorStrandDemo` (with `-CreatorFemale -CreatorHair=Long`, and `-CreatorDressed
-CreatorSelect=2` for a T-shirt under a jacket): close from behind and from the side, standing, aiming a rifle, swinging
a sword, shadowboxing, walking, then aiming with the shapes drawn; pictures `Saved/Smoke/creator/strand_NN_*.png`.
Needs the `hair-grow` art step again (new `grooms.json`; `update-and-play.bat` runs it). The grooms simulate with 10
sub-steps and 10 iterations (Unreal's default 5 and 5 let bare-skinned long hair pass the shell as the body turned on
the spot mid-walk; `HairGroomsCommandlet`). Right after a quick 180-degree turn on the spot the bare-skinned long hair dipped into the back or whipped round the
side for a moment (PC strandwalk frames), then lay back on the back. Simulating it in the chest's frame
(`Pawns.StrandHairTurnFollow`, Unreal's local simulation) was tried at 0.25: the swings went stiff and the hair stayed
sunk after turns, so it is off by default. Turning the body at half the speed (360 degrees a second, PR #450 round 1)
made no difference. So now, **while a body turns faster than 240 degrees a second** (`Pawns.StrandHairTurnHold`; a
turn round, a swing at someone; a walk's gentle curves stay under it) its hair is held at its resting shape on the
head, turning with the body, and it swings freely again from there 0.08 s after the turn eases
(`Pawns.StrandHairTurnHoldAfter`; the groom's simulation is reset each held frame). Log `STRANDHAIR turn hold N`. Dressed bodies are clear (the
shapes stand 3 cm further out). `-CreatorRecord=1` with the demo also saves every other frame of the walk
(`strandwalk_NNNN.png`). `-CreatorStrandWalkPitch=-70` holds the walk's view still, from one side and looking down
at that pitch, so a turn shows whether the hair goes round the body or through it.
