# Smoke tests

Every Unreal-side task (the U and A rows in the Stage 1 plan) proves itself with a smoke test:
the game starts, does something visible, saves screenshots and timings, and quits by itself.

## Run one (sneed's PC, only while it is free)

```
powershell -ExecutionPolicy Bypass -File tools\smoke.ps1 -Test load
```

The script refuses to run if Unreal is already open or the graphics card is busy (override with
`-Force`). It then builds `Rim3DEditor` at below-normal priority with 6 compiler jobs, runs the
test at below-normal priority and prints a table. By default the game draws offscreen (no window
appears, so it is safe while sneed uses the PC) at a small default size; add `-Visible` (with
`-Resolution 2560x1440`) for performance numbers, since offscreen ignores the resolution.
`-NoBuild` skips the build, `-BuildOnly` only compiles. Exit code: 0 pass, 1 test failed, 2 build failed or PC busy,
3 crashed or timed out.

## Output

- `game/Saved/Smoke/<test>.json`: `status` (pass or fail), `errors`, `notes`, and per phase the
  average fps, median game thread, render thread and GPU milliseconds, and the screenshot path.
- `game/Saved/Smoke/<test>/<phase>.png`: one screenshot per phase.

A test fails if: its level does not load, a phase produces no frames, a required asset or
subsystem is missing, it runs past `-SmokeTimeout` (default 300 s), or the game crashes (no JSON).

## Tests

| Name | What it checks |
| --- | --- |
| `load` | Default level opens, `USimSubsystem` started, frames render, a screenshot is taken (about 10 s). |
| `crowd` | 10 MetaHumans close up, then 100 MetaHumans, 100 and 200 mannequins running (Stage 0 test). |
| `terrain` | The sim's 1 km height map as a runtime mesh from three cameras (Stage 0 test). |
| `sim` | The bridge (U2): defs load from `data/`; commands (one bad) give the right events and show in the snapshot; at 3x game time runs at triple speed and hauling pawns move on screen every frame (snapshot blending); Space pauses, `.` steps once, F7 saves a replay that plays back to the same hash in a fresh sim. About 40 s. |
| `overseer` | The overseer (U6): clicks on screen pick the right pawn, wall, item and ground cell; camera pan, turn, tilt and zoom with limits; selecting a pawn and a cell; the ImGui panels draw. Screenshots show the inspect panel. About 20 s. |
| `pawns` | Pawns (U5): 100 hauling pawns get an actor each that follows the sim; the 20 nearest the camera are motion matched, the rest play loops at 20 or 8 updates a second; pick-ups and put-downs play an action animation; then the same as MetaHumans with strand hair only up close. Overseer view and close-up screenshots. About 70 s. |
| `saves` | Saves in the game (U12): with the autosave interval cut to 120 game seconds, autosaves appear; F5 writes the quicksave at a paused moment; the game runs on, F9 puts it back with the same state hash and the same things in the snapshot; 300 steps on after the load equal 300 steps of a second sim loaded from the same file; a damaged and a missing save are refused without touching the game; the pawn actors match the sim's pawns; F8 writes a bug report whose replay plays back to the same hash. About 20 s. |
| `protagonist` | Direct control (U9, [protagonist.md](protagonist.md)): Tab into direct mode on the protagonist; aim at another colonist and take it over with T, then take the protagonist back; walk 5 s in third person (speed, sim and local position agree, full motion matching); walk into a wall and slide along it; first person (camera at the head, body hidden); paused body stays still and 3 clamps to 1x; a save made in direct mode loads back into direct mode; Tab back to the overseer view and the session replays to the same hash. Needs a wall def, so it copies `data/` to `Saved/Smoke/protagonist-data` and adds one. About 30 s. |

`load` also fails if the sim loaded no defs from `data/`. A test phase can have a `Verify` function
(runs when the phase ends, calls `Fail`) and an `OnFrame` function (runs every frame).

## Add a test

In `game/Source/Rim3D/SmokeTest.cpp` write a `Build<Name>Phases()` that adds phases (a name, a
setup function that spawns things and places the camera, a warm-up and a measuring time), add a
branch in `OnWorldBeginPlay`, and report problems with `Fail("...")`. Add a row above. The
harness handles screenshots, timings, the JSON, the timeout and the exit code.

