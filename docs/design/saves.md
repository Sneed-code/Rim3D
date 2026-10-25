# Saves

Code: `ColonySim/Save.h`, `Private/Save.cpp`. Tests: `Tests/SaveTests.cpp` and the `save_*` ctest
runs. Task S6.

A save holds the whole game state in one file. Load it, run on, and the game is the same, step for
step, as if you had never saved.

```cpp
std::vector<uint8_t> bytes = saveSim(sim);                  // Save.h
SaveLoadResult r = loadSim(bytes, defs);                     // r.sim, r.error, r.warnings
std::string json = saveToJson(sim);                          // for reading, not for loading
```

`simrun` has the same three: `--save FILE` writes a save after the run, `--load FILE` continues from
one (`--days` then means days to run after loading, default 0), `--save-json FILE` writes the
readable dump. `simrun --load a.save --days 0 --save-json a.json` turns any save into text.

## What is in a save

| Saved | Notes |
| --- | --- |
| Settings | seed, map size, hauling-test settings, checkpoint interval |
| Clock and random stream | game second, the `Rng` state and stream |
| Every thing | its id, its def **by `defName`**, and every component (position, footprint, stack, hit points, mover and its path, worker with its job and avoid memory, Work tab row by work type `defName`, what it carries, health) |
| Free entity ids | in the order the registry reuses them, so a loaded sim hands out the same ids as the saved one |
| Command log | every command applied so far, failed ones too |
| Queued commands | submitted but not yet applied; they apply at the first step after loading |
| Checkpoints | the state hashes recorded so far |
| Hauling test | its stockpile's middle cell, hauler list, counters, setup errors |
| Bills and the bill job (format 12) | per work building: `nextId` and each bill's id, recipe **by `defName`**, mode, count, suspended; per job: `bill`, `picks` (stack entity and count) and `used` (ingredient **`defName`** and count) ([bills.md](bills.md)) |
| Building plans, marks and a job's site (format 10) | per plan (a thing with a `site`): stage, stuff **by `defName`**, origin, rotation, size, each material by `defName` with how many are needed and delivered, work total and done; per building: what it was `builtOf`, and `marked` for deconstruction; per job: its `site` and `progress` ([construction.md](construction.md)). The cell index of plans is rebuilt on loading |
| Stockpile zones (format 6) | each zone's id, priority, filter by `defName` and cells, and the next zone id |
| Pawn movement (format 3) | each pawn's velocity, line start, held-up counters (`Mover`), and the repath count |
| Colonists (format 4) | each colonist's name, age, backstories, traits, skills and the scenario name in the settings ([colonists.md](colonists.md)) |
| Needs, schedules and beds (format 8) | per pawn: food, rest, malnutrition, the next food search, the 24 schedule hours; per bed: its owners; per job: the chair it eats at ([needs.md](needs.md)) |
| Doors (format 13) | per door: opening progress, steps before it closes, open. Rooms and roofs are rebuilt from the walls ([rooms.md](rooms.md)) |
| Mood (format 11) | per pawn: level, break band, memories (thought **by `defName`**, fade time) ([mood.md](mood.md)) |
| Pawn health (format 5) | per pawn: protagonist flag, rest, injuries (part, hediff **by `defName`**, severity, tend quality), lost parts, blood loss, dead and cause. Pain, bleeding, capacities and downed are worked out again on loading ([health.md](health.md)) |
| Direct control (format 7) | which colonist the player controls (a flag on the pawn); the `ControlPawn` and `DirectMove` commands in the log ([protagonist.md](protagonist.md)) |
| Plants (format 9) | every plant as parallel lists: cell index, def, growth, age; fertility is rebuilt from the seed |
| Fighters (format 15) | per pawn its side if not the player's; for fighters the orders (drafted, anchor, leash, forced target), the plan (target, spot, fire cell and stance, falling back, next plan), the walk and its path ticket, and the enemies it knows (pawn, where and when last seen) ([fighters.md](fighters.md)) |
| Combat (format 12) | per pawn its posture (stance, facing), gear (gun and apparel ids) and trigger (phase, steps, burst, aim, recoil); `EquippedBy` on gear; every projectile in flight with its defs **by `defName`** ([combat.md](combat.md)) |
| Pawn looks (format 16) | each humanlike pawn's lineage (by `defName`), gender, body type, weight, height, melanin and colours; `SetLineage` commands in the log ([looks.md](looks.md)). Older files load with pawns that have no looks |
| Aiming (format 17) | rounds left in each gun that has been fired (`Magazine`), the trigger's reloading phase, a fighter's reaction and dodge times ([aiming.md](aiming.md)) |
| Path queue (format 2) | every request with its answer if it has one, the free tickets, the queue order and the work done on the search in hand ([pathfinding.md](pathfinding.md)) |

