# Pawns on screen: actors and animation by distance

Code: `game/Source/Rim3D/PawnSubsystem.*`, `PawnActor.*`, `PawnAnimInstance.*`. Smoke test: `pawns`
(`tools/smoke.ps1 -Test pawns`). Task U5. Sim side: the `PawnAction` event (`ColonySim/Events.h`).

The sim moves pawns; Unreal only draws and animates them (`architecture.md`, "Pawn animation and
looks"). Stage 0 measured 0.2 ms of game thread per mannequin with CharacterMovement and motion
matching; this design drops the movement component and spends motion matching only where the
camera can see the difference.

## Actors

`USimPawnSubsystem` (one per game world) reads the snapshot every frame. Each pawn gets one
`ASimPawnActor`, spawned the frame the pawn appears and destroyed when it leaves the snapshot (or
when a new sim starts, since entity numbers are reused). The actor has a root, a skeletal mesh
(the sample's UEFN mannequin) and nothing else: no movement component, no collision, no tick of
its own.

Each frame the subsystem calls `UpdateFromSim`:

- **Position**: `USimSubsystem::GetBlendedLocation`, the same blend between the last two
  snapshots the rest of Unreal uses. The smoke test finds every actor within 16 cm of its sim cell
  position (one step of walking at 1x is 15 cm).
- **Facing**: turns toward the blended sim velocity at 720 degrees a second (times the game speed);
  a pawn slower than 0.3 m/s keeps its facing. The sim has no facing of its own yet.
- **Animation speed**: the mesh's `GlobalAnimRateScale` is the game speed, so animations play 2x
  and 3x with the game and freeze while paused. Everything the animation sees is in "1x seconds":
  the velocity is the sim's metres per step times 30.

## Detail by distance

Pawns are sorted by distance to the camera every frame:

| Detail | Who | Animation | Mesh updates | Strand hair |
| --- | --- | --- | --- | --- |
| Full | the 20 nearest (`Pawns.FullDetail`) | motion matching | every frame | yes |
| Near | the rest within 40 m (`Pawns.NearDistance`) | loops | 20 a second | no |
| Far | everyone else | loops | 8 a second | no |

The slower updates are the mesh component's tick interval, so animation simply runs less often
far away. Lower-detail bodies come from the meshes' own LODs (picked by screen size).
`Pawns.ShowDetail 1` puts a green, yellow or red dot over each head.

## The animation, all in C++

`USimPawnAnimInstance` is a native anim instance with no Animation Blueprint. Its proxy supplies a
two-node graph through `GetCustomRootNode`:

1. **Slot** (`FSimPawnSlotNode`): the montage slot for actions. It does what Epic's slot node
   does; Epic's own node can't be used here because its Sequencer hook asks the anim class for
   anim subsystems, which only Blueprint anim classes have (it asserts).
2. **Locomotion** (`FSimPawnLocomotionNode`): Epic's blend stack (`FAnimNode_BlendStack_Standalone`,
   0.25 s blends) fed either by motion matching or by loops.

Root motion is extracted and dropped (`ERootMotionMode::IgnoreRootMotion`): animations play in
place and the sim moves the actor.

**Full detail: motion matching.** The same steps as Epic's motion matching node
(`FAnimNode_MotionMatching`), done by hand:

- **Trajectory**: 10 past samples 0.04 s apart from where the pawn really was, and 15 future
  samples 0.1 s apart (1.5 s) predicted along the sim's route: the snapshot's waypoints ahead
  (up to 8), walked at the pawn's def speed (`stats.moveSpeed`), speeding up and braking by 20% of
  that per step like the sim, braking into the last waypoint, facing the way each leg goes. With
  no route, the pawn brakes where it is heading. Facing is the mesh's (actor yaw minus 90 degrees).
- **Pose history**: `UE::PoseSearch::FPoseHistory`, 10 poses 0.04 s apart, filled after each
  evaluation.
- **Search**: `UPoseSearchLibrary::MotionMatch` over ten of the sample's dense standing
  databases (idles, turn in place, walk and run loops, starts, stops and pivots), continuing the
  current animation when it still fits. A new result blends in through the blend stack with the
  play rate the search wants (0.8 to 1.2).

**Standing still: standing idles with planted feet** (sneed 2026-11-17, again 2026-11-18: "0 animation resets").
A pawn that has stood for 0.3 s (under 30 cm/s, no route longer than 25 cm ahead), or has been still for 0.6 s
whatever its route says, leaves motion matching and plays one of the **standing idles**, picked at random each time it
stops and blended in over 0.4 s from a random point in the clip (so a crowd does not breathe in step). The idles are
Mixamo clips retargeted by `-run=MakeHumanArt -Clips`, each measured from its FBX so that neither foot moves more than
0.3 cm (`USimPawnSubsystem` `PawnStandIdles`):

| Idle | Footprint |
|---|---|
| `Mixamo/MeleeAxe/A_ProMeleeAxeUnarmedIdle` (calm breathing, 1.9 s) | 0 |
| `Mixamo/MeleeAxe/A_ProMeleeAxeUnarmedIdleLookingVer1` (looks round, 11 s) | 0 |
| `Mixamo/MeleeAxe/A_ProMeleeAxeUnarmedIdleLookingVer2` (looks round, 7.8 s) | 0 |
| `Mixamo/Longbow/A_ProLongbowUnarmedIdle01` (relaxed, 4.9 s) | 1 |

A pawn that keeps standing **drifts to another idle** every 14 to 40 s over a slow 1.2 s blend (any of them: the feet
are held where they stand, so only the body above them moves; clips with the same footprint stand on the same spots to
the millimetre).
Left out after measuring: Action Adventure `idle (2)` (one foot 54 cm ahead of the other), `idle (3)` (steps with the
right foot), the fighting crouches, and the bow, magic and examine idles with raised arms.

Why the shuffle survived #327: the sample's `PSD_Dense_Stand_Idles` and `PSD_Dense_Stand_TurnInPlace` databases were
still searched, and motion matching reached for them whenever a pawn was *nearly* still (turning to face a target,
locked on, between swings, the moment after stopping): those clips step a foot forward and shuffle. They are no longer
loaded; standing is always the standing idles. The **starts and pivots** databases are gone too (2026-11-19, from the
first `idle` test run): their "reface" clips turn the body by their own root, which is dropped here (the actor's yaw
follows the sim), so setting off or turning back spun the legs round under the body. Setting off blends from the
standing idle into the walk, run or sprint loop; stops stay. Motion matching keeps up to 12 blends (the default 4
dropped one half shown: a pop), jumps at most every 0.15 s while the clip playing has time left, and may play a clip
at 0.6 to 1.6 times its speed so planted feet keep up with the real pace.

**Planted feet.** While the standing idle plays, each foot is held where it was when the pawn stopped and the leg is
bent to reach it (`FSimPawnHitLayerNode`, last in the graph), so the blend from the walk's stop into an idle that
stands a little differently, a drift between idles and a standing turn move the body over the feet and never slide
them. The hold lets go over 0.15 s when the pawn walks off, and if the body creeps more than 25 cm from its spot.
**Standing turns** twist instead of shuffling: the hips turn back by the twist (legs and feet stay facing where they
stood) and spine_01, spine_02, spine_04 and the head turn forward by 35/25/20/20% of it, so chest and head face the new
way. Past 40 degrees the pawn takes **two small steps**, one foot then the other (0.3 s each, lifted 5 cm), to where
its idle wants the feet for the new facing; the twist never passes 60 degrees. A foot more than 22 cm from where the
idle wants it (after a drift) steps the same way. Leaving, the twist and the hold fade out over 0.2 s. Off while a
whole-body clip, a roll, a jump or a crawl has the body. `GetStats()` also reports `FootHold`, `bFootStepping` and
`FootSteps`. `GetStats()` reports `bStandingIdle`,
`IdlesStarted`, `IdleDrifts` and `StandTwistDegrees`.

**No pops** (sneed 2026-11-18). Every switch is a blend from the pose the body is in, never a restart: a hit react
plays on the upper-body slot (0.12 s in, 0.3 s out) over legs that keep walking or standing, is not played over a clip
the sim is moving the body along, and is not restarted by every bullet of a burst (0.35 s apart at least; the hit
layer's wobble shows the rest). A clip the sim moves the body along blends in over 0.1 s, or 0.16 s when it takes over
from one still playing (the next swing), and the clip going out keeps its share of the set-back while it fades (the
body used to jump forward by the old clip's travel the moment a second swing began). Smoke test `idle` measures it.

Second PC round (2026-11-19): **pawn meshes animate in `TG_PostUpdateWork`** (a ragdoll in `TG_PrePhysics`), after the
pawn subsystem moves and turns the actors as a tickable; animated in `TG_PrePhysics` they drew a pose made for last
frame's place, so every turn swung the held feet by a frame's turn and back (a foot pop at every turn's start and end).
Motion matching **never plays a turn, pivot or reface clip** (`Stand_Turn_*` sat in the loaded databases and were picked
at every change of direction, sliding the feet up to 24 m/s), **nor a run or sprint clip at a walking pace**; a banned
pick keeps the clip playing or falls back to the plain walk or run loop. A standing idle **never wraps on screen**: 0.5 s
before its end it blends into itself from the start (the 11 s looking idle popped the head at each loop). **Walking feet
are locked** while the animation has them on the ground (within 3 cm of their lowest lately): held where they came down,
the leg bent to reach, let go as the animation lifts them with the gap closing over 0.15 s, dragged (never snapped) past
20 cm; the lock fades in and out over 0.15 s and is off for whole-body clips, jumps, rolls and crawling. The smoke tests
read the bones at the start of the next frame (what was drawn), and `idle` fails on any frame that plays a turn clip.

Third PC round (2026-11-19): nothing in the walking lock changes a foot's speed at once (a foot lands with the speed it
came down at, dying away over 0.025 s; a hold more than 8 cm behind the animation's foot follows it at 22 times the
excess a second, 50 cm at most; a lifted foot lets go along a curve that starts at its own speed). The standing hold
and the walking lock fade in and out eased, not linearly. **The hips stay within 8 cm of the sim's position** under
walking legs (the sample's stop and transition clips lead or trail their root by up to 20 cm: staggers, and the walk
back after a roll or a knockdown, drew the hips 17 to 22 cm off). Neither the feet holds nor the hips' pull act while
a whole-body clip still shows (blending out after the sim's clip has ended, a crouching action): the clip's own legs
and hips are drawn then (pulling the hips while a roll blended out left them 2 m from the body). **The facing drawn
follows the sim's** as a damped spring (20/s, at most 30 degrees behind), so a body the sim turns 30 degrees in a few
frames starts and stops turning smoothly. The `idle` test measures a walking foot the lock holds in the world, like
a standing one, and stamps each sample with the time of the frame it shows.

Fourth and fifth PC rounds (2026-11-19): **pawns move by this frame's time.** The pawn subsystem and the sim bridge are
both tickables, whose order is not set; on frames the pawns ticked first they were drawn with the last frame's blend,
so a walking body moved by the last frame's time (its speed wavered 20% frame to frame, and every foot held in the
world seemed to jump against it). `USimSubsystem::AdvanceFrame` now runs the frame's steps once, for whichever asks
first. **The hips' pull works on the locomotion's own pose**, under the montage slot (in the hold layer): a roll or a
sidestep cross-fades from the pulled walk to its own hips, and its set-back (the clip's travel times its weight, read
a frame on, where it is drawn) is never fought by the pull (it pulled the hips 17 cm back as a roll blended in).
**A planted foot is held by its heel or its ball**: down while either is within 3 cm of its lowest; on the ball alone
(pushing off) the ball stays put and the ankle swings over it. **It keeps its facing** (ankle to ball) up to 35 degrees
off the animation's while walking, and a standing foot up to 40 degrees (steps turn it with them), so neither the body
turning over a foot nor an idle swivelling it spins the toes round the ankle; a let-go foot turns back along the same
curve as its place. **The play rate eases** (0.2 s) to the one motion matching wants instead of jumping at each search
(every bone's speed scaled at once), and the hit lean (the whole body tipped along a shot) eases in and out and eases
out of a lean still showing when the next hit comes (it used to jump back upright).
Sixth round: the hips' pull **eases in** (a tanh curve from 4 cm that never passes 8 cm), since a hard stop at 8 cm
stopped the hips' outward speed in one frame (a jolt of every bone as a stop clip's hips led out); a foot **lands only
while the animation's foot moves under 1.5 m/s** over the ground (a running foot swinging low past the ground was taken
for a landing, its speed cut at once); a standing foot the leg can hardly reach any more **steps** before the
stretched leg would drag it.

**Near and far: loops.** The breathing idle, or the walk or run loop (`M_Neutral_*`), picked by speed (run above the
middle of the two loops' speeds, measured from their root motion: 200 and 500 cm/s) and played
faster or slower to match it (0.6 to 1.5). A pawn that comes close again starts motion matching
from that loop.

The game thread fills `USimPawnAnimInstance::Inputs` (position, yaw, velocity, route, detail);
the proxy copies them in `PreUpdate`, so the search runs on animation worker threads.
`GetStats()` reports back what plays, the database it came from, the play rate and how many
searches and jumps there were (the smoke test reads these).

## Actions

The sim sends `PawnAction` events (`pawn`, `target`, what was done: `PickUp`, `PutDown`) from the
hauling placeholder; C1's jobs will send theirs (chop, mine, build, eat) and add values to the
enum. `USimPawnSubsystem` plays a dynamic montage in the slot for each:
`GetActionAnimation` maps an action to an animation. Until there are real ones every action is a
short crouch (the sample's crouch idle: 0.25 s in, 0.35 s held, 0.35 s out). The legs keep
walking underneath because locomotion keeps updating.

## MetaHumans

`Pawns.MetaHumans 1` draws every pawn as the sample's MetaHuman (`BP_Kellan`), as in the Stage 0
test: the mannequin is hidden but keeps animating, and the MetaHuman's body copies its pose
(leader pose). Strand hair (`GroomComponent`, found by class name) is shown only at full detail.
Seeded looks (body shapes, skin colour, height, lineage parts such as horns) come from the sim
since A3 ([looks.md](looks.md)). The default is mannequins.

## Cost, measured (smoke test, editor build, offscreen 888x500)

| Phase | fps | Game thread | GPU |
| --- | --- | --- | --- |
| 100 mannequins, overseer view | 102 | 9.1 ms | 1.7 ms |
| 100 mannequins, close up | 105 | 9.3 ms | 1.7 ms |
| 100 MetaHumans, overseer view | 51 | 19.5 ms | 2.7 ms |
| 100 MetaHumans, close up | 50 | 19.2 ms | 3.6 ms |

Stage 0 had 19 ms of game thread for 100 mannequins with CharacterMovement and full motion
matching, and 35 ms for 100 MetaHumans (at 1440p on screen, so only the game thread compares).
The editor binary is slower on the CPU than a shipped game; G1 measures a packaged build at 1440p.
The extra 10 ms for MetaHumans is probably the MetaHuman actor itself (its face, LOD sync and
Blueprint) rather than the animation, which is the same; not profiled yet. Options if G1 needs
it: hide the face mesh far away, or swap far pawns to the mannequin body.

## Smoke test `pawns` (about 70 s)

A 128 m map with 100 hauling pawns and 600 items, at 1x. Four phases: mannequins from high over the
stockpile, mannequins close up at the stockpile, then both again as MetaHumans. Every frame after a
5 s warm-up it checks: every sim pawn has an actor and no actor is left over, no actor has a
movement component, actors stay within 50 cm of the sim, exactly 20 pawns get full detail and none
of them is farther from the camera than a pawn without it, the update rates match the detail,
full-detail pawns are motion matched (90%) and walking ones play walk or run animations (70%),
motion matching picks new animations (at least 20 per phase), simple-detail pawns play the loops
(95%), pick-ups and put-downs start action animations (90%), and for MetaHumans strand hair shows
on every full-detail pawn and on no other. The overseer phase must have pawns at all three detail
levels.

## Not done here

- **Facing from the sim.** Pawns face where they walk. Standing pawns facing a target (a
  workbench, an enemy) needs a facing in the snapshot; add it with the first job that needs it.
- **Foot placement, orientation and stride warping.** The sample's ABP has them; ours doesn't.
  Feet can slide a little when the play rate is far from 1 or on slopes.
- **Real action animations.** The crouch stands in for all of them (A1 or C1's art).
- **Strafing, crouching, aiming, carrying poses.** Need sim state (stance, a carried item) first.
- **The animation budget allocator.** Fixed distance bands are enough for 100 pawns; switch if G1
  shows spikes.
