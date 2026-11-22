# Wall climbing

sneed (2026-11-19): pawns climb sheer walls, cat-strain twice as fast, with an animation at the top. Code: `Map`
(`setWallClimb`, `refreshWallClimb`, `linkAt`), `MovementSystem` (climb speed), `Sim::writeSnapshot` (the drawn path),
`PawnActor.cpp` (`UpdateWallClimb`). Data: `Rules/ClimbRules.json`. Tests: `Tests/ClimbTests.cpp`.

## What can be climbed

The outside face of any **full-height wall** (a blocking building as tall as rock for sight, `kObstacleTallDm`: every
wall) **with an upper floor laid over it**, from a walkable cell beside it **under open air** (no floor over that cell,
so a pawn never climbs through a ceiling). It ends standing on the floor over the wall. Rock faces and walls with
nothing on top can't be climbed: there is nowhere to stand. RimWorld has no climbing, so the numbers are ours.

Each such face is a `LevelLink` with `wall` set, like stairs and ladders, so paths, regions, the controlled colonist's
climb (walk into the wall: `ClimbCommand`) and saves need nothing new. The links follow walls, floors and ground as they
change (`Map::setBuilding`, `setUpperFloor`, `setTerrain`); they are not saved. `Map::linkAt` lets the pathfinder skip
the link scan in cells with no link end.

## Speed

`ClimbRules.wallSpeedFactor` 0.15 of walking speed (half a ladder's 0.3) over `wallLengthM` 3.5 m (3 m up and over the
top): about 5 real seconds for a human at 1x. Pawns path through a wall climb only when it saves more than that.
Cat-strain climb twice as fast (gene `climbSpeed` 2, `Mover::climbFactor`).

## On screen

The snapshot's `climb` (0 to 1) with `wallClimb`, `wallClimbUp` and the facing (towards the wall). Until
`kWallMantleFrom` (0.8) the pawn is drawn against the face, rising to 1.1 m under the top; after it, it mantles over the
edge onto the floor. Going down is the same backwards. Unreal (`ASimPawnActor::UpdateWallClimb`, PR #389, tuned over four
PC rounds) plays the Game Animation Sample's `M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot` held still, its time
following the climb: evenly up the face to 45% of the clip (`PawnWallClimbGrab`, the hands reach the edge at about 0.7),
then the pull over the top to 68% (`PawnWallClimbEnd`, where it already stands). The pose is drawn about its root, so
the body is lifted by the clip's own rise (stretched to the 3 m wall), not the sim's straight line: the hands stay on
the edge. The sim keeps the ground position; the selection ring lags the body a little mid-climb. Cat-strain jump twice
as high (`JumpHeightFactor`).