**Not saved, because it is rebuilt:** the map's ground (generated again from the seed; the file holds
its hash and loading fails if the generator now gives a different map), the building layer (put back
from the buildings' footprints), regions, cover and threat maps (caches, see `regions.md` and
`cover.md`), the job system's reservations (they follow from the jobs, [jobs.md](jobs.md)), the
items-per-cell index, the pathfinder's scratch buffers and counters (a half-done search is redone on load
from its request and the work it had done), and the event list (a loaded sim's
`events()` is empty; Unreal rebuilds its view from a snapshot).

**The command log stays in the save** so a loaded session can still be recorded as a replay from step
0 (`recordReplay`), which keeps the bug-report key working after a quickload. The cost is the log's
size, about 3 MB per game day once the protagonist sends a position every step; it is binary and
compresses well. If that ever matters, a replay could start from a save plus the log since; nothing
needs it yet.

## File format

```
"RIM3DSAV"            8 bytes
format version        u32, little-endian: 20 since S7, and it stays 20
sections              one after another, to the checksum
checksum              u64, over every byte before it

section:
  name                text, e.g. "fighters"
  version             u32, the section's own version, from 1
  size                u32, bytes of data
  data                that section's fields
```

All numbers are little-endian; floats are written as their bits, so the same state gives the same
bytes on every platform. Text is a u32 length and the bytes. A list is a u32 count and the elements.
An optional value is a byte (0 or 1) and then the value if present. The layout is `saveSections` in
`Save.cpp` and the `saveXxx` functions it calls; the JSON dump uses the same functions, so it shows
exactly what the binary file holds, with field names: one object per section, with its `version`.

**Sections (S7).** Each system keeps its state in a section of its own, with its own version, so
tasks that run in parallel never fight over one format number. Loading finds a section by its name,
whatever its place in the file. A file without a section loads that part empty (how a save from
before the section existed loads); a section this build doesn't know, or of a newer version than it
reads, is refused ("made by a newer version"); a section with more or less data than its layout is
refused as damaged. A thing's parts other than its def, position, footprint, stack and hit points
are kept by the section of the system they belong to, as a list of (thing id, part) in id order.

| Section | Version | Holds |
| --- | --- | --- |
| `sim` | 1 | sim version, data files' hash, settings, clock, random stream, the map's hash, the hauling test |
| `commands` | 1 | the names of the command types used, then queued commands, the command log and the checkpoints |
| `things` | 1 | free ids, then every thing: id, def, position, footprint, stack, hit points |
| `movement` | 1 | `mover` |
| `jobs` | 1 | `worker` (the job, its bill picks and ingredients, avoid memory), `work` (Work tab), `carrying` |
| `colonists` | 1 | `colonist` |
| `health` | 1 | `health` |
| `control` | 1 | `playerControlled`: the ids of controlled pawns |
| `needs` | 1 | `needs`, `schedule`, `bed` |
| `bills` | 1 | `bills` on work buildings |
| `fuel` | 1 | `fuel` on buildings that burn it ([fuel.md](fuel.md)); a building with a fuel part and none saved loads full |
| `construction` | 1 | `site`, `builtOf`, `marked` |
| `mood` | 1 | `mood` |
| `combat` | 1 | `posture`, `gear`, `equippedBy`, `shooter`, `projectile`, `magazine` |
| `doors` | 1 | `door` |
| `fighters` | 1 | `faction`, `fighter` |
| `looks` | 1 | `looks` |
| `paths` | 1 | the path queue |
| `zones` | 1 | next zone id, stockpile zones |
| `plants` | 1 | the plant layer |
| `mining` | 1 | the rock layer and the designations |
| `power` | 1 | `power`: per building with a power part, switched on, powered, battery charge (Wd) and requested draw; nets are made again on loading ([power.md](power.md)) |
| `recreation` | 1 | `joy`: recreation, comfort and outdoors needs, tolerances, and the activity of a recreation job ([recreation.md](recreation.md)) |

