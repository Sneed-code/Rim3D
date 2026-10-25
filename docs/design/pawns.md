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

**Near and far: loops.** Idle, walk or run loop (`M_Neutral_*`), picked by speed (run above the
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
Seeded looks (body types, skin tint, lineage parts) are task A3. The default is mannequins.

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
