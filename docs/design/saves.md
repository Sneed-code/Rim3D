# Saves

Code: `ColonySim/Save.h`, `Private/Save.cpp`, `Private/HaulJob.h` (the hauling placeholder's components,
moved out of `Sim.cpp` so the save can see them). Tests: `Tests/SaveTests.cpp` and the `save_*` ctest
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
| Every thing | its id, its def **by `defName`**, and every component (position, footprint, stack, hit points, mover and its path, hauling job, health) |
| Free entity ids | in the order the registry reuses them, so a loaded sim hands out the same ids as the saved one |
| Command log | every command applied so far, failed ones too |
| Queued commands | submitted but not yet applied; they apply at the first step after loading |
| Checkpoints | the state hashes recorded so far |
| Hauling placeholder | stockpile cell, hauler list, counters, setup errors |
| Pawn movement (format 3) | each pawn's velocity, line start, held-up counters (`Mover`), and the repath count |
| Colonists (format 4) | each colonist's name, age, backstories, traits, skills and the scenario name in the settings ([colonists.md](colonists.md)) |
| Pawn health (format 5) | per pawn: protagonist flag, rest, injuries (part, hediff **by `defName`**, severity, tend quality), lost parts, blood loss, dead and cause. Pain, bleeding, capacities and downed are worked out again on loading ([health.md](health.md)) |
| Path queue (format 2) | every request with its answer if it has one, the free tickets, the queue order and the work done on the search in hand ([pathfinding.md](pathfinding.md)) |

**Not saved, because it is rebuilt:** the map's ground (generated again from the seed; the file holds
its hash and loading fails if the generator now gives a different map), the building layer (put back
from the buildings' footprints), regions, cover and threat maps (caches, see `regions.md` and
`cover.md`), the pathfinder's scratch buffers and counters (a half-done search is redone on load
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
format version        u32, little-endian
data                  see below
checksum              u64, over every byte before it
```

All numbers are little-endian; floats are written as their bits, so the same state gives the same
bytes on every platform. Text is a u32 length and the bytes. A list is a u32 count and the elements.
An optional value is a byte (0 or 1) and then the value if present. The layout is the order of the
`saveXxx` functions in `Save.cpp`; the JSON dump uses the same functions, so it shows exactly what the
binary file holds, with field names.

CI saves the same run on Linux (GCC) and Windows (MSVC) and compares the two files byte for byte
(`cmp`), next to the replay check. So a save made on sneed's PC loads in the cloud.

**Loading says no, with a plain reason, when:** the file isn't a save, is cut short or damaged (the
checksum catches a changed byte anywhere), was made by a newer format, names a `Things` def the data
files no longer have, or the map generator changed. It **warns** and carries on when the data files or
the sim version differ from the ones the save was made with.

## Adding to the save (every task that adds sim state)

New sim state has to go in the save, the state hash and the round-trip test. Missing one of the
three is a bug the tests are built to catch:

1. **A new component on things**: add an `std::optional<YourComponent>` to `SaveEntity`, read it in
   `SimSaveAccess::collect`, write its fields in `saveEntity`, and `emplace` it in
   `SimSaveAccess::restore`. Add `ar.require(...)` checks for values that must be in range (enums,
   sizes), so a damaged file is refused and doesn't crash. Add it to `Sim::stateHash`.
2. **New sim-level state** (a new system's counters, queues, tables): a field in `SaveData`, saved in
   `saveData`, filled in `collect` and put back in `restore`. Hash it in `stateHash`.
3. **A new map layer that changes during play** (mining, plants, roofs): the ground is regenerated
   from the seed, so a layer that differs from generation must be saved as its own list in
   `SaveData` and restored onto the map. Keep a running hash for it (see `stateHash`).
4. **A new command**: write it in `saveCommand` (the `static_assert` on the variant size stops the
   build until you do).
5. **Entity references** inside components (like a job's target) are saved as the id; ids come back
   exactly, so they stay valid. `restore` checks that what they name exists.
6. **Test**: add the new state to the session in `Tests/SaveTests.cpp` (`saveTestSession`) so the
   "load and run on gives the same game" test exercises it. A cache needs no save, but it must be
   rebuilt by whatever asks for it, and nothing that changes game state may read it stale.
7. **Changing the layout of anything already saved**: bump `kSaveFormatVersion`, keep reading the old
   version (the archive's `version()` says which one the file is: `if (ar.version() >= 2) ...`), and
   write the change here. Old saves keep loading until a decision says otherwise.

Never store a `DefId` (or any number that depends on load order) in a save: use the `defName`.
The same goes for region ids, which a loaded map numbers afresh: keep a cell, and never let a
region id decide anything (the pathfinder breaks ties by a region's anchor cell for this reason).

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
- Quicksave, quickload and autosave keys belong to U12 (Unreal side).
