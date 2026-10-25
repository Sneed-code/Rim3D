# The bridge: commands, events, snapshots and replays

Code: `ColonySim/Commands.h`, `Events.h`, `Snapshot.h`, `Replay.h`, `Hash.h`, `Private/Bridge.cpp`,
`Private/Replay.cpp`, and the bridge part of `Sim.h`/`Sim.cpp`. Tests: `Tests/BridgeTests.cpp`,
`Tests/ReplayTests.cpp`, the `replay_record` and `replay_check` ctest runs. Task S5.

Unreal and the sim talk only through three things (architecture.md, "Sim to Unreal bridge"):

| Direction | What | Where |
| --- | --- | --- |
| Unreal to sim | **Commands**, applied at the start of the next step and logged | `Sim::submit`, `Sim::commandLog` |
| Sim to Unreal | **Events**, a list per step | `Sim::events` |
| Unreal reads sim | **Snapshot**, a read-only copy | `Sim::writeSnapshot` |

Plus two tools on top: a **state hash** (`Sim::stateHash`) and **replays** (seed plus command log).

## Commands

A `Command` is a `std::variant` of one struct per command. `Sim::submit(command)` queues it and
returns its id (1, 2, 3... in submit order). At the start of the next `step()`, before any system
runs, every queued command is applied in submit order and appended to `commandLog()` as
`LoggedCommand{step, command}`, where `step` is the game second (`time().seconds`) of that step.
A command that can't be applied changes nothing, sends a `CommandFailed` event with its id and a
reason, and is still logged.

There are four dev commands, like RimWorld's dev mode, since C1 the work and stockpile
commands (below the table, and in [jobs.md](jobs.md)), and since U9 the two for direct control
([protagonist.md](protagonist.md)). The rest (place blueprint, designate, draft, actions) come with
the tasks that build those systems.

| Command | Does | Fails when |
| --- | --- | --- |
| `SpawnThingCommand{def, cell, count, rot}` | `Things::spawn` of a Things def | unknown def; the thing can't go there |
| `DestroyThingCommand{thing}` | `Things::destroy`; jobs using it end first (`JobSystem::forget`) | the thing doesn't exist |
| `DamagePawnCommand{pawn, damageType, amount, part}` | `HealthSystem::applyDamage`; `part` -1 picks like a hit would ([health.md](health.md)) | the thing has no body; unknown damage type; no damage; the pawn is dead or the part is missing or doesn't exist |
| `TendPawnCommand{pawn, quality}` | `HealthSystem::tendAll` | the thing has no body; nothing to tend |
| `SetWorkPriorityCommand{pawn, workType, priority}` | a pawn's Work tab: 0 off, 1 to 4 | not a working pawn; unknown work type; priority outside 0 to 4; work it can never do |
| `CreateStockpileCommand{area, priority}` | a stockpile zone (ids 1, 2, 3...) | no walkable cell outside other zones in `area` |
| `RemoveZoneCellsCommand{area}` | takes cells out of their zones | no zone cell in `area` |
| `SetStockpilePriorityCommand{zone, priority}` | | unknown zone; Unstored |
| `SetStockpileAllowedCommand{zone, def, allowed}` | the zone's filter | unknown zone or def |
| `OrderHaulCommand{pawn, thing}` | "prioritize hauling": ends the pawn's job, hauls the item | not a working pawn; not an item; held by another pawn; unreachable; no room; the player is controlling the pawn |
| `ControlPawnCommand{pawn}` | the player takes over one colonist (the previous one is let go, its job ends); `kNoEntity` lets go ([protagonist.md](protagonist.md)) | not a living colonist on the map |
| `DirectMoveCommand{pawn, x, y}` | moves the controlled pawn toward (x, y) metres, at most 0.5 m a step, out of walls | not the controlled pawn; downed or dead; x or y is not a number |

In replay files an `area` is `[minX, minY, sizeX, sizeY, level]` and a storage priority its name
(`"Preferred"`; `"Normal"` if left out).

