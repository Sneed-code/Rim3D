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
| `faces` | Seeded faces, hair and skin marks (V2, V3, [looks.md](looks.md)): twelve pawns in a row as MetaHumans at a summer morning; every frame each one's shaped face and its morph weights, eye colour material, hair, beard, moustache and brow grooms, materials marked for morph targets, and (at full detail) one decal per tattoo and scar plus body hair match its sim looks; no two of the twelve look alike; close-ups, four 1080p portraits with a lamp, marks on a bare pawn and on a clothed colonist (only skin shows them); 100 pawns for the cost. V2b: every frame a woman's face skin is a female Creator skin and a man's a male one or Kellan's, at least four different face skins in the row, and the bare skin's colour is the pawn's; close shots of the first woman and of the darkest-skinned pawn show face, neck, hands and bare skin together; every frame a pawn away from full detail shows its hair style's cards (and one at full detail none), and with `Pawns.FullDetail 0` the row from 5 m shows them. Needs the PawnLooks, ClothesArt and PawnFaces assets. About 70 s. |

| `world` | Terrain and plants (U3, [terrain.md](terrain.md)): a 256 m map from seed 42 is drawn as 8 x 8 chunks and every sim plant has an instance; low and overview shots; colonists mine a rock cell placed on open ground and the `CellMined` event rebuilds that chunk (fewer vertices, rough stone floor); a tree placed by a dev tool is picked up by the plant scan within a second, then colonists fell it and the `PlantRemoved` event redraws the chunk; a four-cell tunnel in the mountain gets its thick roof, drawn with `Terrain.Roofs 1` and hidden with `0`; then a 1 km map (32 x 32 chunks, about 510,000 plants) with its full build time. To keep a test quick the colonists work all 24 hours and the test runs extra sim steps. About 110 s. |
| `buildings` | Buildings and items (U4, [building-draw.md](building-draw.md)): every item of the Crashlanded start is drawn (36 things, as piles); a stack of 1 steel and a stack of 75 add 1 and 6 instances; a room of 17 steel walls, a door, a wooden bed and table and a stove is planned (21 blueprints drawn as blue ghosts) and the colonists build it, frames rising inside their ghosts, until all 21 stand; what is drawn matches the snapshot kind by kind at every step; the room is shown from above with the roof off and from the side roofed; a stockpile is made inside and a colonist hauls steel to it, so the door slides fully open and shuts again; a new sim wipes the picture. Needs the ghost material the `BuildingMaterials` commandlet makes (below). About 145 s. |
| `orders` | Orders (U7, [orders.md](orders.md)): the architect menu offers the eight buildings and refuses anything else; a line of six walls dragged out shows six green ghosts and plans six blueprints; a bed turned east is two cells by one; a ghost on a plan is red and plans nothing; door, table and stove are planned; four rock cells are marked by a real press and release at their screen positions, a tree is chopped, a bush harvested and a stockpile dragged out, and the overlay has one instance for each mark and each zone cell; a wall is marked for deconstruction and Cancel takes off plans, that mark and the mine marks; R drafts a colonist, a right click on the ground is a move order, a right click on a pawn of another side an attack order, the alarm drafts everyone and stands them down again; the sim refused no command. Needs the ghost material (below). About 20 s. |
| `panels` | Colony panels (U8, [panels.md](panels.md)): the work tab's clicks step a priority forward and back and refuse work a colonist can never do; the schedule tab's clicks change an hour and undo it; a wound shows in the colonist panel's health report and the message log; a stove takes three bills (times, until have, forever), one is paused, one changed, one removed, each agreed by the sim; a refused order reaches the log; every panel drew. About 20 s. |
| `handson` | Hands-on work (U10, [handson.md](handson.md)): a colonist of ours on a flat map, taken over in first person; holding F on a rock (most of it already mined) digs it out in place with the prompt and progress bar on screen, on a tree fells it and drops wood, on a stack of steel picks it up, on a building plan puts the steel in (the rest is put down) and then builds the wall; B takes the build ghost, aimed by the crosshair, which is drawn and plans a wall on a click; X puts a stack down; Tab gives the colonist back. About 90 s. |
| `fights` | Fights on screen (U11, [fights.md](fights.md)): E picks up a gun and the placeholder gun shows at the chest, C crouches; the crosshair finds a pawn's chest through the sim's own trace; the trigger is held for 14 s on a pawn 12 m away: every shot has a muzzle flash and a tracer, bullets make impacts, one hits and the pawn flinches and gets a wound decal and blood on the ground, the player sees a hit marker, R reloads, and every hit lies within 5 cm of the pawn's hitboxes; a shot frozen with its tracer in the air and a hit frozen with blood in the air (screenshots); the hitbox debug view over the pawns, checked against the drawn body; a pawn with 400 damage to the torso becomes a ragdoll and lies low near where it fell, and downed pawns are limp while pawns that are up are not; a firefight of four colonists against four raiders in the open. Needs the decal material the `FightMaterials` commandlet makes (below). About 90 s. |
| `levels` | Upper floors on screen (U17, [levels-draw.md](levels-draw.md)): a flat Crashlanded map run on to 10:00; a walled house with a door and stairs north of the protagonist, then an upper floor and landing (89 cells, three of them over the stairs and not drawn), a wall, wood, a ladder and a colonist upstairs; the floor mesh's top at the sim's floor height, the level 1 instances, the ladder's link; the overseer looking at level 1 (a click lands on the floor's cell) and, after Page Down, at the ground (level 1's floor, buildings and colonist hidden, the click lands on the ground); direct mode: the protagonist walks to the foot of the stairs and into them, is paused halfway (drawn 60 to 270 cm up), arrives at the head on level 1, climbs back down, and a view of the ceiling from inside. About 45 s. |
| `melee` | Melee and weapon switching on screen (U22, [melee-draw.md](melee-draw.md)): a rifle, revolver and gladius given to the protagonist are drawn (rifle in the hands, the others holstered on the back and the hips); Q switches weapons and both weapons move on the way (one of them stopped at three quarters); the swing key does nothing with a gun in the hands and the fire button nothing with a blade; an enemy one metre ahead is picked as the swing's target, the game stopped near the top of the wind-up (blade raised, body leaning back) and just after the hit moment (blade low, body leaning forward, hit-stop on both pawns, blood, flinch); nine seconds of swings land every blow on the picture within 2.5 steps of the sim's hit moment; the right mouse button holds the blade across the chest; two Melee 20 fighters duel at 3x speed with sidesteps and blocks played on screen. About 70 s. |

`fights` needs `game/Content/Rim3D/Generated/Fights/M_WoundDecal` (not in Git): make it once on each machine with
`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=FightMaterials -unattended -nosplash -nullrhi`. Without it the game still runs and draws no wound decals, and the test says so.

`buildings` and `orders` need `game/Content/Rim3D/Generated/Buildings/M_BuildingGhost` (not in Git): make it once on each machine with
`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=BuildingMaterials -unattended -nosplash -nullrhi`. Without it the game still runs and draws plans solid.

`load` also fails if the sim loaded no defs from `data/`. A test phase can have a `Verify` function
(runs when the phase ends, calls `Fail`) and an `OnFrame` function (runs every frame).

## Add a test

In `game/Source/Rim3D/SmokeTest.cpp` write a `Build<Name>Phases()` that adds phases (a name, a
setup function that spawns things and places the camera, a warm-up and a measuring time), add a
branch in `OnWorldBeginPlay`, and report problems with `Fail("...")`. Add a row above. The
harness handles screenshots, timings, the JSON, the timeout and the exit code.

