# The overseer: camera, selection and panels

Code: `game/Source/Rim3D/OverseerPawn.*`, `OverseerSubsystem.*`, `SimPicking.*`. Smoke test:
`overseer` (`tools/smoke.ps1 -Test overseer`). Task U6. Unreal side only; the sim is untouched.

## What it is

The overseer is the RimWorld view in 3D: a free camera over the colony, a click to select, and
panels for the speed and for what is selected.

- `AOverseerPawn`: the camera body. It looks at a **focus** point on the sim's ground from a
  distance, yaw and tilt. The focus stays on the map and rides the ground height; the camera never
  dips under the ground.
- `UOverseerSubsystem`: one per game world. Spawns the pawn (`EnterOverseerMode`), holds the
  **selection**, draws the highlight and the ImGui panels.
- `SimPicking`: what a ray hits. It uses the sim, not Unreal collision.

Start it with `-Overseer` on the command line or `Overseer.Enter` in the console. Since U9
([protagonist.md](protagonist.md)) Tab switches between this and walking a colonist yourself:
`UProtagonistSubsystem` calls `LeaveOverseerMode` (hides the panels, keeps the camera and the
selection) and later `EnterOverseerMode` again, centred on the colonist that was controlled.
`IsOverseerActive` is true only while the overseer view is the one in use.

## Keys

| Key | Does |
| --- | --- |
| W A S D, arrows | pan (Shift: 3 times faster; faster when zoomed out) |
| Q / E | turn the camera |
| mouse wheel | zoom (each notch is 15% closer or further, 6 m to 300 m) |
| hold middle button, move | turn and tilt (tilt 25 to 85 degrees) |
| left click | select the pawn, item or building under the cursor; else the ground cell; else nothing |
| F | centre on the selection |
| R, right click, Esc | with U7 ([orders.md](orders.md)): R drafts the selected colonist (or turns a building ghost), right click orders a drafted colonist, Esc puts a tool down before it clears the selection |
| Home | reset distance, tilt and turn |
| Esc | clear the selection |
| Tab | take control of the selected colonist (else the protagonist): direct mode, see [protagonist.md](protagonist.md). The inspect panel has the same as a button |
| Space, 1, 2, 3, `.`, F5, F7, F8, F9 | the bridge's keys (`USimSubsystem`), unchanged; F5 and F9 quicksave and quickload, F8 writes a bug report (`saves.md`) |

The camera methods behind the keys (`Pan`, `Rotate`, `Zoom`, `FocusOn`, `ResetView`) are public,
which is how the smoke test drives them.

## Picking

`SimPicking::PickFromRay(bridge, origin, direction)`:

1. Every thing in the current snapshot is a box at its **blended** position (the same one the
   picture shows): pawns 90 cm wide and 190 tall, items 70 cm wide and 40 tall, buildings their
   footprint and a placeholder 250 cm height until the meshes (U4) say how tall they are.
   The nearest box hit wins.
2. The ground is `USimSubsystem::SimToWorld` of the sim's height map, found by marching the ray
   (steps of 20 to 300 cm) and then halving the last step 16 times. The cell is the one under the hit.
3. A thing wins if it is nearer than the ground hit. Nothing under the ray (sky, off the map) is
   `ESimPickKind::None`.

Because it reads the sim, it keeps working when the art changes (U3 terrain meshes, U4 and U5
meshes). A mesh that is much taller or wider than its box will click "wrong"; give such a def a
bigger box here rather than using Unreal collision.

## Selection

One selection at a time: a thing (`FSimPick::Thing`) or a cell (`FSimPick::Cell`).
`UOverseerSubsystem::Select` and `ClearSelection` change it; U7 ([orders.md](orders.md)) reads it for draft and
move orders. A selected thing that leaves the map (picked up, destroyed) clears itself. The
selection is a white box around a thing (cyan line along a pawn's route ahead) or a yellow outline
on the ground for a cell, drawn with debug lines until U4 has real highlight materials.

## ImGui

ImGui comes from the free plugin **VesCodes/ImGui** (MIT, Dear ImGui 1.92.8), pinned in
`assets/manifest.json`. The plugin folder `game/Plugins/ImGui/` is not in Git (like the other
third-party packs); the manifest says how to clone it again. It is enabled in `Rim3D.uproject` and
the `Rim3D` module depends on `ImGui`.

How to draw: in a tick, `const ImGui::FScopedContext Scoped; if (Scoped) { ImGui::Begin(...); ... }`.
The plugin adds its overlay to the game viewport; ImGui takes the mouse only while the cursor is
over one of its windows, so clicks on a panel never reach the world. All later UI (U7 architect
menu, U8 colony panels) uses the same pattern and should add its panels in
`UOverseerSubsystem::DrawPanels` or its own subsystem, scaled the same way (font scale from the
viewport height: 1.2 at 1080p, up to 2.4 at 4K and above).

Panels (`Overseer.UI 0` hides them):

- **Top middle: speed.** Day and time, Pause / 1x / 2x / 3x (the current one is highlighted) and a
  Step button while paused. They call `USimSubsystem::SetSpeed` and `StepOnce`, the same as the keys.
- **Bottom left: inspect.** A thing: label from its def, category and id, cell, ground height, hit
  point bar, and by category the pawn's speed and where it is heading, an item's stack, a
  building's footprint. A cell: terrain and height, walkable or door, the building on it and the
  items and pawns standing in it. Needs, mood, skills and health are in the colonist panel of U8 ([panels.md](panels.md)).
- **Bottom right: key hints.**

## Smoke test `overseer` (about 20 s)

1. `overseer_picking`: a 128 m map with 12 hauling pawns, 40 items and 6 walls on a ground mesh.
   Takes the overseer camera zoomed out, projects the middle of every thing to the screen and picks
   there (80% must pick that exact thing, the rest may be hidden behind another thing and none may
   find only ground); picks 3 to 8 empty cells (all must be the right cell); a ray at the sky picks nothing.
2. `overseer_camera_controls`: pan follows the yaw, tilt and zoom stop at their limits, zoom in is
   closer than zoom out, the focus stays on the map after a huge pan.
3. `overseer_select_pawn`: a click by screen position selects that pawn and the selection survives
   while it walks; the ImGui panels drew in at least 10 frames. Screenshot shows the inspect panel.
4. `overseer_select_cell`: a click on empty ground selects that cell. Screenshot shows the cell outline.

Not covered by the smoke test (needs a person at the keyboard): the keyboard and mouse bindings
themselves and clicking the speed buttons.
