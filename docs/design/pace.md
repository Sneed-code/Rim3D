# Pace: day length, game speed and how fast life grows

sneed, 2026-11-16 (thread "Set pawn-mode pacing and fast crops").

## Game speed

- **Playing as a pawn** (direct control): the game runs at **1x or paused only**. The speed keys and
  buttons are capped at 1x while a pawn is steered (`ProtagonistSubsystem` calls `SetMaxSpeed(1)`).
  The game should be worth playing at normal speed; fast travel and night skip come later.
- **Overseer view**: 1x, 2x and 3x as before.

## Day length

One sim step is one game second; at 1x the sim runs `kStepsPerRealSecondAt1x` (30) steps a real
second, so a game day is **48 real minutes** (`Clock.h`). sneed's choice: 48 minutes by default, with a **day-length option from 48 minutes to 2 hours**
(built: `docs/design/day-length.md`). Walking, fighting and work stay real time; needs, crops, rot,
seasons and the sun follow the calendar, so the life pace below compresses or stretches with it.

## Life pace (`pace.life`)

One value in `data/tuning.json`, **`pace.life` (2.75)**, speeds up every slow living clock at once:

| What | Where | With 2.75 |
| --- | --- | --- |
| Plant growth (crops, wild plants, trees) | `Plants`, times `plantGrowth` | rice 2.0 days, potatoes 3.9, strawberries 3.1, haygrass 4.7, cotton 5.4, corn 7.6 |
| Plant lifespans | `Plants` | shortened the same, so a plant's life keeps its shape |
| Animals growing up (life stages) and life expectancy | `AnimalSystem` | a hen is grown in about 2 days instead of 6, a cow in 13 instead of 36 |
| Pregnancy (`gestationDays`) | `Breeding` | a cow 4.4 days instead of 12 |
| Egg hatching (`hatches.days`) | `Breeding` | divided by 2.75 |
| Rest between births (`birthCooldownDays`) | `Breeding` | divided by 2.75 |

Not changed by it: egg laying, milk and wool per day (they are matched to what an animal eats per
day), needs, work speeds, rot, seasons and the day length. The days above count growing in good soil
in the 13 growing hours of a day (`plant.growDays x 24 / 13 / pace`); poor soil, cold and darkness
slow it as before.

The number 2.75 puts rice, the fastest staple, at 2 days and potatoes at 4 (sneed: "crops 2-4 days").
Raise it to make everything living faster, lower it for slower; 1 is RimWorld. Changing the day
length later compresses all of it with the day, as it is counted in game days.

Tests: `PaceTests.cpp`.
