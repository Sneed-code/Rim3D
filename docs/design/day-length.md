# Day length

sneed (2026-11-16): the length of a game day is a player option, a slider from **48 real minutes**
(the default, as before) up to **2 hours** at 1x. It is a new-game setting and can be changed in
play. A longer day must not slow walking, fighting or work: only the things counted by the day.

## Two clocks

One sim step is still 1/30 of a real second at 1x (`kStepsPerRealSecondAt1x`), so anything done
per step keeps its real-time speed. The sim now keeps two clocks in `GameTime`:

- `steps`: one per step, always. Real time.
- `seconds`: the calendar (the clock on screen, the day, the hour, the season). With the 48-minute
  day it moves one game second every step, as before. With a day of `dayMinutes` minutes it moves
  on 48 of every `dayMinutes` steps (2 of every 5 at 2 hours): `Sim::calendarAcc_` adds 48 each
  step and the calendar moves on when it reaches `dayMinutes`. `GameTime::tick` says whether this
  step moves it.

With the 48-minute day the two clocks are equal and every step ticks, so the game plays exactly as
before (same state hashes, same saves, same replays).

## What follows which clock

**Calendar systems run only on steps that tick**, with their numbers unchanged (per game second,
hour or day): health (bleeding, healing, disease, immunity), needs (food, rest, joy, comfort...),
fuel burning, caravans and pods on the world map, raids' and sieges' plans, prisoners, traders,
quests, the ship's countdown, recreation tolerances, drugs, animals (hunger, growth, sleep, wild
animals coming and going, their fights' timers), husbandry, breeding, social talk and lovin,
mental breaks, mood, inspirations, power and batteries, filth, room stats, temperature, rot, bills
upkeep, weather, plant and crop growth, skills' daily decay, the narrator and disasters, and the
checkpoints replays compare.

**Real-time systems run every step** on the step clock: jobs (work speed per step: building,
mining, crafting, tending, cutting plants), field care, fighters (aim, reaction, dodges, plans),
psycasts' strain, movement, melee (swings, blocks), combat (shots, projectiles, stagger),
explosives, traps and turrets, artillery, fire (spread, burning pawns), loot drops, the path queue,
rooms, roof collapse and lights. Fire uses the step clock so a fire spreads as fast in real time
whatever the day.

Mixed cases:

- **Jobs** run every step, but their timers are calendar ones (sleep 8 game hours, eat, a
  recreation activity's hours, lovin), so these last more steps with a longer day, which is right.
  Joy gained from recreation and parties is a rate per game hour, so it is added only on ticking
  steps.
- **Psycast focus** fades per game day (ticking steps only); strain recovers per step.
- **Apparel** runs every step (putting armour on takes steps); hourly wear waits for a tick.
- **Monsters** run every step (a destroyed mound drops jelly at once); their mound checks wait for
  a tick.
- **Fast travel** takes the walk's steps; the calendar that passes meanwhile is the steps times 48
  / `dayMinutes`. **Night skip** runs `nightBatch × dayMinutes / 48` steps a batch, so a skipped
  night takes the same real time whatever the day length.
- **Other maps** (caravan visits) are stepped every step and told the colony's tick, so both keep
  the same time.

Speed-up (2x, 3x, overseer only) multiplies steps as before: both clocks speed up together.

## Commands, saves, replays

- `SimSettings::dayMinutes` (48 to 120, clamped): the new-game setting.
- `SetDayLengthCommand{minutes}` changes it in play; it fails outside 48 to 120.
- The save's new section `clock` (version 1) holds `steps`, `dayMinutes` and `calendarAcc`. A save
  from before it loads with steps equal to seconds and the 48-minute day.
- Replays and the command log now count by step (`LoggedCommand::step`, `Checkpoint::step`,
  `Replay::endStep` are `steps`); equal to the game second with the 48-minute day. A replay names
  `dayMinutes` in its settings when it isn't 48.
- The state hash takes in the step clock and day length only when they differ from the 48-minute
  day's, so hashes recorded before stay valid.
- The snapshot has `steps` and `dayMinutes` beside `gameSeconds`. Unreal should use `steps`, not
  `gameSeconds`, to tell that a step has run.

## Not done yet

- Unreal: the slider in the new-game screen and the in-game options, and switching the few
  "has a step run?" checks (shooting once a step, overlays) from `gameSeconds` to `steps`.
- Some job and animal timers in game seconds that are really real-time waits (a hunter's repath,
  a wild animal's chase, waiting a minute before looking for food again) now last longer in real
  time with a longer day. None changes the outcome much; move them to steps if playtests show it.
- Days shorter than 48 minutes would need more than one calendar second per step; not offered.

Tests: `Tests/DayLengthTests.cpp`.
