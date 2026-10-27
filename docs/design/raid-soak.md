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

Both also watch for **stuck fighters**: one that should be walking (`walk` not 0) but gets nowhere,
under half a metre in 150 steps. (A hurt pawn up a steep slope crawls at a few centimetres a step and
still gets somewhere; the first version counted "same cell for 150 steps" and called those stuck.)

Only waiting where cover exists within 10 m of the pawn counts (the rule is "with cover within 10 m"),
and only cover the pawn may stand on: a drafted colonist keeps within its leash (10 m) of where it was
drafted, so cover beyond that is no use to it and doesn't count. A raid that finds no way in from any
edge (an island start) or a start with no room is skipped and reported as `raidsNoStartRoom`.

## Rules checked (the run fails if one breaks)

| Rule | Fails the run at |
|---|---|
| Under 10% of the waiting in the open | 10% or more |
| No stuck fighter | more than one seed in 20 has one |
| Every raid arrives and ends (or the colony is down) inside the quarter day | any raid that never came, or more than one seed in 20 still on |

The first measurement (40 seeds from 1000) was 13.6% in the open, 18 stuck fighters and 4 of 37 raids
still on, so the limits started loose (20%, one stuck per seed, a quarter of the raids). The fix task
(below) brought them to the limits above: over 80 seeds from each of 1000, 2000 and 3000 the open share
is 6 to 9%, stuck fighters 1 to 2, and raids still on 0 to 4. Zero is the aim; the few that remain are
two sides of healthy pawns trading shots from cover with poor accuracy (a tuning matter for the
shooting numbers) and the odd pair of pawns jamming in a narrow place.

Deaths and fight length are only reported: downed colonists often bleed out, so they are numbers to watch, not
rules.

**With medics** (F10, [field-care.md](field-care.md)): each seed's raid is played a second time with the first
colonist as the alarm's Medic and the second as its Firefighter. The table's last columns and the JSON line
`{"raidSoakMedics":{...}}` give raids ended and still on, colonists dead, tends during the fight, downed colonists
dragged to cover and stuck fighters (not counted in the stuck rule). Only a raid that never came fails the run: with two colonists out of the firing line more raids
hold on past the quarter day.
Deaths and fight length are only reported: they are numbers to watch, not rules.

## What the first run found, and the fixes (F7 fix task)

- **"Stuck" fighters were crawling.** Hurt pawns (a Moving capacity of 20 to 40%) on steep ground (slope
  0.3 to 0.8 over a metre, the slope table gives 17 to 50% speed) walked to spots at 1 to 2 cm a step.
  The spot score counted the walk as straight metres. Now the best spots (three times `refine`) are scored by the
  real effort of the walk (`walkEffort`: ground and slope) divided by the pawn's Moving capacity
  (`minWalkShare` keeps that at most four times), so a hurt or uphill pawn picks a spot close by.
- **Spot hopping in a crowd.** A pawn held up by friends in a narrow place re-planned every half second, took a
  new spot and started a new walk, which cleared the movement system's held-up count, so it never walked
  through the others or gave up. A walking fighter held up for 5 steps now keeps its spot unless a friend
  has taken it, and a pawn on its way gets `walkStayBonus` on top of `stayBonus` for its current spot.
- **Waiting at the edge of range.** A gunner whose spot was 25.02 m from a target in a 25 m gun's range
  planned "can fire" and then never fired (the check when firing uses where it really stands), for
  thousands of steps. A fire spot now has to be `rangeMargin` (1 m) inside the range.
- **Waiting in the open: mostly the measurement.** Cover beyond a drafted pawn's leash was counted as cover it
  could have used; with that fixed the old behaviour was 6.5% (13.6% before). The plan changes above
  moved it to 6 to 9%.
- **Raiders stuck behind a cliff.** An advancing raider that knew the colony but had no spot within 10 m
  that was closer sat for good at the foot of a slope (49 m off). Now an advancing fighter with a goal,
  no spot it can shoot from and its target out of range follows its squad's route to the goal.
- **Raids that never end.** Every pawn hurt (pain 40 to 60%) but none downed: the squad only counted
  downed, dead or gone pawns for its rout, so both sides traded shots until the give-up time. The wounded
  now count as part lost: the squad runs when its members' fitness (1 - pain, times Moving) adds up to
  under half of what it came with (`raids.md`).

## Output

A table per seed and one JSON line `{"raidSoak":{...}}` with the totals. The nightly run (03:17 UTC,
or by hand from the Actions tab with a seed count) plays 40 seeds from 1000 and puts the output
in the run's summary and in the `raid-soak` artifact.
