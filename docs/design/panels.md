# Colony panels

Code: `game/Source/Rim3D/PanelsSubsystem.*`. Smoke test: `panels` (`tools/smoke.ps1 -Test panels`).
Task U8. Unreal side only; the sim is untouched. Everything is ImGui, drawn only while the overseer
view is the one in use ([overseer.md](overseer.md)); `Panels.UI 0` (or `Overseer.UI 0`) hides it.

`UPanelsSubsystem` reads the sim only through `USimSubsystem` (snapshot, `GetSim()` for the colonist
record, the work and schedule rows, mood thoughts and health report) and changes it only by commands.
While the game is paused a command makes the sim step once, so a click shows at once.

## What is on screen

| Where | What |
| --- | --- |
| Top left | **Work**, **Schedule** and **Log** buttons, and a bar with one button per colonist, coloured by mood band (green, yellow, orange, red; grey when down). Click selects, double click centres the camera, hover shows mood, food and rest |
| Middle | **Work tab**: a row per colonist, a column per kind of work that has a scanner in the sim. A cell shows 1 (done first) to 4 (last), `-` for never, `X` for work the colonist can never do. Left click goes `0 > 1 > 2 > 3 > 4 > 0`, right click goes back. Hover shows the work and the colonist's level in its skill. Sends `SetWorkPriorityCommand` |
| Middle | **Schedule tab**: a row per colonist, 24 hour columns, each A (anything), W (work), J (joy) or S (sleep); the current hour is marked `>h<`. Left click goes forward, right click back. Sends `SetScheduleCommand` |
| Left, under the bar | With a **colonist** selected: name, age, backstories, traits, what they are doing, food and rest bars, the mood bar with the three break levels and every thought with its offset (memories show how long they last), their bed (button to free it, list to assign another: `AssignBedCommand`), skills with passions (`*` minor, `**` major) and the health report (pain, bleeding, blood lost, injuries, missing parts, abilities). With a **stove** (any finished building with recipes) selected: its bills in order, each with mode, count (`-10 -1 +1 +10`), Pause/Resume and Remove, and a form to add one. Sends `AddBillCommand`, `SetBillCommand`, `RemoveBillCommand` |
| Right, under the alarm | **Message log**: the last 5 lines, or 40 with the Log button on. Colonist deaths, going down and getting up, wounds, lost parts, mood trouble (crossing a break threshold, calming down), finished bills, and orders the sim refused. Time stamped; a line about a colonist selects and centres them when clicked. 300 lines kept, cleared when the sim is replaced |

## Notes

- Names: labels come from the defs (`label`), so a rename in `data/defs/` or `naming.md` shows here with no code change.
- The Work tab hides work types with no scanner yet (nothing happens for them), so it grows as C-tasks add work.
- The alarm is still U7's button; the log is placed under it.
- Not here yet: priorities for many colonists at once (drag, column header click), stockpile filter and priority panels,
  per-bill ingredient and worker filters, a bill's "store in" and "pause when" options, JOY choices (no recreation exists yet), and
  clicking a log line about a thing that is not a pawn.

## Smoke test `panels` (about 20 s)

Driven by the calls the buttons make, on the Crashlanded start (paused).

1. `panels_setup`: the panels list at least three colonists and drew.
2. `panels_work`: a click moves a priority one step and a right click undoes it; the colonist's mining ends at priority 1 for the
   picture; a click on work the colonist can never do sends nothing. The Work tab drew.
3. `panels_schedule`: a click changes hour 10 and a right click undoes it, three evening hours are Joy for the picture, hour 24 is refused. The Schedule tab drew.
4. `panels_colonist`: a colonist is cut; the panel drew, the health report has the injury and the log says they were hurt.
5. `panels_bills`: a stove is placed; three bills are added (a number of times, until have, forever), the first is paused, the second changed to
   "5 times", the third removed; the sim agrees with each; the bills panel drew.
6. `panels_log`: a bill on a colonist is refused by the sim and the log says "Refused"; the open log drew.

Screenshots are in `game/Saved/Smoke/panels/`. Not covered (needs a person): the mouse itself, the combo boxes, and layout at 4K.
