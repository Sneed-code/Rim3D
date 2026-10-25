# The protagonist: walking a colonist yourself

Code: sim side `ColonySim/Commands.h` (`ControlPawnCommand`, `DirectMoveCommand`), `Things.h`
(`PlayerControl`), `Sim.cpp`, `MovementSystem.cpp`; Unreal side `game/Source/Rim3D/ProtagonistPawn.*`,
`ProtagonistSubsystem.*`. Tests: `Tests/ControlTests.cpp` and the smoke test `protagonist`
(`tools/smoke.ps1 -Test protagonist`). Task U9.

In direct mode the player walks one colonist, the protagonist to start with, in first or third
person. One key (Tab) switches between that and the overseer view. In the overseer view the colonist
goes back to the sim's own routines.

## Who owns what

**The sim decides, Unreal shows.** The colonist is a normal sim pawn all the time. What changes in
direct mode is who moves it.

| | Does |
| --- | --- |
| Sim | owns the position, keeps the pawn out of walls, turns the player's request into movement and a velocity in the snapshot, saves and replays all of it |
| `AProtagonistPawn` | the camera body: reads WASD and the mouse, keeps a local copy of the position, moves it against the sim's walls each frame |
| `UProtagonistSubsystem` | tells the sim who is controlled, sends the position every step, switches views, draws the HUD |
| `USimPawnSubsystem` | draws the colonist exactly like every other pawn: from the sim's snapshot, motion matched (it is the nearest to the camera, so it gets full detail) |

So the body you see is the sim's pawn, not a second character: it follows the same blend between
snapshots, plays the same animations (the Game Animation Sample mannequin, or a MetaHuman with
`Pawns.MetaHumans 1`) and is animated from the velocity the sim reports.

## The two commands

- `ControlPawnCommand{pawn}`: the player takes over `pawn`. Only a colonist on the map; the
  hauling placeholder's pawns refuse. Taking a second colonist lets the first go where it stands.
  `kNoEntity` lets go of whoever is controlled. It marks the pawn with the `PlayerControl`
  component, clears its path and stops it.
