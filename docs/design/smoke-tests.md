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

## Add a test

In `game/Source/Rim3D/SmokeTest.cpp` write a `Build<Name>Phases()` that adds phases (a name, a
setup function that spawns things and places the camera, a warm-up and a measuring time), add a
branch in `OnWorldBeginPlay`, and report problems with `Fail("...")`. Add a row above. The
harness handles screenshots, timings, the JSON, the timeout and the exit code.

