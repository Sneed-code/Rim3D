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
| `looks` | Seeded pawn looks (A3, [looks.md](looks.md)): the Crashlanded colonists and 14 more as MetaHumans, some Emberkin; every frame each pawn's height, body shape morphs on body and clothes, skin material and colour, and horns match its sim looks; the looks vary; screenshots close up and from the overseer camera with horn and head sizes in pixels; lineages changed back and forth; 100 walking pawns for the cost. Needs the generated looks assets. About 40 s. |

| `world` | Terrain and plants (U3, [terrain.md](terrain.md)): a 256 m map from seed 42 is drawn as 8 x 8 chunks and every sim plant has an instance; low and overview shots; colonists mine a rock cell placed on open ground and the `CellMined` event rebuilds that chunk (fewer vertices, rough stone floor); a tree placed by a dev tool is picked up by the plant scan within a second, then colonists fell it and the `PlantRemoved` event redraws the chunk; a four-cell tunnel in the mountain gets its thick roof, drawn with `Terrain.Roofs 1` and hidden with `0`; then a 1 km map (32 x 32 chunks, about 510,000 plants) with its full build time. To keep a test quick the colonists work all 24 hours and the test runs extra sim steps. About 110 s. |
| `buildings` | Buildings and items (U4, [building-draw.md](building-draw.md)): every item of the Crashlanded start is drawn (36 things, as piles); a stack of 1 steel and a stack of 75 add 1 and 6 instances; a room of 17 steel walls, a door, a wooden bed and table and a stove is planned (21 blueprints drawn as blue ghosts) and the colonists build it, frames rising inside their ghosts, until all 21 stand; what is drawn matches the snapshot kind by kind at every step; the room is shown from above with the roof off and from the side roofed; a stockpile is made inside and a colonist hauls steel to it, so the door slides fully open and shuts again; a new sim wipes the picture. Needs the ghost material the `BuildingMaterials` commandlet makes (below). About 145 s. |
| `orders` | Orders (U7, [orders.md](orders.md)): the architect menu offers the eight buildings and refuses anything else; a line of six walls dragged out shows six green ghosts and plans six blueprints; a bed turned east is two cells by one; a ghost on a plan is red and plans nothing; door, table and stove are planned; four rock cells are marked by a real press and release at their screen positions, a tree is chopped, a bush harvested and a stockpile dragged out, and the overlay has one instance for each mark and each zone cell; a wall is marked for deconstruction and Cancel takes off plans, that mark and the mine marks; R drafts a colonist, a right click on the ground is a move order, a right click on a pawn of another side an attack order, the alarm drafts everyone and stands them down again; the sim refused no command. Needs the ghost material (below). About 20 s. |
| `panels` | Colony panels (U8, [panels.md](panels.md)): the work tab's clicks step a priority forward and back and refuse work a colonist can never do; the schedule tab's clicks change an hour and undo it; a wound shows in the colonist panel's health report and the message log; a stove takes three bills (times, until have, forever), one is paused, one changed, one removed, each agreed by the sim; a refused order reaches the log; every panel drew. About 20 s. |

`buildings` and `orders` need `game/Content/Rim3D/Generated/Buildings/M_BuildingGhost` (not in Git): make it once on each machine with
`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=BuildingMaterials -unattended -nosplash -nullrhi`. Without it the game still runs and draws plans solid.

`load` also fails if the sim loaded no defs from `data/`. A test phase can have a `Verify` function
(runs when the phase ends, calls `Fail`) and an `OnFrame` function (runs every frame).

## Add a test

In `game/Source/Rim3D/SmokeTest.cpp` write a `Build<Name>Phases()` that adds phases (a name, a
setup function that spawns things and places the camera, a warm-up and a measuring time), add a
branch in `OnWorldBeginPlay`, and report problems with `Fail("...")`. Add a row above. The
harness handles screenshots, timings, the JSON, the timeout and the exit code.