**Commands are saved by name (S7).** The `commands` section starts with the names of the command
types it uses (`commandNames`); each command then gives its type as a place in that list, and its
fields. So the order of the types in the `Command` variant doesn't matter to any save or replay, and
two tasks that add commands at once don't clash. `commandFromName` (`Commands.h`) turns a name back
into a command. A name this build doesn't know is refused, saying which.

**Formats 6 to 19** (before S7) were one long layout that each format added to. They still load:
`saveLegacyData` and `saveLegacyEntity` read them with the same field functions (`saveXxxPart`), and
there every section's version counts as 1. Their commands give the type's place in the variant as
it was at format 19; that table in `saveCommand` is frozen and never grows. Saving always writes
format 20. `Tests/saves/` keeps two format 19 files made by the game before S7, and a test loads
them, saves them again and checks the game goes on the same.

CI saves the same run on Linux (GCC) and Windows (MSVC) and compares the two files byte for byte
(`cmp`), next to the replay check. So a save made on sneed's PC loads in the cloud.

**Loading says no, with a plain reason, when:** the file isn't a save, is cut short or damaged (the
checksum catches a changed byte anywhere), was made by a newer format or has a section or command
from a newer version, names a `Things` def the data files no longer have, or the map generator
changed. It **warns** and carries on when the data files or the sim version differ from the ones the
save was made with.

## Adding to the save (every task that adds sim state)

New sim state has to go in the save, the state hash and the round-trip test. Missing one of the
three is a bug the tests are built to catch. **Never bump `kSaveFormatVersion`**: give new state
a section of its own, and bump only your own section's version when you change its layout. Additions
to shared lists (`SaveEntity`, `SaveData`, `collect`, `restore`, `saveSections`) go at the end, so
parallel tasks merge cleanly.

1. **A new system with state**: add a section at the end of `saveSections`:
   `ar.section("fuel", 1, [&] { ... });` with a name nobody uses (add it to the table above). Saves
   from before it have no such section and load it empty, so `restore` must give the defaults a
   game without it would have (a stove that was saved before fuel existed starts full, say).
2. **A new component on things**: add an `std::optional<YourComponent>` at the end of `SaveEntity`,
   fill it in `SimSaveAccess::collect`, write its fields in a `saveYourPart(ar, part)` function, save
   the list in your section with `saveParts(ar, "yourPart", e, &SaveEntity::yourPart, ...)`, and
   `emplace` it in `SimSaveAccess::restore`. Add `ar.require(...)` checks for values that must be in
   range (enums, sizes), so a damaged file is refused and doesn't crash. Add it to `Sim::stateHash`.
