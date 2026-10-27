# Night skip and fast travel

Code: `ColonySim/Travel.h`, `Private/Travel.cpp`, `Sim::step`. Tests: `Tests/TravelTests.cpp`. Data:
`Rules/TravelRules.json`. Task P5. Both are Palworld's, not RimWorld's, so every number is ours.

Both let game time run on while the player waits for nothing. The sim keeps stepping, `batch` game seconds inside one
`Sim::step` (1,200 for a night skip, 120 for fast travel), so the game's own loop sees time jump and can fade the screen.
Nothing is faked: needs, healing, growth, fuel, rot, weather and everyone's jobs run as they always do. Two things differ:
**the narrator is held** (it rolls no incident while skipping, and its plans and cooldowns move later by the time that
passed, `NarratorSystem::postpone`, so nothing due in the skipped hours comes all at once afterwards), and in a night skip
**animals, their husbandry and breeding stand still** (`freezeAnimals`; it keeps a 300-animal map cheap).
Measured: a game day of the 6-colonist scenario costs about 5 s of CPU, so a night of 8 hours is under 2 s.

## Night skip

`SkipNightCommand{}` runs to the end of the sleeping hours (`NeedRules` 22:00 to 5:59, so to 6:00). It fails

- in daytime (outside the sleeping hours),
- while anything threatens the colony (below),
- when a colonist whose schedule says Sleep this hour and who is tired (rest under `fallAsleepBelow`) is not lying in a
  bed or on the ground yet ("2 colonists aren't in bed yet"),
- when nobody is asleep.

Colonists who are rested, on another shift, downed, prisoners or under the player's control do not hold the night back.
They carry on as normal during the skip. The colonist under control is let go where it stands (its job system takes it, so it
goes to bed if it is tired); the `SkipStarted` and `SkipEnded` events carry its id so Unreal can take control again.
Sleepers wake as they always do (rested to 1.0, or the schedule changes), so most are up by 6:00.

## Fast travel

`FastTravelCommand{pawn, x, y}` moves the controlled colonist to a place it has **visited**. The sim marks the 8 m blocks
within 16 m of a controlled colonist on the ground as visited (saved, section `travel`). The trip takes the time of the
walk: the straight line times 1.3 at the colonist's own speed in metres per game second (0.153 for a human, less when hurt),
so 400 m is about 1.5 game hours. The colonist stays where it is, under control, until it arrives; then it is set down at the
target and the world has moved on. It fails for a colonist not under control, downed or burning; upstairs ("go outside
first"); a place nobody can stand, that is not visited, closer than 20 m, with no way there, or while anything threatens.

## Threats and cancelling

A skip stops at once, and says why in `SkipEnded` ("night: something is on fire"), when a threat appears; it is looked for
every 10 game seconds. A threat is: the alarm sounding, a raid squad on the map, a fire (cell or pawn), a pawn of another
faction that is not a prisoner or an animal, a hostile animal (`AnimalSystem::hostile`), a colonist in a mental break, a
drafted or defending colonist (`Fighter`). Visitors and traders (C23) come as pawns of another faction and would count; whoever
adds them gives the check a way to tell them from enemies. `CancelSkipCommand{}` ends a skip now; a cancelled fast travel
leaves the colonist where it is. A travelling colonist that goes down or is let go on the way never arrives.

## For Unreal (U-tasks)

- Events: `SkipStarted` (`message` "night" or "travel", `thing` the colonist, `amount` game seconds it will run) and
  `SkipEnded` (`message` "night: morning", "travel: arrived", "..: cancelled" or the threat; `amount` seconds skipped).
- `Snapshot::travel`: `skipKind` (0 none, 1 night, 2 travel), `skipEndsAt`, `canSkipNight` with `skipWhy` when not,
  counters, `visitedBlocks`. Ask `Sim::travel().visited(cell)` to draw the visited places on the map.
- A bed button that sends `SkipNightCommand`; a map click on a visited place that sends `FastTravelCommand`. Send
  `CancelSkipCommand` on any key press while `skipKind` is not 0.

## Save and replay

Section `travel` 1: the running skip (kind, start, end, colonist, target), the counters and the visited blocks.
Commands `SkipNight`, `FastTravel` and `CancelSkip` by name. A skip is part of `stateHash`. A replay re-plays the commands
and the batches come out the same, since they depend only on the state.