**Speed and pause are not commands.** They decide how often Unreal calls `step()`, not what a step
does, so they stay in `USimSubsystem`. A replay counts steps, so it doesn't need them.

**Adding a command:** a struct in `Commands.h` added to the `Command` variant; its name in
`commandName` (`Bridge.cpp`); applying it in `Sim::applyCommand`; reading and writing it in
`Replay.cpp` (`replayCommandJson`, `replayReadCommand`); a test. The `static_assert`s in the
visitors stop the build if one of the first two is forgotten. Things are named by `Entity`; the
same commands give the same ids, so replays can name them too. Never apply a command anywhere but
`applyCommand`, and never change sim state from outside a command.

## Events

`Sim::events()` lists what happened during the last step; it is cleared when the next step
starts, so read it after every step. Before the first step it holds what setting up the map did.
`Event` is one flat struct; each type uses only its fields.

| Event | Fields | When |
| --- | --- | --- |
| `ThingSpawned` | `thing`, `def`, `cell` | a thing appears on the map (made, or put down) |
| `ThingDespawned` | `thing`, `def` | a thing leaves the map but still exists (picked up) |
| `ThingDestroyed` | `thing`, `def` | a thing stops existing; comes after `ThingDespawned` if it was on the map |
| `CommandFailed` | `command`, `message` | a command couldn't be applied |
| `PawnAction` | `thing` (the pawn), `def`, `action`, `target` | a pawn did something Unreal animates: `PickUp`, `PutDown` (U5, [pawns.md](pawns.md)) |
| `PawnInjured` | `thing`, `def` (the injury), `part`, `amount`, `cell` | a pawn got an injury (F1) |
| `PawnPartLost` | `thing`, `def` (MissingBodyPart), `part`, `amount`, `cell` | a pawn lost a body part |
| `PawnDowned`, `PawnGotUp` | `thing`, `cell` | a pawn can no longer act, or can again |
| `PawnDied` | `thing`, `message` (the cause), `cell` | a pawn died; it stays on the map with `dead` set |
| `JobStarted` | `thing` (the pawn), `def`, `job`, `target`, `cell` | a pawn took a job ([jobs.md](jobs.md)) |
| `JobEnded` | the same, and `message`: "done", "failed: why" or "interrupted: why" | a pawn's job ended |

`Things` reports its own spawns and despawns once the sim points it at the event list
(`Things::setEventLog`). Later systems add their events (shot fired, wound added, job started,
letters) here. Events say what changed; positions and states are in the snapshot.

## Snapshot

`Sim::writeSnapshot(out)` fills a `Snapshot` (reusing its memory): the game time, the map size
and a `ThingView` per thing on the map, sorted by id (`Snapshot::find` is a binary search).
A `ThingView` has the id, def, category, position (metres on a level), rotation and footprint
size for buildings, stack count for items, and hit points. Pawns with a body also have `downed` and
`dead` (F1); the rest of a pawn's health is read with `Sim::healthReport`. Things off the map (a carried item) are
not in it. Since S4 it also has the ground height under the thing (`z`), a pawn's velocity (`vx`,
`vy`, metres per step) and the route still ahead (`waypointCount` points from
`Snapshot::waypoints[firstWaypoint]`), see [movement.md](movement.md). Later tasks add what they draw
and show: pawn info, the work tab, room stats.

Unreal reads only snapshots and events, never the registry or `Things.h`. That lets the sim move to
its own thread later: it will write snapshots into a buffer that Unreal swaps, with no change to
what Unreal sees.

## State hash and checkpoints

`Sim::stateHash()` is a 64-bit hash of the whole game state: time, the map's ground, the random
stream, the hauling test's lists and stats, the stockpile zones, every entity id the registry has handed out
(including the free list, which decides the next ids), and every component of every thing. Floats
are hashed by their bits. Same seed and commands give the same hash; anything else almost surely
doesn't.