3. **New sim-level state** (a new system's counters, queues, tables): a field at the end of
   `SaveData`, saved in your section, filled in `collect` and put back in `restore`. Hash it in
   `stateHash`.
4. **A new map layer that changes during play** (mining, plants, roofs): the ground is regenerated
   from the seed, so a layer that differs from generation must be saved as its own list in your
   section and restored onto the map. Keep a running hash for it (see `stateHash`).
5. **A new command**: write its fields in `saveCommand` (an `else if constexpr` before the last
   `else`, whose `static_assert` stops the build until you do). Its name comes from `commandName`;
   nothing about it goes in the frozen table for formats 6 to 19.
6. **Entity references** inside components (like a job's target) are saved as the id; ids come back
   exactly, so they stay valid. `restore` checks that what they name exists.
7. **Test**: add the new state to the session in `Tests/SaveTests.cpp` (`saveTestSession`) so the
   "load and run on gives the same game" test exercises it, or a save round trip in your own test
   file. A cache needs no save, but it must be rebuilt by whatever asks for it, and nothing that
   changes game state may read it stale.
8. **Changing the layout of a section already saved**: bump that section's version in
   `saveSections` (and in the table above), keep reading the old one (`if (ar.sectionVersion() >= 2)
   ...` around the new fields), and write the change in the section list under "Format versions".
   Old saves keep loading until a decision says otherwise (decision 0005 dropped formats 1 to 5 for
   the job system).

Never store a `DefId` (or any number that depends on load order) in a save: use the `defName`.
The same goes for region ids, which a loaded map numbers afresh: keep a cell, and never let a
region id decide anything (the pathfinder breaks ties by a region's anchor cell for this reason).

## Saving and loading in the game (task U12)

Code: `USimSubsystem` (`SimSubsystem.h/.cpp`). Smoke test: `saves`.

| Key | Console | What it does |
| --- | --- | --- |
| F5 | `Sim.Save [slot]` | Saves to `game/Saved/Saves/<slot>.save` (default slot `quicksave`). |
| F9 | `Sim.Load [slot]` | Replaces the running game with that save. |
| F8 | `Sim.BugReport` | Writes a folder in `game/Saved/BugReports/` (below). |
| F7 | `Sim.SaveReplay` | Just the replay file, in `game/Saved/Replays/` (U2). |

- **Autosave** happens once a game day, at midnight (day 2 starts, day 3 starts...), into
  `autosave-1.save`, `autosave-2.save`, `autosave-3.save` in turn, so a bad autosave never costs the two
  before it. A loaded game does not autosave until the next midnight. `SetAutosaveInterval` shortens it
  for tests; 0 turns it off. A save takes a few milliseconds for a small colony, done on the game thread.
- **A save never replaces a good file with half a file**: it is written beside the old one and swapped in.
- **Loading is all or nothing.** The new sim is made first; only if that works does it replace the
  running one. A missing, damaged or newer-format file, or a def the data no longer has, leaves the game
  running as it was and shows the reason in red on screen and in the log. A different sim version or
  different data files only warn in the log.
- **After a load** the bridge takes a fresh snapshot, so the next frame draws the loaded game. The pawn
  actors notice the sim is a new object and rebuild themselves. Speed and pause stay as they were.
  Anything else that keeps a thing's id between frames (the overseer's selection) drops it when that
  id is no longer in the snapshot.
- **Slot names** are cleaned to letters, digits, `-` and `_`.

**The bug report** (F8) is a folder `game/Saved/BugReports/<date-time>-day<N>/`:

| File | Use |
| --- | --- |
| `replay.json` | `simrun --replay replay.json` in the cloud plays the whole session from step 0 and says if the hashes match |
| `state.save` | `simrun --load state.save --days 1` continues from the exact moment |
| `state.json` | the same save as text, for reading |
| `notes.txt` | sim version, seed, game second, state hash, how to run the other files |
| `unreal.log` | the game's log, copied if it was not locked |

sneed's whole job for a bug is: press F8, then send the folder (zip it, or drop the files into the thread).

## Format versions

- **1** (S6): first version.
- **2** (S3): `pathBudget` in the settings, a hauler's path ticket in its hauling job, and the path
  queue after the entities. Version 1 files load with the default budget and an empty queue.

- **3** (S4): a pawn's `Mover` also has its velocity, where its current line started, the held-up
  counters and `gaveUp`; the hauling job's `WaitPathCarrying` stage; `SimStats::repaths`. Older
  files load with pawns at rest.
- **4** (C8): `scenario` in the settings and a `Colonist` component on a pawn. Older files load
  with no scenario and no colonists.
- **5** (F1): `Health` on pawns (a pawn whose def has `race.body` and whose save has none gets a
  healthy one). Commands 2 and 3 (`DamagePawn`, `TendPawn`).
- **6** (C1): the job system. Things have `worker`, `work` and `carrying` instead of the hauling
  placeholder's components, the zones follow the path queue, and commands 4 to 9 are the work and
  stockpile commands. **Formats 1 to 5 no longer load** ("made before the job system",
  [decision 0005](../decisions/0005-saves-before-jobs.md)): the placeholder's state has no
  counterpart in jobs. The `ar.version() >= 2` to `>= 5` branches stay, so the old layout is still
  written down in one place.
- **7** (U9): a `playerControlled` flag on a thing, and command types 10 (`ControlPawn`) and 11
  (`DirectMove`) in the command log. Older files load with nobody controlled. A file with two
  controlled pawns, or a controlled pawn that isn't a colonist on the map, is refused.
- **8** (C6): needs, schedules, bed owners and a job's chair; commands 12 (`SetSchedule`) and 13
  (`AssignBed`). Older files load with pawns without needs.
- **9** (C2): the plant layer after the zones (`docs/design/plants.md`). Older files load with a bare map.
- **10** (C4): building plans (`site`), `builtOf` and deconstruct marks, a job's site and progress;
  commands 14 to 16 (`PlaceBlueprint`, `CancelBuild`, `Deconstruct`) ([construction.md](construction.md)).
- **11** (C9): mood (level, band, memories by `defName`) ([mood.md](mood.md)). Older files load with
  pawns without a mood.
- **12** (F2): `Posture`, `Gear`, `EquippedBy`, `Shooter` and `Projectile` components (a pawn saved
  without a posture stands facing east). Commands 17 to 19 (`Equip`, `Shoot`, `SetStance`).
- **13** (C5): a `Door` component (progress, hold, open) on doors ([rooms.md](rooms.md)). Older files load with closed doors.
- **14** (C3): the rock layer and the designations after the plants, job types `Mine`, `CutPlant`
  and `HarvestPlant`, commands 20 (`Designate`) and 21 (`CancelDesignations`)
  (`docs/design/mining.md`). Cells of rock in the generated ground with no record in the file were
  mined and load as rough stone. Older files load with the generated rock layer and no marks.
- **15** (F4): `Faction` and `Fighter` components (orders, spot, plan, path ticket, the enemies it
  knows) ([fighters.md](fighters.md)). Commands 22 to 25 (`Draft`, `MoveOrder`, `AttackOrder`,
  `SetFaction`). Older files load with everyone on the player's side and nobody drafted.
- **16** (A3): each humanlike pawn's looks (`docs/design/looks.md`), command 26 (`SetLineage`). Older files load with pawns that have no looks.
- **17** (F5): a `Magazine` component on fired guns, the `Reloading` trigger phase, a fighter's
  `reactUntil` and `dodgeUntil` ([aiming.md](aiming.md)). Command 27 (`Reload`). Older files load
  with every gun full and no fighter waiting to react.
- **18** (C7): a building's bills and a bill job's bill, picks and used ingredients; commands 28 to
  30 (`AddBill`, `RemoveBill`, `SetBill`) ([bills.md](bills.md)).
- **19** (U10): a job's `handsOn` state; command 31 (`HandsOn`) ([handson.md](handson.md)).
- **20** (S7): named sections, each with its own version, and commands by name (above). The last
  format number: from here on, sections change instead.

### Section versions

Every section is version 1 at format 20. A task that changes a section adds a line here, e.g.
"`fighters` 2 (F9): ...". New sections are listed in the table above.

- `roomstats` 1 (C17): floors, filth, carried filth, the filth rng, beauty needs ([room-stats.md](room-stats.md)). Commands `PlaceFloor` and `AddFilth` by name.

## Why the state hash changed in S6

`Sim::stateHash` used to hash live things in the order the entity registry happened to hold them,
which depends on the history of creating and destroying things. A loaded save can't repeat that
history, so the hash now lists the free ids in reuse order and then every live thing in id order.
Same game, same hash; only recordings made before S6 have different hash values.
(`Tests/replays/haul-and-build.json` has no hashes, so it is unaffected.)

## Limits for now

- **Order inside the registry is not saved.** Which thing a `view` visits first can differ after a
  load. The rule in `things.md` (break ties by entity id, or keep your own ordered list) is what makes
  that safe; the round-trip test is what catches a system that breaks it.
- Only ground level (0) is stored, like the rest of the sim.
- Saves are not compressed. The map ground is not in the file, so a save is mostly things (about 200
  bytes each; a hauling run with 10 pawns and 100 items is 21 KB) plus the command log.
- Quicksave, quickload, autosave and the bug report key are in the game since U12 (next section).
