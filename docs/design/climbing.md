# Wall climbing

sneed (2026-11-19): pawns climb sheer walls, cat-strain twice as fast, with an animation at the top. Code: `Map`
(`setWallClimb`, `refreshWallClimb`, `linkAt`), `MovementSystem` (climb speed), `Sim::writeSnapshot` (the drawn path),
`PawnActor.cpp` (`UpdateWallClimb`, follow-up PR). Data: `Rules/ClimbRules.json`. Tests: `Tests/ClimbTests.cpp`.

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
edge onto the floor. Going down is the same backwards. Unreal (a follow-up PR, compiled on the PC) plays the Game Animation Sample's
`M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot` with its time following the climb (the face up to the catch at 45%
of the clip, the mantle the rest) and its root travel taken out (the sim moves the body). Tune `PawnWallClimbGrab` on
the PC by eye.