It must stay cheap, because checkpoints run inside a step and S3 holds every step under about
3 ms on a 1 km map. Hashing a 1 km map's cells takes about 10 ms, so the ground (fixed after
generation) is hashed once when the sim starts, and the building layer is covered by the
buildings' footprints. A task that makes map cells change in play (mining, plants, roofs, terrain)
keeps a running hash of its layer, updated when a cell changes, and adds that.

**Every new piece of sim state must go into `stateHash()`** (and, after S6, into the save). A
missing piece makes replays look fine when they aren't.

Every `SimSettings::checkpointEvery` game seconds (default one game hour, 0 for never) the sim
records `Checkpoint{step, hash}` after the step. A replay carries them, so a run that went
differently says from roughly when.

**Regions are not hashed.** They are a cache rebuilt from the map's passage layer, and they
rebuild lazily on the first query after a change, so their ids depend on when something asked
(Unreal asking for a tooltip is enough). Nothing that changes game state may depend on region
ids or on the order regions were built in (S3: break ties by cell, not by region id). If it
does, the replay checks catch it as a mismatch.

`DefDatabase::contentHash()` fingerprints the data files (every def after inheritance and tuning,
plus the tuning values), so a replay can tell it is being played with different data.

## Replays

A `Replay` is the settings, the command log and the hashes. `recordReplay(sim)` makes one from a
running sim (queued commands not yet applied are left out); `playReplay(replay, sim, endStep)`
plays one into a fresh sim made from `replay.settings` (`ReplayFeeder` does the same one step at
a time); `checkReplay(replay, sim)` compares the hashes. Same build plus same data files gives the
same game, and CI checks that Linux (GCC) and Windows (MSVC) agree too.

Files are JSON text, one command and one checkpoint per line:

```json
{
  "format": "rim3d-replay",
  "version": 1,
  "simVersion": "0.1.0",
  "defsHash": "49e4da1eea3c345a",
  "settings": {"seed":42,"mapSize":128,"pawns":10,"items":80,"flatMap":false,"pawnDef":"Human","haulItemDef":"Steel","checkpointEvery":3600,"pathBudget":3000},
  "endStep": 86400,
  "endHash": "4abdc6ed43f4b211",
  "commands": [
    {"step":0,"type":"SpawnThing","def":"Wall","cell":[60,60,0],"count":1,"rot":"North"},
    {"step":3000,"type":"DestroyThing","thing":91}
  ],
  "checkpoints": [
    {"step": 3600, "hash": "c88a5ae35fa76262"}
  ]
}
```

Only `format`, `version` and `settings` are required; missing settings take their defaults, and
`count` and `rot` default to 1 and North. A file without hashes is a **scripted scenario**:
`Tests/replays/haul-and-build.json` is one. `version` changes when the format does; a new
`SimSettings` field must be added to `writeReplay` and `readReplay`.

**simrun:**

```
simrun --replay FILE                   plays it to its endStep (--days overrides), compares hashes
simrun --replay FILE --record OUT      also writes what this run did, with fresh hashes
simrun --seed 42 --days 1 --record OUT records a run with no commands
```

The JSON line it prints gains `commands`, `commandsFailed`, `events`, `stateHash`, `replay`
(`none`, `unchecked`, `matched` or `mismatch`), `hashesCompared` and `firstMismatch`. A mismatch
exits with code 3 and names the game second. A warning is printed if the data files or sim
version differ from the recording.

**Tests and CI.** ctest runs `replay_record` (plays the scenario with the fixture defs in
`Tests/data/`, records `build/haul-and-build.recorded.json`) and `replay_check` (plays the
recording and checks every hash). CI uploads the recording from Linux and Windows and
`tools/compare_replays.py` checks they match hash for hash.

