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

So far there are only two dev commands, like RimWorld's dev mode. The real ones (place blueprint,
designate, work priorities, draft, protagonist moved and actions) come with the tasks that build
those systems.

| Command | Does | Fails when |
| --- | --- | --- |
| `SpawnThingCommand{def, cell, count, rot}` | `Things::spawn` of a Things def | unknown def; the thing can't go there |
| `DestroyThingCommand{thing}` | `Things::destroy` | the thing doesn't exist; the hauling placeholder is using it |

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

`Things` reports its own spawns and despawns once the sim points it at the event list
(`Things::setEventLog`). Later systems add their events (shot fired, wound added, job started,
letters) here. Events say what changed; positions and states are in the snapshot.

## Snapshot

`Sim::writeSnapshot(out)` fills a `Snapshot` (reusing its memory): the game time, the map size
and a `ThingView` per thing on the map, sorted by id (`Snapshot::find` is a binary search).
A `ThingView` has the id, def, category, position (metres on a level), rotation and footprint
size for buildings, stack count for items, and hit points. Things off the map (a carried item) are
not in it. Later tasks add what they draw and show: pawn velocity and path for motion matching
(S4, U5), pawn info, the work tab, room stats.

Unreal reads only snapshots and events, never the registry or `Things.h`. That lets the sim move to
its own thread later: it will write snapshots into a buffer that Unreal swaps, with no change to
what Unreal sees.

## State hash and checkpoints

`Sim::stateHash()` is a 64-bit hash of the whole game state: time, the map's ground, the random
stream, the hauling placeholder's lists and stats, every entity id the registry has handed out
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
  "settings": {"seed":42,"mapSize":128,"pawns":10,"items":80,"flatMap":false,"pawnDef":"Human","haulItemDef":"Steel","checkpointEvery":3600},
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

**Bug reports from sneed's PC.** A dev key in Unreal (U2 or later) writes `recordReplay` to
`Saved/Replays/`. The file plus the commit it was made on reproduces the session in the cloud
with `simrun --replay`; `firstMismatch` and the checkpoints narrow down when things went wrong.

## Unreal side (task U2)

- Step one at a time (`step()`, not `stepMany`) and handle `events()` after each step.
- Keep two snapshots, from the last two steps, and blend positions by how far the frame is into
  the next step (`StepAccumulator`).
- Commands from input go through `submit` between steps; show `CommandFailed` messages.
- The bridge converts units (sim metres to Unreal centimetres, Z up); nothing else does.
- Entity ids in snapshots and events are `entt::entity` (a 32-bit number with a version); they
  stay valid until `ThingDestroyed`.

## Limits for now

- The command log grows for the whole session. Fine for dev commands; when the protagonist sends
  a position every step (vanilla core input) it is about 3 MB per game day, still fine. Saves
  (S6, `saves.md`) keep the log, so a loaded session still records a full replay from step 0.
- Not thread-safe: submit, step and writeSnapshot all run on the game thread through Stage 1.
- Replays from a different build or different data files may not match; they say so instead of
  pretending.