- `DirectMoveCommand{pawn, x, y}`: where the body wants to be, in metres. Sent **every sim step**.
  The sim moves the pawn toward it by at most `kDirectMaxStep` (0.5 m a step, 15 m/s at 1x, over
  three times the fastest pawn), tries the whole move, then sliding along one axis, then staying
  (`pawnFitsAt`, the same test the pathfinder's smoothing uses), and sets the pawn's velocity to the
  distance it really moved. A step without a command leaves the velocity at zero. Fails only if the
  pawn is not the controlled one or the numbers are not numbers.

While a pawn has `PlayerControl`, the movement system skips it (the other pawns still steer around
it) and C1's job system must too: **when the job system exists, taking a colonist over cancels its
job and the AI does not run for it until control is let go.**

Both are logged like every command, so a session with direct play replays exactly (`simrun
--replay`), saves in format 5 and is part of `stateHash`. A `DirectMove` per step is about 3 MB of
command log per game day, as the bridge doc already planned.

## Unreal side

**Moving.** `AProtagonistPawn::Move` runs every frame: the keys give a direction relative to the
camera yaw, the speed is the pawn's own (`stats.moveSpeed` times 30, 4.6 m/s for a human) times the
sim's ground and slope factor (`walkSpeedFactor`, so sand and hills slow you like any pawn), Ctrl
walks at 1.5 m/s, and the velocity eases in and out. The new position is checked with the sim's
`pawnFitsAt` and slides along walls the same way the sim does, so the two agree.

**Once per step** (`USimSubsystem::OnBeforeStep`), the subsystem submits a `DirectMoveCommand` with
that position. **Once per frame** it compares with the snapshot: if the local copy is more than a
metre away from the sim's pawn (a wall appeared, a load, a hitch), the sim's position wins
(`Reconcile`). In the smoke test the two stay within 0.22 m and never needed a correction.

**Speed.** The body moves in real time, so direct mode holds the game at **1x or paused**
(`USimSubsystem::SetMaxSpeed(1)`; the 2 and 3 keys and buttons clamp to 1). Paused, the body
freezes (the sim would not apply its moves anyway) and the view still turns. Leaving direct mode
lifts the cap.

**Camera.** First person: at the head, 165 cm up, and the player's own body is hidden (so you
don't see inside your own head). Third person: 3.5 m behind and above a point 1.5 m up, wheel zooms
(1.5 to 8 m), never under the ground. The camera follows the sim's blended position, so it moves
as smoothly as the body.

## Keys

| Key | Does |
| --- | --- |
| Tab (overseer view) | take control of the selected colonist, else the protagonist |
| Tab (direct mode) | back to the overseer view, centred on the colonist, which is selected |
| W A S D | walk, relative to where the camera looks; Ctrl for a slow walk |
| mouse | look (yaw and pitch, up to 80 degrees) |
| wheel | third person distance |
| V | first and third person |
| T | take over the colonist under the crosshair (within 25 m) |
| Space, 1 | pause, 1x (2 and 3 stay at 1x) |
| F5, F9, F8, F7 | the bridge's keys; a save made in direct mode puts you back in direct mode when you load it |

The overseer's inspect panel also has a **Take control (Tab)** button for a selected colonist. The
console has `Protagonist.Enter` and `Protagonist.Exit`. The game starts in direct mode with `-Direct`
(use `-SimScenario=Crashlanded` so there is a protagonist; without a scenario the sim has no
colonists and the game says so in the log).

## Smoke test `protagonist` (about 30 s)

On a 128 m flat map with the Crashlanded start and a wall def added to a copy of `data/` (the real
data has no buildings yet):

1. `overseer_view`: the overseer is active, nobody is controlled, the cap is 3x.
2. `direct_possess`: Tab takes the selected protagonist; aiming at another colonist and pressing T
   takes it over; taking the protagonist back lets the other one go (it stands still); three
   `ControlPawn` commands are logged.
3. `direct_walk_third_person`: holds W for 5 s: at least 12 m, 3 to 5 m/s in the snapshot, the
   local position within a metre of the sim and at most 2 corrections, the drawn pawn within 60 cm
   of the sim, full motion matching in 90% of frames, the camera at least 2 m away, the game held
   at 1x, the HUD drew.
4. `direct_wall`: a wall across the way; the colonist stops short of it (never inside, in the sim
   or locally), then slides along it for at least 2 m.
5. `direct_first_person`: the camera is at the head and the body is hidden.
6. `direct_pause_and_speed`: paused, the body does not move; pressing 3 gives 1x; the body shows
   again after leaving first person.
7. `direct_save_and_load`: a save made in direct mode, loaded from the overseer view, puts the
   player back in direct mode on the same colonist.
8. `back_to_overseer`: Tab leaves direct mode; the sim lets the colonist go; the colonist is
   selected and visible; the cap is lifted; the whole session (about 600 commands) replays to the
   same state hash.

The keyboard and mouse themselves are not covered (the test drives the same methods); a person has
to try W A S D, the mouse and Tab once.

## Not done here

- **Facing and strafing.** The body turns toward where it walks, so walking backwards turns the
  character around, and aiming does not turn it. Strafe and backpedal animations need the
  sample's aiming databases and a facing in the snapshot (U11 needs both for shooting).
- **Camera against walls.** The third person camera only avoids the ground. Walls are debug boxes
  until U4; it can see through them for now.
- **Stairs, jumping, crouching, sprinting.** None of them (no levels in Stage 1, no sim state for
  stance). The slow walk is a key, not a sim state.
- **Hands-on work** (chop, mine, build, pick up) is U10; shooting is U11. Both use what is here:
  the controlled pawn, its position and the crosshair.
- **Pawns in the way.** Other pawns steer around the player, but the player walks through them
  (`DirectMove` checks walls only).
- **Colonists that fall asleep, go down or die while controlled.** Nothing in the sim does that yet.
  If the controlled thing stops existing, the game goes back to the overseer view.