**Bug reports from sneed's PC.** F7 in the game (U2) writes `recordReplay` to
`Saved/Replays/`; F8 (U12) writes a whole bug report folder (replay, save, readable dump, notes; see
`saves.md`). The file plus the commit it was made on reproduces the session in the cloud
with `simrun --replay`; `firstMismatch` and the checkpoints narrow down when things went wrong.

## Unreal side (task U2)

Code: `game/Source/Rim3D/SimSubsystem.h/.cpp` (`USimSubsystem`). Smoke test: `-Test sim`
(`SmokeTest.cpp`, `BuildSimPhases`).

- **Starting.** `StartSim(FSimStartOptions)` loads `<repo>/data` (`DefDatabase::loadDirectory`;
  `-SimData=<folder>` picks another), makes a `Sim` with those defs and takes the opening snapshot.
  Command-line options: `-SimSeed=`, `-SimMapSize=`, `-SimPawns=`, `-SimItems=`, `-SimFlat`. The
  default start is seed 42, a 256 m map, no pawns. A broken data folder is logged and the sim starts
  with no defs; a missing one is a warning. Packaged builds will need `data/` staged next to the
  game (a task for the packaging step, G1 or G2).
- **Stepping.** `Tick` adds `DeltaTime x 30 x speed` to an accumulator and runs whole steps one at a
  time (`step()`, never `stepMany`), at most 90 steps after a hitch. Speed 0 pauses. After each
  step `events()` is read and every event goes to `OnSimEvent`; a `CommandFailed` is also logged and
  shown in red on screen. Events from setting the map up (before the first step) are not sent:
  read the opening snapshot instead.
- **Snapshots and blending.** Only the last two steps of a frame are copied into `Previous` and
  `Current` (`GetPreviousSnapshot`, `GetCurrentSnapshot`). `GetBlendAlpha()` is the accumulator's
  leftover (0 to 1), so what is drawn lags the sim by less than one step but never jumps.
  `GetBlendedLocation(entity, out)` blends a thing's position between the two and returns Unreal
  centimetres; a thing that appeared this step is drawn where it is. Pausing sets alpha to 1.
- **Units.** `SimToWorld(x, y, level)` is the one place sim metres become Unreal centimetres: x and y
  times 100, Z from the ground height at that point (interpolated between cell corners, like the
  terrain mesh's vertices) plus 3 m per level, plus `WorldOrigin` (zero in the game; the smoke
  tests move it away from the sample level).
- **Commands.** `Submit(command)` queues one for the start of the next step. Nothing happens
  while paused until the next step (the period key steps once).
- **Keys** (a Slate input pre-processor, so no player controller is needed): Space pauses and
  resumes at the earlier speed, 1 / 2 / 3 set the speed, `.` steps once while paused, F7 saves a
  replay to `game/Saved/Replays/replay-<time>-day<N>.json`. Keys are ignored while the console is
  open. Console: `Sim.Speed <0-3>`, `Sim.Spawn <Def> <x> <y> [count]`, `Sim.SaveReplay`,
  `Sim.ShowTimings 0|1` (on by default), `Sim.DebugDraw 0|1` (coloured shapes at blended positions,
  until U4 and U5 draw the real things).
- **On-screen timings** (top left): game time and speed, things on the map, events so far, the
  sim's cost per frame (average and worst over about a second, red above the 5 ms budget), steps per
  frame, cost per step and per snapshot.
- Entity ids in snapshots and events are `entt::entity` (a 32-bit number with a version); they
  stay valid until `ThingDestroyed`.
- A replay file made in the game reproduces the session in the cloud only with the same commit
  and the same `data/` files; `simrun --replay FILE` says if the data differs.

## Limits for now

- The command log grows for the whole session. Fine for dev commands; when the protagonist sends
  a position every step (vanilla core input) it is about 3 MB per game day, still fine. Saves
  (S6, `saves.md`) keep the log, so a loaded session still records a full replay from step 0.
- Not thread-safe: submit, step and writeSnapshot all run on the game thread through Stage 1.
- Replays from a different build or different data files may not match; they say so instead of
  pretending.
