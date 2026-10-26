# Raid soak (F7)

Code: `game/Plugins/ColonySim/Tools/simrun/RaidSoak.cpp`; workflow `.github/workflows/nightly.yml`;
CTest `simrun_raid_soak` (3 seeds, part of every CI run).

`simrun --seed S --raid-soak N` plays two scenarios on each of the seeds S to S+N-1:

1. **Firefight in cover**: three colonists (drafted) against three hostile pawns with rifles and
   a revolver, on a flat 96 m field with six short lines of low cover placed by the seed (tables
   stand in for sandbags, which have no def yet). 2400 steps or until one side is down.
   Measured: pawn-steps a colonist waits on its spot with an enemy known and not shooting, and
   how many of those are in the open from the nearest enemy (`coverAgainst` under 0.5 crouched).
2. **Raid**: 300 points of pirates against the six armed Crashlanded colonists, a quarter of a
   game day. Measured: did it come, did it end, how long the fight ran, who was downed or died.

Both also watch for **stuck fighters**: one that should be walking (`walk` not 0) but stays on one
cell for 150 steps.

Only waiting where cover exists within 10 m of the pawn counts (the rule is "with cover within 10 m").
A raid that finds no way in from any edge (an island start) or a start with no room is skipped and
reported as `raidsNoStartRoom`.

## Rules checked (the run fails if one breaks)

The design rules are on the left; the run fails at the limit on the right, because the first
measurement (40 seeds from 1000) was worse than the rule and a tuning task has to close the gap.

| Rule | Limit that fails the run | First measurement |
|---|---|---|
| Under 10% of the waiting in the open | 20% | 13.6% |
| No stuck fighter | more than one per seed | 18 in 40 seeds, all in raids |
| Every raid arrives and ends inside the quarter day | none missing, at most a quarter still on | 4 of 37 still on |

Deaths and fight length are only reported: downed colonists often bleed out, so they are numbers to watch, not
rules.

**With medics** (F10, [field-care.md](field-care.md)): each seed's raid is played a second time with the first
colonist as the alarm's Medic and the second as its Firefighter. The table's last columns and the JSON line
`{"raidSoakMedics":{...}}` give raids ended and still on, colonists dead, tends during the fight, downed colonists
dragged to cover and stuck fighters (not counted in the stuck rule). Only a raid that never came fails the run: with two colonists out of the firing line more raids
hold on past the quarter day.

## Found while writing it

- A raid with no chosen edge gave up when its random edge had no way in; it now tries the other three
  (`Raids.cpp`).
- Stuck fighters: a fighter with `walk` 2 (walking to its spot), nothing shooting, enemies known, that
  stays on one cell for 150 steps. Not yet explained; a fix task should look at `mover.arrived()` and
  `gaveUp` in `Fighters.cpp`.
- Fighters wait in the open 13.6% of the time; the nearest-enemy changes under them are the likely cause.

## Output

A table per seed and one JSON line `{"raidSoak":{...}}` with the totals. The nightly run (03:17 UTC,
or by hand from the Actions tab with a seed count) plays 40 seeds from 1000 and puts the output
in the run's summary and in the `raid-soak` artifact.
